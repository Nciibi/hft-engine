-- The order book: its types, and the constants both sides share.
--
-- The state names and the FNV-1a constants are the same on both sides
-- of the differential, and that is deliberate in one direction and not
-- the other.
--
-- The FNV constants are shared because they are a published
-- algorithm, not a design: both sides compute the same well-known
-- function over the same values, and agreeing about the function is
-- agreeing about the specification, which is what the differential is
-- checking.
--
-- The state names are NOT shared, and they must not be. The C++ has an
-- enum called `OrderState` and this package has a different set of
-- names for the same five values, written from the same
-- specification. If both sides used one shared enumeration, a value
-- that meant "partially filled" on one side and "cancelled" on the
-- other would agree by construction, and the comparison would be
-- checking nothing.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package hft_book_pkg is

    -- The state of an order, in the order the checksum folds it.
    --
    -- 0 resting, never touched
    -- 1 resting, part of it executed
    -- 2 executed in full
    -- 3 cancelled
    -- 4 superseded by a replace
    --
    -- Only 0 and 1 can appear in a dump of a live book: an order that
    -- was filled, cancelled or replaced has been detached from it. The
    -- other three are carried because the two sides must not disagree
    -- about which is which.
    subtype order_state_t is natural range 0 to 4;

    constant st_resting : order_state_t := 0;
    constant st_part_executed : order_state_t := 1;
    constant st_executed : order_state_t := 2;
    constant st_cancelled : order_state_t := 3;
    constant st_superseded : order_state_t := 4;

    -- The outcome of applying one message.
    --
    -- These are the same five outcomes the C++ book reports, in the
    -- same order, written out again rather than shared. The
    -- differential does not compare statuses -- the C++ side does not
    -- report them in the dump -- but the RTL needs them, because a
    -- message the book rejects changes nothing, and the RTL has to
    -- know that.
    type book_status_t is (bs_ok, bs_unknown_order, bs_duplicate_order,
                           bs_zero_size, bs_over_reduce, bs_capacity_exhausted);

    -- FNV-1a, 64-bit. The constants are the published ones.
    constant kFnvOffsetBasis : std_logic_vector(63 downto 0) := x"CBF29CE484222325";
    constant kFnvPrime : std_logic_vector(63 downto 0) := x"00000100000001B3";

    -- Fold one 64-bit value into a running hash.
    --
    -- The bytes are folded LOW FIRST, in a fixed order, so the result
    -- does not depend on the host's endianness. A checksum that
    -- depended on the byte order of the machine that computed it would
    -- be a checksum of the machine as much as of the book.
    function fnv1a_u64(hash : std_logic_vector(63 downto 0);
                       value : std_logic_vector(63 downto 0)) return std_logic_vector;

    -- Fold a 32-bit value, zero-extended. The price is a 4-byte field
    -- on the wire and the C++ widens it to 64 bits before folding it,
    -- so this is the same fold with the same result.
    function fnv1a_u32(hash : std_logic_vector(63 downto 0);
                       value : std_logic_vector(31 downto 0)) return std_logic_vector;

    -- The hash of nothing: the basis.
    function fnv1a_basis return std_logic_vector;

end package hft_book_pkg;

package body hft_book_pkg is

    function fnv1a_u64(hash : std_logic_vector(63 downto 0);
                       value : std_logic_vector(63 downto 0)) return std_logic_vector is
        variable h : std_logic_vector(63 downto 0) := hash;
        variable byte : std_logic_vector(7 downto 0);
    begin
        for i in 0 to 7 loop
            byte := value(i * 8 + 7 downto i * 8);
            h := h xor std_logic_vector(resize(unsigned(byte), 64));
            h := std_logic_vector(unsigned(h) * unsigned(kFnvPrime));
        end loop;
        return h;
    end function;

    function fnv1a_u32(hash : std_logic_vector(63 downto 0);
                       value : std_logic_vector(31 downto 0)) return std_logic_vector is
    begin
        return fnv1a_u64(hash, std_logic_vector(resize(unsigned(value), 64)));
    end function;

    function fnv1a_basis return std_logic_vector is
    begin
        return kFnvOffsetBasis;
    end function;

end package body hft_book_pkg;

