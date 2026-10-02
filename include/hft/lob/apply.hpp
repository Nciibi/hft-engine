// Applying decoded messages to the book.
//
// This is the OMS kernel: the single place where a decoded ITCH message
// becomes a book mutation. Keeping it separate from the decoder and the
// book means the three can each be tested against the other two, and it
// keeps the tag dispatch in one place instead of scattered across the
// decoder, the book, and the replay loop.
//
// Every message type funnels into exactly one of the book's four
// mutations. There is no fifth path, and no message type may reach the
// book by any other route.

#pragma once

#include <variant>

#include "hft/itch/decode.hpp"
#include "hft/lob/order_book.hpp"

namespace hft::lob {

/// Outcome of applying one message. `detail` carries the book's own
/// status; the wrapper adds nothing the caller needs to distinguish.
struct ApplyResult {
    BookStatus detail = BookStatus::ok;
    /// True when the message changed book state. A message for an
    /// unknown order is *not* an error in a live feed: the add that
    /// created it may have been lost in an earlier gap, so it is
    /// reported rather than thrown.
    bool applied = false;

    [[nodiscard]] bool ok() const noexcept {
        return detail == BookStatus::ok || detail == BookStatus::unknown_order;
    }
};

/// Apply one decoded message to `book`.
///
/// ITCH 'C' (Order Executed With Price) is treated exactly like 'E'
/// for book-state purposes. The execution price is not applied to the
/// resting order, because a fill at a price other than the displayed
/// price does not change the order's own limit price. Consuming the
/// shares is the only book-visible effect, and getting that wrong is a
/// way to silently desynchronise aggregate size.
[[nodiscard]] ApplyResult apply(const itch::Message& message, OrderBook& book) noexcept {
    return std::visit(
        [&book](const auto& payload) noexcept -> ApplyResult {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, itch::AddOrder>) {
                BookStatus status{};
                book.add(payload.side, payload.price, payload.size, payload.id, status);
                return ApplyResult{status, status == BookStatus::ok};
            } else if constexpr (std::is_same_v<T, itch::OrderExecuted>) {
                const BookStatus s = book.execute(payload.id, payload.shares);
                return ApplyResult{s, s == BookStatus::ok};
            } else if constexpr (std::is_same_v<T, itch::OrderExecutedAtPrice>) {
                const BookStatus s = book.execute(payload.id, payload.shares);
                return ApplyResult{s, s == BookStatus::ok};
            } else if constexpr (std::is_same_v<T, itch::OrderCancel>) {
                const BookStatus s = book.cancel_partial(payload.id, payload.shares);
                return ApplyResult{s, s == BookStatus::ok};
            } else if constexpr (std::is_same_v<T, itch::OrderReplace>) {
                // Remove the original outright, then add the replacement.
                //
                // Not a cancel-then-add of the same reference: the
                // specification issues a NEW reference number, which is
                // what conveys new time priority. `add` appends to the
                // back of the level's queue, so the replacement lands
                // behind everything already resting at that price, which
                // is exactly what "belongs below other orders at the same
                // price in priority" means.
                //
                // The original order is looked up rather than assumed,
                // because 'U' carries no side, no stock and no MPID:
                // those are carried over from the Add Order, and there
                // is nowhere else to get them. A replace for an unknown
                // reference is reported, not guessed.
                const std::optional<OrderSnapshot> prior = book.find(payload.original_id);
                if (!prior.has_value()) {
                    return ApplyResult{BookStatus::unknown_order, false};
                }
                const BookStatus removed = book.remove(payload.original_id);
                if (removed != BookStatus::ok) {
                    return ApplyResult{removed, false};
                }
                BookStatus status{};
                book.add(prior->side, payload.price, payload.shares, payload.new_id, status);
                return ApplyResult{status, status == BookStatus::ok};
            } else if constexpr (std::is_same_v<T, itch::BrokenTrade>) {
                // Deliberately a no-op. The specification states that a
                // firm using the feed to build a book "may ignore these
                // messages as they have no impact on the current book".
                // Reported as applied so that a caller counting processed
                // messages sees it arrive, rather than silently
                // swallowing a tag it does not otherwise handle.
                return ApplyResult{BookStatus::ok, true};
            } else {
                const BookStatus s = book.remove(payload.id);
                return ApplyResult{s, s == BookStatus::ok};
            }
        },
        message.body);
}

/// Apply every Add Order in a decoded message, or nothing. Used by the
/// differential test to confirm the apply path and the direct book API
/// agree, which is the one way to catch a divergence between them.
[[nodiscard]] inline ApplyResult apply_add(const itch::AddOrder& ao, OrderBook& book) noexcept {
    BookStatus status{};
    book.add(ao.side, ao.price, ao.size, ao.id, status);
    return ApplyResult{status, status == BookStatus::ok};
}

}  // namespace hft::lob
