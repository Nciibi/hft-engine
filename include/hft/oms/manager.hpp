// Order management: our own orders, and the state they are in.
//
// Distinct from the book, which tracks everyone's orders as the market
// reports them. The OMS tracks the orders this process owns and is
// responsible for: what we asked for, what the venue acknowledged,
// what filled, and what we must cancel. The two disagree often, and
// reconciling them is the whole job.
//
// The hard cases, which is why this is a state machine and not a
// handful of booleans:
//
//   * A fill arrives after a cancel was requested. The order is not
//     cancelled, it is partially filled, and the remainder still needs
//     cancelling. Systems that treat "cancel pending" as a terminal
//     state silently drop the residual order and leak inventory.
//   * A replace is acknowledged with the new reference but the venue
//     rejected the modification. The original order is still working.
//   * An order fills completely in the same batch that acknowledges it.
//     It must go to filled, never to working, or it will sit in the
//     working set forever.
//
// Terminal states are terminal. Nothing leaves filled, cancelled or
// rejected, and the state machine refuses the transition rather than
// letting a late message resurrect a completed order.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "hft/risk/limits.hpp"
#include "hft/types.hpp"

namespace hft::oms {

enum class OrdState : std::uint8_t {
    /// Sent, not yet acknowledged by the venue.
    pending_new = 0,
    /// Live on the exchange.
    working,
    /// Cancel requested, awaiting the venue's confirmation.
    pending_cancel,
    /// Modification requested, awaiting confirmation.
    pending_replace,
    /// Some quantity filled; a remainder is still working.
    partially_filled,
    /// Terminal: fully filled.
    filled,
    /// Terminal: cancelled, no quantity left.
    cancelled,
    /// Terminal: refused by the venue or by pre-trade risk.
    rejected,
};

[[nodiscard]] constexpr const char* to_string(OrdState s) noexcept {
    switch (s) {
        case OrdState::pending_new:      return "pending_new";
        case OrdState::working:          return "working";
        case OrdState::pending_cancel:   return "pending_cancel";
        case OrdState::pending_replace:  return "pending_replace";
        case OrdState::partially_filled: return "partially_filled";
        case OrdState::filled:           return "filled";
        case OrdState::cancelled:        return "cancelled";
        case OrdState::rejected:         return "rejected";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool is_terminal(OrdState s) noexcept {
    return s == OrdState::filled || s == OrdState::cancelled || s == OrdState::rejected;
}

/// Would an order in this state still be at risk of trading?
[[nodiscard]] constexpr bool is_live(OrdState s) noexcept {
    return !is_terminal(s);
}

/// The transition table, as data rather than as scattered `if`s, so it
/// can be exhaustively tested and printed.
[[nodiscard]] constexpr bool can_transition(OrdState from, OrdState to) noexcept {
    switch (from) {
        case OrdState::pending_new:
            return to == OrdState::working || to == OrdState::partially_filled ||
                   to == OrdState::filled || to == OrdState::rejected ||
                   to == OrdState::cancelled;
        case OrdState::working:
            return to == OrdState::partially_filled || to == OrdState::filled ||
                   to == OrdState::pending_cancel || to == OrdState::pending_replace ||
                   to == OrdState::cancelled;
        case OrdState::pending_cancel:
            // A fill can always arrive while a cancel is outstanding.
            // This edge is the entire reason pending_cancel is not
            // terminal.
            return to == OrdState::partially_filled || to == OrdState::filled ||
                   to == OrdState::cancelled || to == OrdState::rejected;
        case OrdState::pending_replace:
            return to == OrdState::partially_filled || to == OrdState::filled ||
                   to == OrdState::cancelled || to == OrdState::rejected ||
                   to == OrdState::working;
        case OrdState::partially_filled:
            return to == OrdState::partially_filled || to == OrdState::filled ||
                   to == OrdState::pending_cancel || to == OrdState::cancelled;
        case OrdState::filled:
        case OrdState::cancelled:
        case OrdState::rejected:
            return false;  // terminal
    }
    return false;
}

enum class OmsStatus : std::uint8_t {
    ok = 0,
    unknown_order,
    illegal_transition,
    /// Refused by pre-trade risk. The risk reason is in the OMS record.
    risk_rejected,
    /// No such handle, or it is not live.
    not_live,
    /// Cancelling an order with no remaining quantity.
    nothing_to_cancel,
};

struct Order {
    OrderId id = kInvalidOrderId;
    Side side = Side::bid;
    Price price{};
    Quantity original_size{};
    Quantity leaves_qty{};
    OrdState state = OrdState::pending_new;
    /// Reference the venue assigned, or kInvalidOrderId if the venue
    /// has not acknowledged yet. Distinct from `id`, which is ours.
    OrderId venue_ref = kInvalidOrderId;
    /// The risk verdict, retained so a rejected order can be
    /// explained after the fact. An unexplained rejection is the thing
    /// that costs an engineer an afternoon.
    risk::LimitStatus risk_status = risk::LimitStatus::ok;
    Nanos last_update = 0;
};

class Manager final {
public:
    /// Reserve room for `capacity` concurrent orders. Preallocating is
    /// the point: an OMS that grows a vector while trading is an OMS
    /// with a latency cliff exactly when it is already stressed.
    explicit Manager(std::size_t capacity = 65'536, risk::Limits limits = {})
        : orders_(capacity), live_(capacity, 0), held_(capacity, 0), limits_(limits), risk_(limits) {
        // Populated in reverse so the first submit takes slot 0, which
        // makes behaviour reproducible run to run and test failures
        // directly replayable.
        free_slots_.reserve(capacity);
        for (std::size_t i = capacity; i-- > 0;) {
            free_slots_.push_back(i);
        }
    }

    // ---- Configuration ---------------------------------------------

    void set_limits(const risk::Limits& limits) noexcept {
        limits_ = limits;
        risk_.set_limits(limits);
    }
    [[nodiscard]] risk::PreTradeRisk& risk() noexcept { return risk_; }
    [[nodiscard]] const risk::PreTradeRisk& risk() const noexcept { return risk_; }

    void set_reference_price(Price price) noexcept { risk_.set_reference_price(price); }

    // ---- Our actions -------------------------------------------------

    /// Submit a new order. Pre-trade risk is evaluated here and only
    /// here: a limit that can be bypassed is not a limit.
    ///
    /// Returns a live slot index, or -1. On rejection the reason is
    /// available from `risk().last_status()`.
    int submit(Side side, Price price, Quantity size, Nanos now) noexcept {
        const risk::Decision decision = risk_.check(side, price, size, now);
        if (!decision.allowed()) {
            ++risk_rejected_;
            return -1;
        }

        const std::size_t slot = free_slots_.empty() ? SIZE_MAX : free_slots_.back();
        if (slot == SIZE_MAX) {
            // Full. Refusing is the only safe answer: evicting a live
            // order to make room for a new one would strand inventory.
            // Counted separately from a risk rejection, because the
            // two call for completely different operator responses and
            // a single combined counter hid that distinction.
            ++pool_full_;
            return -1;
        }
        free_slots_.pop_back();

        Order& o = orders_[slot];
        // A free slot may still hold a retired order's terminal state.
        // Reusing the slot discards that order, so its contribution to
        // the state counters must be removed BEFORE the record is
        // overwritten.
        //
        // Skipping this made the counters drift negative over a long
        // run, because `o = Order{}` resets the state without telling
        // the counters anything. The sum of the counters stopped
        // equalling the number of orders, and a reconcile view showing
        // -105 orders in live states is the kind of thing that gets
        // discovered in production.
        --state_counts_[static_cast<std::size_t>(o.state)];
        o = Order{};
        o.id = next_id_++;
        o.side = side;
        o.price = price;
        o.original_size = size;
        o.leaves_qty = size;
        o.risk_status = risk::LimitStatus::ok;
        o.last_update = now;
        live_[slot] = 1;
        // Counted through the same helper as every other transition,
        // so the reconcile view cannot drift from the actual state.
        transition(o, OrdState::pending_new);

        ++submitted_;
        return static_cast<int>(slot);
    }

    /// Ask the venue to cancel. Returns what the state machine decided.
    OmsStatus request_cancel(int slot, Nanos now) noexcept {
        Order& o = lookup(slot);
        if (o.id == kInvalidOrderId) {
            return OmsStatus::unknown_order;
        }
        if (!is_live(o.state)) {
            return OmsStatus::not_live;
        }
        if (o.leaves_qty.is_zero()) {
            return OmsStatus::nothing_to_cancel;
        }
        if (!can_transition(o.state, OrdState::pending_cancel)) {
            return OmsStatus::illegal_transition;
        }
        transition(o, OrdState::pending_cancel);
        o.last_update = now;
        return OmsStatus::ok;
    }

    /// Ask the venue to modify quantity and price. A modify keeps the
    /// same order reference at most venues, so `venue_ref` is
    /// untouched here and only settled by the venue's reply.
    OmsStatus request_replace(int slot, Price new_price, Quantity new_size, Nanos now) noexcept {
        Order& o = lookup(slot);
        if (o.id == kInvalidOrderId) {
            return OmsStatus::unknown_order;
        }
        if (!is_live(o.state)) {
            return OmsStatus::not_live;
        }
        if (!can_transition(o.state, OrdState::pending_replace)) {
            return OmsStatus::illegal_transition;
        }
        o.price = new_price;
        o.original_size = new_size;
        o.last_update = now;
        transition(o, OrdState::pending_replace);
        return OmsStatus::ok;
    }

    // ---- Venue messages ----------------------------------------------

    OmsStatus on_ack(int slot, OrderId venue_ref, Nanos now) noexcept {
        Order& o = lookup(slot);
        if (o.id == kInvalidOrderId) {
            return OmsStatus::unknown_order;
        }
        if (!can_transition(o.state, OrdState::working)) {
            return OmsStatus::illegal_transition;
        }
        o.venue_ref = venue_ref;
        transition(o, OrdState::working);
        o.last_update = now;
        return OmsStatus::ok;
    }

    OmsStatus on_reject(int slot, Nanos now) noexcept {
        Order& o = lookup(slot);
        if (o.id == kInvalidOrderId) {
            return OmsStatus::unknown_order;
        }
        if (!can_transition(o.state, OrdState::rejected)) {
            return OmsStatus::illegal_transition;
        }
        transition(o, OrdState::rejected);
        o.leaves_qty = Quantity{};
        o.last_update = now;
        retire(slot);
        return OmsStatus::ok;
    }

    /// An execution report. `qty` may only consume what is left: an
    /// over-fill is a protocol violation, and accepting it would make
    /// the position counters disagree with the venue's.
    OmsStatus on_fill(int slot, Quantity qty, Price fill_price, Nanos now) noexcept {
        Order& o = lookup(slot);
        if (o.id == kInvalidOrderId) {
            return OmsStatus::unknown_order;
        }
        if (is_terminal(o.state)) {
            return OmsStatus::illegal_transition;
        }
        if (qty.is_zero() || qty.raw() > o.leaves_qty.raw()) {
            return OmsStatus::illegal_transition;
        }

        // Position is updated from the fill, not from the order's
        // resting size. Deriving one from the other is how an OMS ends
        // up with a position that disagrees with the venue by exactly
        // the amount that has been cancelled and refilled.
        risk_.on_fill(o.side, qty);
        ++fills_;

        o.leaves_qty = o.leaves_qty.saturating_sub(qty);
        o.last_update = now;
        (void)fill_price;

        if (o.leaves_qty.is_zero()) {
            transition(o, OrdState::filled);
            retire(slot);
            return OmsStatus::ok;
        }
        // A partial fill while a cancel is outstanding leaves the
        // remainder working-and-cancel-pending, which `pending_cancel`
        // already means.
        if (!can_transition(o.state, OrdState::partially_filled)) {
            return OmsStatus::illegal_transition;
        }
        transition(o, OrdState::partially_filled);
        return OmsStatus::ok;
    }

    OmsStatus on_cancel_ack(int slot, Nanos now) noexcept {
        Order& o = lookup(slot);
        if (o.id == kInvalidOrderId) {
            return OmsStatus::unknown_order;
        }
        if (o.state == OrdState::filled) {
            // A fill beat the cancel. The order is done, and the
            // cancel ack is stale news. Reporting an error here would
            // make a normal race look like a fault.
            return OmsStatus::illegal_transition;
        }
        if (!can_transition(o.state, OrdState::cancelled)) {
            return OmsStatus::illegal_transition;
        }
        transition(o, OrdState::cancelled);
        o.leaves_qty = Quantity{};
        o.last_update = now;
        retire(slot);
        return OmsStatus::ok;
    }

    OmsStatus on_replace_ack(int slot, Nanos now) noexcept {
        Order& o = lookup(slot);
        if (o.id == kInvalidOrderId) {
            return OmsStatus::unknown_order;
        }
        if (!can_transition(o.state, OrdState::working)) {
            return OmsStatus::illegal_transition;
        }
        transition(o, OrdState::working);
        o.last_update = now;
        return OmsStatus::ok;
    }

    // ---- Queries -----------------------------------------------------

    [[nodiscard]] const Order* at(int slot) const noexcept {
        if (slot < 0 || static_cast<std::size_t>(slot) >= orders_.size()) {
            return nullptr;
        }
        return &orders_[static_cast<std::size_t>(slot)];
    }

    /// Count of orders in each state, for reconciliation.
    ///
    /// Reads maintained counters rather than scanning the slot pool.
    /// An earlier version gated this on the live flag, which meant a
    /// retired order's terminal state became invisible the moment its
    /// slot was released: `count(filled)` returned zero for an OMS
    /// whose entire history was filled orders. For the one purpose
    /// this function exists for, that is worse than useless.
    [[nodiscard]] std::size_t count(OrdState s) const noexcept {
        return state_counts_[static_cast<std::size_t>(s)];
    }

    [[nodiscard]] std::uint64_t submitted() const noexcept { return submitted_; }
    /// Refused by pre-trade risk.
    [[nodiscard]] std::uint64_t risk_rejected() const noexcept { return risk_rejected_; }
    /// Refused because the slot pool was full. Distinct from a risk
    /// rejection: this one means capacity, not misbehaviour.
    [[nodiscard]] std::uint64_t pool_full() const noexcept { return pool_full_; }
    /// Every refusal, of either kind.
    [[nodiscard]] std::uint64_t rejected() const noexcept {
        return risk_rejected_ + pool_full_;
    }
    [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }

    /// Engage the kill switch and cancel every live order.
    ///
    /// Cancelling in the same call is deliberate. A switch that stops
    /// new orders but leaves working ones in place is not a kill switch;
    /// it is a suggestion.
    void kill_all(const char* reason, Nanos now) noexcept {
        risk_.trip(reason);
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            if (live_[i] == 0) {
                continue;
            }
            Order& o = orders_[i];
            if (is_terminal(o.state)) {
                continue;
            }
            o.leaves_qty = Quantity{};
            o.last_update = now;
            transition(o, OrdState::cancelled);
            retire(i);
        }
    }

private:
    [[nodiscard]] Order& lookup(int slot) noexcept {
        static Order kEmpty{};
        if (slot < 0 || static_cast<std::size_t>(slot) >= orders_.size()) {
            return kEmpty;
        }
        return orders_[static_cast<std::size_t>(slot)];
    }

    /// The ONLY way an order's state changes.
    ///
    /// Routing every transition through one function means the state
    /// counters cannot drift from the actual state. Letting each caller
    /// assign `o.state` directly is how a reconcile view starts
    /// disagreeing with reality: the counters get updated in some
    /// places and not others, and the discrepancy only shows up as a
    /// wrong number on an operations dashboard.
    void transition(Order& o, OrdState to) noexcept {
        if (o.state != to) {
            --state_counts_[static_cast<std::size_t>(o.state)];
            ++state_counts_[static_cast<std::size_t>(to)];
            o.state = to;
        }
    }

    /// Return a slot to the free pool. Only for terminal orders.
    void retire(std::size_t slot) noexcept {
        if (live_[slot] != 0) {
            live_[slot] = 0;
            free_slots_.push_back(slot);
        }
    }

    std::vector<Order> orders_;
    std::vector<std::uint8_t> live_;
    std::vector<std::size_t> free_slots_;

    risk::Limits limits_{};
    risk::PreTradeRisk risk_{};

    OrderId next_id_ = 1;
    std::uint64_t submitted_ = 0;
    std::uint64_t risk_rejected_ = 0;
    std::uint64_t pool_full_ = 0;
    std::uint64_t fills_ = 0;
    /// Index is the OrdState value. Maintained by `transition` only.
    std::size_t state_counts_[8] = {};
};

}  // namespace hft::oms
