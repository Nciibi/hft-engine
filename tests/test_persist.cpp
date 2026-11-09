// Persistence tests: WAL round-trip, corruption/truncation
// detection, duplicate skip (idempotent restart), gap reporting,
// snapshot restore checksum match, snapshot+WAL-tail recovery.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "hft/itch/decode.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/persist/wal.hpp"
#include "hft/types.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

using hft::OrderId;
using hft::Price;
using hft::Quantity;
using hft::Side;
namespace lob = hft::lob;
namespace persist = hft::persist;

// Build a minimal raw Add Order frame: 2-byte prefix + 36-byte body.
std::vector<std::uint8_t> add_frame(std::uint64_t id, std::uint32_t shares, std::uint32_t price) {
    std::vector<std::uint8_t> f(38, 0);
    f[0] = 0;
    f[1] = 36;  // length prefix (body size)
    f[2] = 'A';
    // locate(2)=1, tracking(2)=0, timestamp(6)=0, id(8)
    for (std::size_t i = 0; i < 8; ++i) {
        f.at(13 + i) = static_cast<std::uint8_t>((id >> ((7 - i) * 8)) & 0xFFu);
    }
    f[2 + 19] = 'B';  // side
    for (std::size_t i = 0; i < 4; ++i) {
        f.at(22 + i) = static_cast<std::uint8_t>((shares >> ((3 - i) * 8)) & 0xFFu);
    }
    const char* sym = "SIMTEST ";
    for (std::size_t i = 0; i < 8; ++i) {
        f.at(26 + i) = static_cast<std::uint8_t>(sym[i]);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        f.at(34 + i) = static_cast<std::uint8_t>((price >> ((3 - i) * 8)) & 0xFFu);
    }
    return f;
}

}  // namespace

int main() {
    // WAL round-trip: write 3 frames, read them back in order.
    {
        persist::WalWriter w;
        const auto f1 = add_frame(1, 100, 1'000'000);
        const auto f2 = add_frame(2, 200, 1'000'100);
        const auto f3 = add_frame(3, 300, 999'900);
        w.append(1, f1.data(), f1.size());
        w.append(2, f2.data(), f2.size());
        w.append(3, f3.data(), f3.size());
        check(w.records() == 3, "writer counts records");
        check(w.last_seq() == 3, "writer last seq");

        persist::WalReader r(w.bytes());
        persist::WalRecord rec;
        persist::WalStatus st;
        lob::OrderBook book(1024, 1024);
        std::uint64_t expect_seq = 1;
        int applied = 0;
        while (r.next(rec, st)) {
            check(st == persist::WalStatus::ok, "round-trip record ok");
            check(rec.seq == expect_seq, "round-trip seq in order");
            ++expect_seq;
            const auto res = hft::itch::decode(r.frame(rec), rec.frame_size);
            check(res.ok(), "round-trip frame decodes");
            if (res.ok()) {
                check(hft::lob::apply(res.message, book).applied, "round-trip frame applies");
                ++applied;
            }
        }
        check(applied == 3, "all three frames applied from WAL");
        check(book.order_count(Side::bid) == 3, "book holds three orders after WAL replay");
    }

    // Corruption: flip a byte in the log -> corrupt, not ok.
    {
        persist::WalWriter w;
        const auto f1 = add_frame(10, 50, 1'000'000);
        w.append(1, f1.data(), f1.size());
        auto bytes = w.bytes();
        bytes.at(20) ^= 0xFF;  // inside the frame bytes
        persist::WalReader r(bytes);
        persist::WalRecord rec;
        persist::WalStatus st;
        const bool got = r.next(rec, st);
        check(!got || st == persist::WalStatus::corrupt, "corrupted record reported corrupt");
        check(st == persist::WalStatus::corrupt, "bit flip detected by checksum");
    }

    // Truncation: cut the log mid-record -> truncated.
    {
        persist::WalWriter w;
        const auto f1 = add_frame(20, 50, 1'000'000);
        w.append(1, f1.data(), f1.size());
        auto bytes = w.bytes();
        bytes.resize(bytes.size() - 4);  // cut checksum tail
        persist::WalReader r(bytes);
        persist::WalRecord rec;
        persist::WalStatus st;
        r.next(rec, st);
        check(st == persist::WalStatus::truncated, "torn tail reported truncated");
    }

    // Idempotent restart: same WAL replayed twice (duplicate seqs
    // skipped on the second pass after resume_from).
    {
        persist::WalWriter w;
        const auto f1 = add_frame(30, 100, 1'000'000);
        const auto f2 = add_frame(31, 100, 1'000'100);
        w.append(1, f1.data(), f1.size());
        w.append(2, f2.data(), f2.size());
        persist::WalReader r(w.bytes());
        persist::WalRecord rec;
        persist::WalStatus st;
        lob::OrderBook book(1024, 1024);
        while (r.next(rec, st)) {
            if (st == persist::WalStatus::ok) {
                const auto res = hft::itch::decode(r.frame(rec), rec.frame_size);
                if (res.ok()) {
                    const auto ar1 = hft::lob::apply(res.message, book); (void)ar1;
                }
            }
        }
        check(book.order_count(Side::bid) == 2, "first replay applies both");
        const auto csum_first = persist::book_checksum(book);
        // Restart: new reader over the same log, resumed at seq 2.
        // Every record is a duplicate and must be skipped.
        persist::WalReader r2(w.bytes());
        r2.resume_from(2);
        int dupes = 0;
        while (r2.next(rec, st)) {
            if (st == persist::WalStatus::duplicate) {
                ++dupes;
            } else {
                check(false, "replayed record after resume is duplicate, not ok");
            }
        }
        check(dupes == 2, "both records skipped as duplicates on restart");
        check(persist::book_checksum(book) == csum_first, "book unchanged after idempotent replay");
    }

    // Gap: seq jumps 1 -> 5 -> reported as gap.
    {
        persist::WalWriter w;
        const auto f1 = add_frame(40, 10, 1'000'000);
        w.append(1, f1.data(), f1.size());
        w.append(5, f1.data(), f1.size());
        persist::WalReader r(w.bytes());
        persist::WalRecord rec;
        persist::WalStatus st;
        r.next(rec, st);
        check(st == persist::WalStatus::ok, "first record ok before gap");
        r.next(rec, st);
        check(st == persist::WalStatus::gap, "seq jump reported as gap");
        check(rec.seq == 5, "gap record carries its seq");
    }

    // Snapshot: take, restore into a fresh book, checksums match.
    {
        lob::OrderBook book(1024, 1024);
        lob::BookStatus s{};
        book.add(Side::bid, Price::from_raw(1'000'000), Quantity::from_raw(100), 100, s);
        check(s == lob::BookStatus::ok, "snapshot setup add 1");
        book.add(Side::bid, Price::from_raw(1'000'000), Quantity::from_raw(200), 101, s);
        check(s == lob::BookStatus::ok, "snapshot setup add 2 (same level)");
        book.add(Side::ask, Price::from_raw(1'001'000), Quantity::from_raw(50), 102, s);
        check(s == lob::BookStatus::ok, "snapshot setup add 3 (ask)");
        // Partial execute on order 100 changes size/state.
        check(book.execute(100, Quantity::from_raw(30)) == lob::BookStatus::ok,
              "snapshot setup partial execute");

        const auto snap = persist::take_snapshot(book, 42);
        check(snap.applied_seq == 42, "snapshot carries applied seq");
        check(snap.orders.size() == 3, "snapshot carries all live orders");

        lob::OrderBook restored(1024, 1024);
        check(persist::restore_snapshot(restored, snap), "snapshot restore succeeds");
        check(persist::book_checksum(restored) == snap.checksum,
              "restored book checksum matches snapshot");
        check(persist::book_checksum(restored) == persist::book_checksum(book),
              "restored book checksum matches original");
    }

    // Snapshot + WAL tail: snapshot at seq 2, then WAL records 3-4
    // applied on the restored book == full replay.
    {
        const auto f1 = add_frame(200, 100, 1'000'000);
        const auto f2 = add_frame(201, 100, 1'000'100);
        const auto f3 = add_frame(202, 100, 999'900);
        const auto f4 = add_frame(203, 100, 999'800);
        lob::OrderBook full(1024, 1024);
        const std::vector<std::vector<std::uint8_t>> frames = {f1, f2, f3, f4};
        persist::WalWriter w;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto res = hft::itch::decode(frames[i].data(), frames[i].size());
            check(res.ok(), "full replay frame decodes");
            if (res.ok()) {
                const auto ar2 = hft::lob::apply(res.message, full); (void)ar2;
            }
            w.append(static_cast<std::uint64_t>(i + 1), frames[i].data(), frames[i].size());
        }
        // Recover: snapshot after seq 2 from a half-replayed book,
        // then WAL tail 3-4.
        lob::OrderBook half(1024, 1024);
        for (std::size_t i = 0; i < 2; ++i) {
            const auto res = hft::itch::decode(frames[i].data(), frames[i].size());
            if (res.ok()) {
                const auto ar3 = hft::lob::apply(res.message, half); (void)ar3;
            }
        }
        const auto snap = persist::take_snapshot(half, 2);
        lob::OrderBook recovered(1024, 1024);
        check(persist::restore_snapshot(recovered, snap), "recovery restore succeeds");
        persist::WalReader r(w.bytes());
        r.resume_from(2);
        persist::WalRecord rec;
        persist::WalStatus st;
        int tail_applied = 0;
        while (r.next(rec, st)) {
            if (st == persist::WalStatus::ok) {
                const auto res = hft::itch::decode(r.frame(rec), rec.frame_size);
                if (res.ok()) {
                    const auto ar4 = hft::lob::apply(res.message, recovered); (void)ar4;
                    ++tail_applied;
                }
            }
        }
        check(tail_applied == 2, "WAL tail applied after snapshot");
        check(persist::book_checksum(recovered) == persist::book_checksum(full),
              "snapshot+tail recovery matches full replay");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
