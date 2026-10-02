// Pre-trade risk.
//
// This runs on every order before it is allowed anywhere near the wire,
// so it has two hard requirements: it must be fast, and when it rejects
// an order it must say precisely why. A risk engine that returns a
// blanket "no" is worse than none, because a trader cannot respond to it
// and an operator cannot diagnose it.
//
// Time is injected, never read.
//
// Every entry point that needs the current time takes it as a
// parameter. A risk engine that called a clock internally would be
// impossible to test deterministically and impossible to compare
// against a reference implementation, which is precisely the property
// the rest of this repository relies on. The cost is one parameter; the
// benefit is that a rate limiter here is a pure function of its inputs
// and can be differentially tested like everything else.

#pragma once

#include <cstdint>

#include "hft/types.hpp"

namespace hft::risk {

/// Why an order was refused. Every rejection carries a specific
/// reason; there is deliberately no catch-all, because an unexplained
/// rejection is unactionable.
enum class LimitStatus : std::uint8_t {
    ok = 0,
    /// The kill switch is engaged. Checked before every other limit so
    /// that a tripped switch stops the bleeding immediately rather than
    /// after three more limit evaluations.
    kill_switch,
    /// This side's position would exceed its cap.
    position_limit,
    /// Total absolute exposure across both sides would exceed its cap.
    gross_limit,
    /// The order's own notional exceeds the per-order cap.
    order_notional,
    /// The price is outside the band around the reference price, i.e.
    /// a probable fat finger.
    price_band,
    /// No reference price has been seen yet, so the price band cannot
    /// be evaluated. Refusing is the safe default: a book quoting
    /// without a reference is a book quoting against nothing.
    no_reference_price,
    /// The token bucket is empty.
    rate_limit,
};

[[nodiscard]] constexpr const char* to_string(LimitStatus s) noexcept {
    switch (s) {
        case LimitStatus::ok:                 return "ok";
        case LimitStatus::kill_switch:        return "kill switch engaged";
        case LimitStatus::position_limit:     return "position limit";
        case LimitStatus::gross_limit:        return "gross exposure limit";
        case LimitStatus::order_notional:     return "order notional limit";
        case LimitStatus::price_band:         return "price outside band";
        case LimitStatus::no_reference_price: return "no reference price";
        case LimitStatus::rate_limit:         return "order rate limit";
    }
    return "unknown";
}

struct Limits {
    /// Maximum resting size on one side, in shares. Checked against
    /// the side's current position plus the incoming order.
    std::uint64_t max_position_per_side = 50'000;
    /// Maximum absolute net position across both sides, in shares.
    std::uint64_t max_abs_position = 50'000;
    /// Maximum single-order notional, in raw 1/10000 units.
    std::int64_t max_order_notional_raw = 500'000'000;  // $50,000
    /// Maximum per-order distance from the reference price, in raw
    /// units. $1.00 by default, which is wide enough for a legitimate
    /// far touch and tight enough to catch a missing decimal point.
    std::int64_t max_price_deviation_raw = 10'000;
    /// Sustained order rate, and how much of it may be spent at once.
    /// A bucket of `max_order_rate` refilling at the same rate admits
    /// one full burst then holds the sustained rate, which is the
    /// behaviour a fat-fingering loop needs.
    std::uint64_t max_order_rate = 1'000;
};

/// Result of a single pre-trade check.
struct Decision {
    LimitStatus status = LimitStatus::ok;

    [[nodiscard]] bool allowed() const noexcept { return status == LimitStatus::ok; }
    [[nodiscard]] const char* reason() const noexcept { return to_string(status); }
};

/// Token bucket for order-rate limiting.
///
/// Integer arithmetic throughout, in microtokens: one token is
/// 1'000'000 microtokens. A double here would make the limiter
/// non-reproducible across compilers, and a rate limiter is exactly the
/// kind of component whose behaviour must be identical everywhere.
class TokenBucket final {
public:
    static constexpr std::uint64_t kMicroTokensPerToken = 1'000'000;

    TokenBucket() = default;
    /// `burst_multiplier` is the bucket depth in units of one second's
    /// worth of orders, not an absolute token count. Passing 1 means
    /// "burst to a full second, then hold the sustained rate", which is
    /// the right default for catching a submission loop.
    ///
    /// An earlier revision treated the multiplier as the absolute token
    /// count, so `TokenBucket(100, 1)` admitted exactly ONE order and
    /// then refused a hundred. The rate limit still worked; it simply
    /// did not work at the rate anyone asked for.
    TokenBucket(std::uint64_t rate_per_second, std::uint64_t burst_multiplier)
        : rate_(rate_per_second == 0 ? 1 : rate_per_second),
          burst_tokens_((burst_multiplier == 0 ? 1 : burst_multiplier) * rate_) {
        tokens_ = burst_tokens_ * kMicroTokensPerToken;
    }

    /// Spend one token. Returns false when the bucket is empty.
    [[nodiscard]] bool consume(Nanos now) noexcept {
        refill(now);
        if (tokens_ < kMicroTokensPerToken) {
            return false;
        }
        tokens_ -= kMicroTokensPerToken;
        return true;
    }

    /// Move the bucket forward in time without spending.
    void refill(Nanos now) noexcept {
        if (now > last_) {
            const std::uint64_t elapsed_us = (now - last_) / 1'000u;
            last_ = now;
            if (elapsed_us == 0) {
                return;
            }
            // Clamp the interval to the time it takes to fill the bucket
            // from empty. Beyond that the bucket saturates anyway, so
            // the extra time is genuinely unobservable.
            //
            // The bound must be the time to fill the WHOLE bucket, not
            // the time to add one token. An earlier revision clamped to
            // one token's worth, which silently cut any interval longer
            // than a fraction of a second and made a 10/second limiter
            // refill at roughly 1/second.
            const std::uint64_t fill_us =
                (burst_tokens_ * kMicroTokensPerToken) / rate_ + 1u;
            const std::uint64_t effective_us = elapsed_us > fill_us ? fill_us : elapsed_us;
            // effective_us * rate_ is the refill in MICROtokens
            // directly: microseconds times (tokens per second) is
            // already microtokens, because a microsecond is a
            // millionth of a second. Dividing again here was an
            // earlier mistake that made a 10/second limiter grant
            // 0.005 tokens per half second instead of 5.
            //
            // Overflow: effective_us is bounded by fill_us, so the
            // product is bounded by roughly burst_tokens_ *
            // kMicroTokensPerToken, which is the bucket capacity.
            const std::uint64_t refill_micro = effective_us * rate_;
            const std::uint64_t cap_micro = burst_tokens_ * kMicroTokensPerToken;
            tokens_ = (tokens_ + refill_micro > cap_micro) ? cap_micro : tokens_ + refill_micro;
        } else if (now < last_) {
            // Time went backwards. Do not let it create tokens.
            last_ = now;
        }
    }

    /// Current balance, in whole tokens. For reporting and tests.
    [[nodiscard]] std::uint64_t available_tokens() const noexcept {
        return tokens_ / kMicroTokensPerToken;
    }

private:
    std::uint64_t rate_ = 1;
    std::uint64_t burst_tokens_ = 1;
    std::uint64_t tokens_ = 0;
    Nanos last_ = 0;
};

class PreTradeRisk final {
public:
    PreTradeRisk() = default;

    /// The rate limiter MUST be built here, not left default
    /// constructed. A default TokenBucket has a rate of 1/second and an
    /// empty balance, so a constructor that copies only `limits_`
    /// silently ignores `max_order_rate` and then refuses every order.
    explicit PreTradeRisk(const Limits& limits) { set_limits(limits); }

    // ---- Configuration ---------------------------------------------

    void set_limits(const Limits& limits) noexcept {
        limits_ = limits;
        rate_limiter_ = TokenBucket(limits.max_order_rate, 1);
    }

    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

    /// Feed the reference price the price band is centred on. In a
    /// real system this comes from the market data handler; here it is
    /// an explicit input so the risk engine has no data dependency of
    /// its own.
    void set_reference_price(Price reference) noexcept {
        reference_price_ = reference;
        has_reference_ = true;
    }

    // ---- Kill switch -------------------------------------------------

    /// Engage the switch. It latches: nothing disengages it but
    /// `reset_kill_switch`, so a fault handler cannot clear it by
    /// accident, and clearing it requires a deliberate act.
    ///
    /// `reason` is copied into a fixed buffer rather than stored as a
    /// pointer, because a latched switch that outlives the string it
    /// names is a dangling pointer waiting to be read at 3am.
    void trip(const char* reason) noexcept {
        engaged_ = true;
        std::size_t i = 0;
        if (reason != nullptr) {
            for (; i + 1 < sizeof(reason_) && reason[i] != '\0'; ++i) {
                reason_[i] = reason[i];
            }
        }
        reason_[i] = '\0';
    }

    void reset_kill_switch() noexcept { engaged_ = false; }

    [[nodiscard]] bool kill_switch_engaged() const noexcept { return engaged_; }
    [[nodiscard]] const char* kill_switch_reason() const noexcept { return reason_; }

    // ---- State -------------------------------------------------------

    /// Resting size on one side, in shares.
    [[nodiscard]] std::uint64_t position(Side side) const noexcept {
        return side == Side::bid ? bid_shares_ : ask_shares_;
    }

    /// Signed net position: bids positive, asks negative.
    [[nodiscard]] std::int64_t net_position() const noexcept {
        return static_cast<std::int64_t>(bid_shares_) -
               static_cast<std::int64_t>(ask_shares_);
    }

    /// Total absolute exposure across both sides, in shares.
    [[nodiscard]] std::uint64_t gross_position() const noexcept {
        return bid_shares_ + ask_shares_;
    }

    /// Apply a fill to the position counters. A fill on the bid side
    /// increases the long position; on the ask side, the short.
    void on_fill(Side side, Quantity quantity) noexcept {
        if (side == Side::bid) {
            bid_shares_ += quantity.raw();
        } else {
            ask_shares_ += quantity.raw();
        }
    }

    /// Undo a fill, for a broken trade or a bust.
    void on_unfill(Side side, Quantity quantity) noexcept {
        if (side == Side::bid) {
            bid_shares_ = bid_shares_ > quantity.raw() ? bid_shares_ - quantity.raw() : 0;
        } else {
            ask_shares_ = ask_shares_ > quantity.raw() ? ask_shares_ - quantity.raw() : 0;
        }
    }

    void reset_positions() noexcept {
        bid_shares_ = 0;
        ask_shares_ = 0;
    }

    // ---- The check ---------------------------------------------------

    /// Evaluate an order. `now` is nanoseconds from any monotonic
    /// source; see the file header for why it is a parameter.
    ///
    /// Limits are evaluated in a fixed order, and the order is part of
    /// the contract: the kill switch first, because a tripped switch
    /// must stop everything regardless of what else is true, and the
    /// price band before the rate limiter, so a fat-fingered order is
    /// reported as a fat finger rather than being silently rate-limited
    /// into a different and less actionable reason.
    [[nodiscard]] Decision check(Side side, Price price, Quantity quantity,
                                 Nanos now) noexcept {
        if (engaged_) {
            reject(LimitStatus::kill_switch);
            return Decision{LimitStatus::kill_switch};
        }

        if (quantity.is_zero()) {
            reject(LimitStatus::order_notional);
            return Decision{LimitStatus::order_notional};
        }

        // Price band. Evaluated before notional so a wildly wrong price
        // is reported as a price problem, which is what it almost
        // certainly is.
        if (!has_reference_) {
            reject(LimitStatus::no_reference_price);
            return Decision{LimitStatus::no_reference_price};
        }
        if (!within_band(price)) {
            reject(LimitStatus::price_band);
            return Decision{LimitStatus::price_band};
        }

        // Per-order notional, computed in 128 bits on the platforms that
        // have them and with an explicit overflow guard elsewhere, so
        // a fat-fingered price times a large size cannot wrap into a
        // small number and sail past the check.
        const std::int64_t notional = checked_notional(price, quantity);
        if (notional < 0) {
            reject(LimitStatus::order_notional);
            return Decision{LimitStatus::order_notional};
        }
        if (notional > limits_.max_order_notional_raw) {
            reject(LimitStatus::order_notional);
            return Decision{LimitStatus::order_notional};
        }

        // Position. The incoming order is added to the side's current
        // exposure before comparison, so a check cannot pass and then
        // leave the account over its limit.
        const std::uint64_t incoming = quantity.raw();
        if (position(side) + incoming > limits_.max_position_per_side) {
            reject(LimitStatus::position_limit);
            return Decision{LimitStatus::position_limit};
        }
        if (gross_position() + incoming > limits_.max_abs_position) {
            reject(LimitStatus::gross_limit);
            return Decision{LimitStatus::gross_limit};
        }

        if (!rate_limiter_.consume(now)) {
            reject(LimitStatus::rate_limit);
            return Decision{LimitStatus::rate_limit};
        }

        ++passed_;
        return Decision{LimitStatus::ok};
    }

    // ---- Counters ----------------------------------------------------
    //
    // A limit that fires and nobody notices is not a limit. These are
    // per-reason so an operator can see which bound is actually biting
    // instead of watching a single "rejected" counter climb.

    [[nodiscard]] std::uint64_t passed() const noexcept { return passed_; }
    [[nodiscard]] std::uint64_t rejected(LimitStatus s) const noexcept {
        return rejected_[static_cast<std::size_t>(s)];
    }
    [[nodiscard]] std::uint64_t total_rejected() const noexcept {
        std::uint64_t sum = 0;
        for (const std::uint64_t v : rejected_) {
            sum += v;
        }
        return sum;
    }

private:
    [[nodiscard]] bool within_band(Price price) const noexcept {
        // Compare as a difference to avoid any risk of comparing
        // distant magnitudes, and handle both directions explicitly.
        const std::int64_t delta = price.raw() - reference_price_.raw();
        const std::int64_t magnitude = delta < 0 ? -delta : delta;
        return magnitude <= limits_.max_price_deviation_raw;
    }

    /// price_raw * qty, in raw notional units, or -1 on overflow.
    /// Returns -1 rather than a saturated value so the caller rejects
    /// the order: an order whose notional cannot be computed is not an
    /// order that should be sent.
    [[nodiscard]] static std::int64_t checked_notional(Price price, Quantity qty) noexcept {
#if defined(__SIZEOF_INT128__)
        // `__int128` is a compiler extension, not ISO C++, and this
        // project compiles with -Wpedantic -Werror. The relaxation is
        // scoped to these four lines rather than applied to the
        // translation unit: widening to 128 bits is the right way to
        // detect a notional overflow without doing the arithmetic
        // twice, and it is guarded by a capability check with a
        // portable fallback below. Widening the pedantic setting
        // project-wide to accommodate one extension is how a second
        // extension gets added without anyone noticing.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
        const unsigned __int128 product =
            static_cast<unsigned __int128>(static_cast<std::uint64_t>(price.raw())) *
            static_cast<unsigned __int128>(qty.raw());
        const unsigned __int128 limit =
            static_cast<unsigned __int128>(static_cast<std::uint64_t>(INT64_MAX));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        return product > limit ? -1 : static_cast<std::int64_t>(product);
#else
        // Portable fallback: reject prices or sizes whose product
        // cannot fit, by checking against a conservative bound before
        // multiplying.
        const std::uint64_t p = static_cast<std::uint64_t>(price.raw());
        const std::uint64_t q = qty.raw();
        const std::uint64_t limit = static_cast<std::uint64_t>(INT64_MAX);
        if (p == 0 || q == 0) {
            return 0;
        }
        if (p > limit / q) {
            return -1;
        }
        return static_cast<std::int64_t>(p * q);
#endif
    }

    void reject(LimitStatus s) noexcept { ++rejected_[static_cast<std::size_t>(s)]; }

    Limits limits_{};
    TokenBucket rate_limiter_{};
    bool engaged_ = false;
    bool has_reference_ = false;
    Price reference_price_{};
    char reason_[64] = {};
    std::uint64_t bid_shares_ = 0;
    std::uint64_t ask_shares_ = 0;
    std::uint64_t passed_ = 0;
    std::uint64_t rejected_[8] = {};
};

}  // namespace hft::risk
