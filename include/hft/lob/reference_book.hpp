// Naive reference implementation of the order book.
//
// This exists to be obviously correct, not fast. It shares no code and
// no data structure with the fast book: no pools, no handles, no flat
// hashing, no intrusive lists. Price priority comes free from std::map
// ordering; arrival priority comes from a deque. A differential test
// between two implementations that share a data structure proves very
// little, so the whole point is that these are structurally unrelated.
//
// It is also the executable specification. If the fast book and this
// disagree, this one is right until proven otherwise.

#pragma once

#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "hft/lob/order_book.hpp"
#include "hft/types.hpp"

namespace hft::lob {

class ReferenceBook final {
public:
    BookStatus add(Side side, Price price, Quantity size, OrderId id) {
        if (id == kInvalidOrderId || location_.count(id) != 0) {
            return BookStatus::duplicate_order;
        }
        if (size.is_zero()) {
            return BookStatus::zero_size;
        }
        books_[index(side)][price].push_back(Order{id, size, OrderState::new_order});
        location_[id] = price;
        sides_[index(side)] = side;
        return BookStatus::ok;
    }

    BookStatus execute(OrderId id, Quantity qty) {
        auto loc = location_.find(id);
        if (loc == location_.end()) {
            return BookStatus::unknown_order;
        }
        const Price price = loc->second;
        const Side side = side_of(id);
        auto& level = books_[index(side)].at(price);
        Order& o = *find_in(level, id);
        if (qty.raw() > o.size.raw()) {
            return BookStatus::over_reduce;
        }
        if (qty.raw() == o.size.raw()) {
            level.erase(find_in(level, id));
            location_.erase(loc);
            if (level.empty()) {
                books_[index(side)].erase(price);
            }
            return BookStatus::ok;
        }
        o.size = Quantity::from_raw(o.size.raw() - qty.raw());
        o.state = OrderState::partially_filled;
        return BookStatus::ok;
    }

    /// ITCH 'X': partial cancel. Deducts from remaining size and leaves
    /// the order working. Not the same as `remove`.
    BookStatus cancel_partial(OrderId id, Quantity qty) {
        auto loc = location_.find(id);
        if (loc == location_.end()) {
            return BookStatus::unknown_order;
        }
        const Price price = loc->second;
        const Side side = side_of(id);
        auto& level = books_[index(side)].at(price);
        auto pos = find_in(level, id);
        Order& o = *pos;
        if (qty.raw() > o.size.raw()) {
            return BookStatus::over_reduce;
        }
        if (qty.raw() == o.size.raw()) {
            level.erase(pos);
            location_.erase(loc);
            if (level.empty()) {
                books_[index(side)].erase(price);
            }
            return BookStatus::ok;
        }
        o.size = Quantity::from_raw(o.size.raw() - qty.raw());
        o.state = OrderState::new_order;
        return BookStatus::ok;
    }

    /// ITCH 'D': remove the order entirely, discarding any remainder.
    BookStatus remove(OrderId id, Quantity* discarded = nullptr) {
        auto loc = location_.find(id);
        if (loc == location_.end()) {
            return BookStatus::unknown_order;
        }
        const Price price = loc->second;
        const Side side = side_of(id);
        auto& level = books_[index(side)].at(price);
        auto pos = find_in(level, id);
        if (discarded != nullptr) {
            *discarded = pos->size;
        }
        level.erase(pos);
        location_.erase(loc);
        if (level.empty()) {
            books_[index(side)].erase(price);
        }
        return BookStatus::ok;
    }

    [[nodiscard]] std::optional<Price> best_bid() const { return extreme(Side::bid); }
    [[nodiscard]] std::optional<Price> best_ask() const { return extreme(Side::ask); }

    [[nodiscard]] std::optional<Quantity> size_at(Side side, Price price) const {
        const auto& book = books_[index(side)];
        auto it = book.find(price);
        if (it == book.end()) {
            return std::nullopt;
        }
        Quantity total{};
        for (const Order& o : it->second) {
            total = Quantity::from_raw(total.raw() + o.size.raw());
        }
        return total;
    }

    /// Best first, matching the fast book's ladder order. Bids are
    /// walked from the highest price down; asks from the lowest up.
    /// std::map<Price> is ascending, so those are opposite directions.
    [[nodiscard]] std::vector<LevelSnapshot> levels(Side side) const {
        std::vector<LevelSnapshot> out;
        for_each_level_ordered(side, [&](Price price, const std::deque<Order>& resting) {
            Quantity total{};
            for (const Order& o : resting) {
                total = Quantity::from_raw(total.raw() + o.size.raw());
            }
            out.push_back(
                LevelSnapshot{price, total, static_cast<std::uint32_t>(resting.size())});
        });
        return out;
    }

    /// Price priority, then arrival order: the same sequence the fast
    /// book produces, so the two can be compared element by element.
    [[nodiscard]] std::vector<OrderSnapshot> orders(Side side) const {
        std::vector<OrderSnapshot> out;
        for_each_level_ordered(side, [&](Price price, const std::deque<Order>& resting) {
            for (const Order& o : resting) {
                out.push_back(OrderSnapshot{o.id, price, o.size, o.state});
            }
        });
        return out;
    }

private:
    struct Order {
        OrderId id;
        Quantity size;
        OrderState state;
    };

    using Book = std::map<Price, std::deque<Order>>;

    /// Visit each price level in display priority: best first.
    template <class F>
    void for_each_level_ordered(Side side, F&& visit) const {
        const Book& book = books_[index(side)];
        if (side == Side::bid) {
            for (auto it = book.rbegin(); it != book.rend(); ++it) {
                visit(it->first, it->second);
            }
        } else {
            for (auto it = book.cbegin(); it != book.cend(); ++it) {
                visit(it->first, it->second);
            }
        }
    }

    static constexpr int index(Side s) noexcept {
        return s == Side::bid ? 0 : 1;
    }

    [[nodiscard]] static std::deque<Order>::iterator find_in(std::deque<Order>& level,
                                                            OrderId id) {
        for (auto it = level.begin(); it != level.end(); ++it) {
            if (it->id == id) {
                return it;
            }
        }
        return level.end();
    }

    [[nodiscard]] Side side_of(OrderId id) const { return sides_.at(id); }

    [[nodiscard]] std::optional<Price> extreme(Side side) const {
        const auto& book = books_[index(side)];
        if (book.empty()) {
            return std::nullopt;
        }
        // std::map<Price> is ascending, so the best bid is the LAST
        // element and the best ask is the FIRST. This asymmetry is why
        // the fast book keeps two separately-ordered ladders.
        return side == Side::bid ? std::optional<Price>{book.rbegin()->first}
                                : std::optional<Price>{book.begin()->first};
    }

    Book books_[2]{};
    std::map<OrderId, Price> location_{};
    std::map<OrderId, Side> sides_{};
};

}  // namespace hft::lob
