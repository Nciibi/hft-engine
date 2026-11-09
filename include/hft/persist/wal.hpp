// Write-ahead log + snapshot + idempotent restart.
//
// The problem: a restart loses all state. A real system needs a WAL
// (every applied frame, in order) and a snapshot cadence (book state
// at sequence N), so recovery is snapshot(N) + WAL tail. This header
// is storage-agnostic on purpose: the log is a byte vector the
// caller persists however it likes (mmap, fsync'd file, object
// store), which keeps the format testable without touching disk.
//
// Record format (all big-endian, fixed order):
//   magic     4 bytes  "HWAL"
//   seq       8 bytes  global applied-message sequence number
//   len       2 bytes  frame length INCLUDING the 2-byte prefix
//   frame     len bytes (the raw ITCH frame, prefix + body)
//   checksum  8 bytes  FNV-1a over magic..frame (detects torn writes
//                       and bit flips; NOT cryptographic)
//
// Idempotency: a reader tracks the last applied seq. A record whose
// seq <= last_applied is a duplicate (replayed after a crash between
// persist and ack) and is SKIPPED, not re-applied -- re-applying an
// Add Order would double it. A seq jump is a gap: reported, never
// silently continued (same policy as SequenceTracker).
//
// Snapshot: book orders in dump order (best-first per side, queue
// order within a level) + FNV checksum over the same fold replay.cpp
// uses. Restore re-adds orders in snapshot order, which reproduces
// queue order because add() appends to the level tail. The restored
// checksum must equal the snapshot checksum or recovery fails.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "hft/lob/order_book.hpp"
#include "hft/types.hpp"

namespace hft::persist {

inline constexpr std::uint32_t kWalMagic = 0x4857414CUL;  // "HWAL"
inline constexpr std::size_t kWalHeaderSize = 14;         // magic+seq+len
inline constexpr std::size_t kWalChecksumSize = 8;

[[nodiscard]] inline std::uint64_t fnv1a_u64(std::uint64_t h, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= 0x0000'0100'0000'01B3ULL;
    }
    return h;
}

inline void put_be16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}
inline void put_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}
inline void put_be64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}
[[nodiscard]] inline std::uint16_t get_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}
[[nodiscard]] inline std::uint32_t get_be32(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v = static_cast<std::uint32_t>((v << 8) | p[i]);
    }
    return v;
}
[[nodiscard]] inline std::uint64_t get_be64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

/// Append-only WAL writer over an in-memory byte vector.
class WalWriter final {
public:
    /// Append one raw frame (2-byte length prefix + body) at `seq`.
    void append(std::uint64_t seq, const std::uint8_t* frame, std::size_t frame_size) {
        const std::size_t base = bytes_.size();
        put_be32(bytes_, kWalMagic);
        put_be64(bytes_, seq);
        put_be16(bytes_, static_cast<std::uint16_t>(frame_size));
        bytes_.insert(bytes_.end(), frame, frame + frame_size);
        std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
        for (std::size_t i = base; i < bytes_.size(); ++i) {
            h ^= bytes_[i];
            h *= 0x0000'0100'0000'01B3ULL;
        }
        put_be64(bytes_, h);
        ++records_;
        last_seq_ = seq;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t records() const noexcept { return records_; }
    [[nodiscard]] std::uint64_t last_seq() const noexcept { return last_seq_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t records_ = 0;
    std::uint64_t last_seq_ = 0;
};

enum class WalStatus : std::uint8_t { ok = 0, duplicate, gap, corrupt, truncated };

struct WalRecord {
    std::uint64_t seq = 0;
    std::size_t offset = 0;   // start of this record in the log
    std::size_t frame_size = 0;
};

/// WAL reader with idempotent apply bookkeeping.
class WalReader final {
public:
    explicit WalReader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    /// Set the "already applied up to" watermark (e.g. from a snapshot).
    void resume_from(std::uint64_t applied_seq) noexcept {
        last_applied_ = applied_seq;
    }

    /// Read the next record. Returns false at clean end of log.
    /// `status` reports duplicate/gap/corrupt/truncated for this record.
    bool next(WalRecord& rec, WalStatus& status) {
        status = WalStatus::ok;
        if (offset_ >= bytes_.size()) {
            return false;
        }
        rec.offset = offset_;
        if (bytes_.size() - offset_ < kWalHeaderSize) {
            status = WalStatus::truncated;
            return false;
        }
        const std::uint8_t* p = bytes_.data() + offset_;
        if (get_be32(p) != kWalMagic) {
            status = WalStatus::corrupt;
            return false;
        }
        rec.seq = get_be64(p + 4);
        rec.frame_size = get_be16(p + 12);
        const std::size_t total = kWalHeaderSize + rec.frame_size + kWalChecksumSize;
        if (bytes_.size() - offset_ < total) {
            status = WalStatus::truncated;
            return false;
        }
        std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
        for (std::size_t i = 0; i < kWalHeaderSize + rec.frame_size; ++i) {
            h ^= p[i];
            h *= 0x0000'0100'0000'01B3ULL;
        }
        if (h != get_be64(p + kWalHeaderSize + rec.frame_size)) {
            status = WalStatus::corrupt;
            return false;
        }
        offset_ += total;
        if (rec.seq <= last_applied_) {
            status = WalStatus::duplicate;
            return true;  // caller skips applying
        }
        if (last_applied_ != 0 && rec.seq != last_applied_ + 1) {
            status = WalStatus::gap;
            return true;  // caller decides; do not auto-continue
        }
        status = WalStatus::ok;
        last_applied_ = rec.seq;
        return true;
    }

    [[nodiscard]] const std::uint8_t* frame(const WalRecord& rec) const noexcept {
        return bytes_.data() + rec.offset + kWalHeaderSize;
    }

    [[nodiscard]] std::uint64_t last_applied() const noexcept { return last_applied_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_ = 0;
    std::uint64_t last_applied_ = 0;
};

// ---- Snapshot ------------------------------------------------------

struct SnapshotOrder {
    std::uint8_t side = 0;  // 'B' / 'S'
    std::uint64_t id = 0;
    std::uint64_t price = 0;  // raw
    std::uint64_t size = 0;   // raw
    std::uint64_t state = 0;
};

struct Snapshot {
    std::uint64_t applied_seq = 0;
    std::uint64_t checksum = 0;
    std::vector<SnapshotOrder> orders;  // dump order: bids then asks
};

[[nodiscard]] inline std::uint64_t book_checksum(const lob::OrderBook& book) {
    std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
    for (const auto& o : book.orders(Side::bid)) {
        h = fnv1a_u64(h, o.id);
        h = fnv1a_u64(h, static_cast<std::uint64_t>(o.price.raw()));
        h = fnv1a_u64(h, o.size.raw());
        h = fnv1a_u64(h, static_cast<std::uint64_t>(o.state));
    }
    for (const auto& o : book.orders(Side::ask)) {
        h = fnv1a_u64(h, o.id);
        h = fnv1a_u64(h, static_cast<std::uint64_t>(o.price.raw()));
        h = fnv1a_u64(h, o.size.raw());
        h = fnv1a_u64(h, static_cast<std::uint64_t>(o.state));
    }
    return h;
}

[[nodiscard]] inline Snapshot take_snapshot(const lob::OrderBook& book, std::uint64_t applied_seq) {
    Snapshot s;
    s.applied_seq = applied_seq;
    s.checksum = book_checksum(book);
    const Side sides[2] = {Side::bid, Side::ask};
    for (Side side : sides) {
        for (const auto& o : book.orders(side)) {
            SnapshotOrder so;
            so.side = side == Side::bid ? static_cast<std::uint8_t>('B') : static_cast<std::uint8_t>('S');
            so.id = o.id;
            so.price = static_cast<std::uint64_t>(o.price.raw());
            so.size = o.size.raw();
            so.state = static_cast<std::uint64_t>(o.state);
            s.orders.push_back(so);
        }
    }
    return s;
}

/// Rebuild a book from a snapshot. Returns false (and does not claim
/// success) if the restored checksum differs from the snapshot's.
inline bool restore_snapshot(lob::OrderBook& book, const Snapshot& snap) {
    for (const SnapshotOrder& o : snap.orders) {
        lob::BookStatus st{};
        book.add(o.side == static_cast<std::uint8_t>('B') ? Side::bid : Side::ask,
                 Price::from_raw(static_cast<std::int64_t>(o.price)),
                 Quantity::from_raw(o.size), o.id, st);
        if (st != lob::BookStatus::ok) {
            return false;
        }
    }
    return book_checksum(book) == snap.checksum;
}

}  // namespace hft::persist
