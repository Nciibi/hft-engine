-- The order book: a price-time-priority book, in hardware.
--
-- This is the RTL side of the order-book differential. The C++ side is
-- `include/hft/lob/order_book.hpp` and `include/hft/lob/apply.hpp`, and
-- the two are compared by dumping both books and diffing them, record
-- by record and field by field.
--
-- ---- The structure --------------------------------------------------
--
-- The C++ book has two ladders: a sparse one, which is a hash index
-- for the price plus a price-ordered list of levels, and a dense one,
-- which is a direct-indexed grid with an occupancy bitmap. This models
-- both. DENSE selects the grid; with it off the book uses the sparse
-- list, which is the default and the regression path. The dump is the
-- same either way, so the two sides stay comparable without configuring
-- either.
--
-- So the structures are:
--
--   * An ORDER POOL. A fixed array of order records, with a free
--     list threaded through the records themselves. Orders are
--     referenced by handle, never by pointer, and a handle is an
--     integer offset into the array.
--
--   * A LEVEL POOL. The same shape, for price levels.
--
--   * A PRICE-ORDERED LIST per side. A doubly-linked list of levels,
--     ordered by price: descending for the bid side, ascending for the
--     ask. This is the order the dump is in, and it is the order
--     price-time priority defines.
--
--   * A QUEUE per level. A FIFO of orders, threaded through the order
--     records: each order knows its predecessor and successor within
--     its level. The head of the queue is the highest priority order
--     at that price.
--
--   * AN ORDER-REFERENCE INDEX. An open-addressed hash table from the
--     ITCH order reference to the order's handle.
--
-- ---- The index is insert-only, and that is deliberate ---------------
--
-- ITCH order references are unique for the trading day, so an
-- identifier that has been used is never used again, and a reference
-- whose order has left the book will never be looked up again
-- successfully. The index therefore never erases: a slot that has been
-- written keeps its key, and a lookup that finds a dead key reports
-- "not present".
--
-- That is not an approximation of the C++ index, which does erase and
-- does accumulate tombstones -- and pays for them. The C++ book had a
-- defect where the table wedged permanently once every slot was
-- occupied or tombstoned, and a book that traded a few hundred million
-- shares would stop accepting orders. An insert-only table cannot
-- wedge in that way, because it never needs to reclaim a slot.
--
-- ---- The cost, stated plainly ---------------------------------------
--
-- With DENSE off, finding a price is a walk down the list, and the walk
-- is O(depth). The C++ has the same asymptotics for its sparse ladder
-- and measures the walk at up to 178x a touch insert at 3,200 levels;
-- with DENSE on, the grid removes it, and the best and next slots come
-- from an occupancy bitmap. With the grid off the walk costs one clock
-- cycle per level, so the same number that the C++ reports as a ratio is
-- here a cycle count.
--
-- That is the honest result and it is a useful one: it says the dense
-- grid is worth exactly `depth` cycles per add, which is a number a
-- hardware designer can act on and a ratio on a desktop cannot be.
--
-- ---- The interface ---------------------------------------------------
--
-- One message at a time. The caller presents a decoded message and
-- waits for `apply_done`. There is no backpressure and no queue
-- inside: the testbench decodes the capture first and then applies the
-- messages, so the two rates are independent and the book can be
-- measured on its own.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

library work;
use work.hft_itch_pkg.all;
use work.hft_book_pkg.all;

entity hft_book is
    generic (
        MAX_ORDERS  : positive := 4096;
        MAX_LEVELS  : positive := 2048;
        TABLE_SLOTS : positive := 8192;
        DENSE       : boolean := false;
        PRICE_FLOOR : natural := 0;
        PRICE_TICK  : positive := 1;
        DENSE_SLOTS : positive := 4096
    );
    port (
        clk : in std_logic;
        rst : in std_logic;

        -- Apply one decoded message.
        apply_start  : in std_logic;
        apply_type   : in std_logic_vector(7 downto 0);
        apply_fields : in field_array_t;

        apply_done   : out std_logic;
        apply_status : out book_status_t;
        -- The clock cycles this apply took, from the message being
        -- presented to the result being ready.
        --
        -- A property of the DESIGN, not of the machine: the same
        -- number on any simulator, on any host, for ever. The C++ can
        -- only report the same thing as a ratio, and a ratio is a
        -- statement about a machine rather than about a design.
        apply_cycles : out natural range 0 to natural'high;

        -- Dump the book, in priority order.
        dump_start : in std_logic;
        dump_valid : out std_logic;
        dump_kind  : out std_logic_vector(7 downto 0);  -- x"4C" for a level, x"4F" for an order
        dump_side  : out std_logic_vector(7 downto 0);  -- x"42" for the bid side, x"41" for the ask
        dump_a     : out std_logic_vector(63 downto 0);  -- level: price      order: id
        dump_b     : out std_logic_vector(63 downto 0);  -- level: aggregate  order: price
        dump_c     : out std_logic_vector(63 downto 0);  -- level: order count order: size
        dump_state : out order_state_t;                 -- order only
        dump_pos   : out natural;                        -- order only
        dump_done  : out std_logic
    );
end entity hft_book;

architecture rtl of hft_book is

    -- ---- The record types --------------------------------------------

    type order_rec_t is record
        id    : std_logic_vector(63 downto 0);
        price : std_logic_vector(63 downto 0);
        size  : std_logic_vector(63 downto 0);
        state : order_state_t;
        side  : std_logic;
        level : natural range 0 to MAX_LEVELS;   -- handle + 1
        q_prev : natural range 0 to MAX_ORDERS;  -- handle + 1, within the level's queue
        q_next : natural range 0 to MAX_ORDERS;  -- handle + 1, within the level's queue
        live  : std_logic;
    end record;

    type level_rec_t is record
        price       : std_logic_vector(63 downto 0);
        head        : natural range 0 to MAX_ORDERS;  -- handle + 1
        tail        : natural range 0 to MAX_ORDERS;  -- handle + 1
        order_count : natural range 0 to natural'high;
        aggregate   : std_logic_vector(63 downto 0);
        l_prev      : natural range 0 to MAX_LEVELS;  -- handle + 1
        l_next      : natural range 0 to MAX_LEVELS;  -- handle + 1
        side        : std_logic;
        live        : std_logic;
    end record;

    type id_entry_t is record
        key    : std_logic_vector(63 downto 0);
        handle : natural range 0 to MAX_ORDERS;  -- handle + 1
        live   : std_logic;
        used   : std_logic;
    end record;

    type order_pool_t is array (0 to MAX_ORDERS - 1) of order_rec_t;
    type level_pool_t is array (0 to MAX_LEVELS - 1) of level_rec_t;
    type id_table_t is array (0 to TABLE_SLOTS - 1) of id_entry_t;

    -- ---- The dense price grid -----------------------------------------
    --
    -- A level is found by arithmetic on the price instead of by walking
    -- the sparse list. The grid covers a contiguous band of prices that
    -- lie on PRICE_TICK boundaries. A price outside the band, or one
    -- that is not on a tick boundary, falls back to the list, so the
    -- book is still correct when the market moves past the configured
    -- window or uses a finer price than the grid.
    --
    -- A slot stores the same handle the sparse list stores, and zero
    -- means empty. The bitmap answers "is any slot in this 64-price
    -- word occupied?", so finding the best dense price does not walk
    -- the slot array. The hint is the extreme occupied word: the
    -- highest word for a bid, the lowest for an ask.
    constant DENSE_WORDS : positive := (DENSE_SLOTS + 63) / 64;
    type dense_slot_t is array (0 to DENSE_SLOTS - 1) of natural range 0 to MAX_LEVELS;
    type dense_bits_t is array (0 to DENSE_WORDS - 1) of std_logic_vector(63 downto 0);

    -- ---- The state machine --------------------------------------------

    type op_t is (op_none, op_add, op_execute, op_cancel, op_remove, op_replace);

    type book_state_t is (
        st_idle,
        st_probe,       -- walking the order-reference index
        st_walk,        -- walking the price-ordered level list
        st_new_level,   -- allocating and linking a level
        st_new_order,   -- allocating and linking an order
        st_place,       -- writing the order-reference index
        st_reduce,      -- consuming or cancelling shares
        st_detach,      -- taking an order off the book
        st_finish,      -- the result is ready
        st_dump_level,  -- emitting a level
        st_dump_order   -- emitting an order
    );

    signal o_done : std_logic := '0';
    signal o_status : book_status_t := bs_ok;
    signal o_cycles : natural range 0 to natural'high := 0;
    signal o_dump_valid : std_logic := '0';
    signal o_dump_kind : std_logic_vector(7 downto 0) := x"4C";
    signal o_dump_side : std_logic_vector(7 downto 0) := x"42";
    signal o_dump_a : std_logic_vector(63 downto 0) := (others => '0');
    signal o_dump_b : std_logic_vector(63 downto 0) := (others => '0');
    signal o_dump_c : std_logic_vector(63 downto 0) := (others => '0');
    signal o_dump_state : order_state_t := st_resting;
    signal o_dump_pos : natural := 0;
    signal o_dump_done : std_logic := '0';

    -- Does `candidate` rank better than `reference` on this side?
    --
    -- The side encoding is '1' for the bid side and '0' for the ask
    -- side, and the two are not symmetric in the encoding: a bid ranks
    -- better when its price is HIGHER and an ask ranks better when its
    -- price is LOWER.
    --
    -- Getting this backwards does not produce a wrong answer, which is
    -- what makes it worth writing down. It produces a book that is
    -- internally consistent and in the wrong order: the ladder is
    -- perfectly sorted, just sorted the other way up, and every field
    -- of every order is exactly right. The differential catches it
    -- because the ORDER is part of the state.
    function is_better(side : std_logic;
                       candidate : std_logic_vector(63 downto 0);
                       reference : std_logic_vector(63 downto 0)) return boolean is
    begin
        if side = '1' then
            return unsigned(candidate) > unsigned(reference);
        end if;
        return unsigned(candidate) < unsigned(reference);
    end function;

    function is_zero(word : std_logic_vector(63 downto 0)) return boolean is
    begin
        return word = (word'range => '0');
    end function;

    function lowest_set_bit(word : std_logic_vector(63 downto 0)) return natural is
    begin
        for b in 0 to 63 loop
            if word(b) = '1' then
                return b;
            end if;
        end loop;
        return 0;
    end function;

    function highest_set_bit(word : std_logic_vector(63 downto 0)) return natural is
    begin
        for b in 63 downto 0 loop
            if word(b) = '1' then
                return b;
            end if;
        end loop;
        return 0;
    end function;

    -- The slot for a price, offset by one so that zero means "outside
    -- the dense band". PRICE_FLOOR and PRICE_TICK are expressed in raw
    -- price units, the same units the wire carries. A price that is not
    -- on a tick boundary is not addressable in a grid, so it falls back
    -- to the sparse list rather than being rounded to a neighbour.
    function ladder_slot(price : std_logic_vector(63 downto 0)) return natural is
        constant tick_width : unsigned(63 downto 0) := to_unsigned(PRICE_TICK, 64);
        variable offset : unsigned(63 downto 0);
    begin
        if not DENSE then
            return 0;
        end if;
        if unsigned(price) < to_unsigned(PRICE_FLOOR, 64) then
            return 0;
        end if;
        offset := unsigned(price) - to_unsigned(PRICE_FLOOR, 64);
        if offset mod tick_width /= to_unsigned(0, 64) then
            return 0;
        end if;
        offset := offset / tick_width;
        if offset >= to_unsigned(DENSE_SLOTS, 64) then
            return 0;
        end if;
        return to_integer(offset) + 1;
    end function;

    -- The best occupied slot on a side, offset by one. The hint is the
    -- extreme word, so the scan starts at the end that can win instead
    -- of at the start of the grid.
    function best_dense_slot(bits : dense_bits_t;
                             side : std_logic;
                             hint : natural range 0 to DENSE_WORDS) return natural is
    begin
        if hint = 0 then
            return 0;
        end if;
        if side = '1' then
            for w in hint - 1 downto 0 loop
                if not is_zero(bits(w)) then
                    return w * 64 + highest_set_bit(bits(w)) + 1;
                end if;
            end loop;
        else
            for w in hint - 1 to DENSE_WORDS - 1 loop
                if not is_zero(bits(w)) then
                    return w * 64 + lowest_set_bit(bits(w)) + 1;
                end if;
            end loop;
        end if;
        return 0;
    end function;

    -- The next occupied slot after `from_slot`, in price priority order.
    -- This is the dense-grid equivalent of following a level's l_next.
    function next_dense_slot(bits : dense_bits_t;
                             from_slot : natural;
                             side : std_logic) return natural is
        variable slot_index : natural;
        variable word_index : natural;
    begin
        if from_slot = 0 then
            return 0;
        end if;
        slot_index := from_slot - 1;
        word_index := slot_index / 64;
        if side = '1' then
            for b in (slot_index mod 64) - 1 downto 0 loop
                if bits(word_index)(b) = '1' then
                    return word_index * 64 + b + 1;
                end if;
            end loop;
            if word_index > 0 then
                for w in word_index - 1 downto 0 loop
                    if not is_zero(bits(w)) then
                        return w * 64 + highest_set_bit(bits(w)) + 1;
                    end if;
                end loop;
            end if;
        else
            for b in (slot_index mod 64) + 1 to 63 loop
                if bits(word_index)(b) = '1' then
                    return word_index * 64 + b + 1;
                end if;
            end loop;
            if word_index + 1 < DENSE_WORDS then
                for w in word_index + 1 to DENSE_WORDS - 1 loop
                    if not is_zero(bits(w)) then
                        return w * 64 + lowest_set_bit(bits(w)) + 1;
                    end if;
                end loop;
            end if;
        end if;
        return 0;
    end function;

    -- The side byte, as the wire carries it: 'B' or 'S'.
    function side_is_bid(side_byte : std_logic_vector(63 downto 0)) return boolean is
    begin
        return side_byte(7 downto 0) = x"42";
    end function;

    -- The side, as the dump spells it: x"42" for the bid side, x"41"
    -- for the ask side.
    function side_char(side : std_logic) return std_logic_vector is
    begin
        if side = '1' then
            return x"42";
        end if;
        return x"41";
    end function;

    -- The index slot for an order reference.
    --
    -- The hash does not have to match the C++'s: only the RESULT of a
    -- lookup has to match, and any hash with open addressing gives the
    -- same answer for the same key. This one is a multiply by the
    -- 64-bit golden ratio, which spreads sequential references -- and
    -- ITCH references are sequential -- across the table instead of
    -- colliding on every one of them.
    function hash_slot(key : std_logic_vector(63 downto 0)) return natural is
        constant kHashMix : unsigned(31 downto 0) := to_unsigned(16#7FEB352D#, 32);
        variable mixed : unsigned(31 downto 0);
    begin
        -- The multiply is wider than the result and is truncated back
        -- to 32 bits, which is what a 32-bit mixer does anyway.
        --
        -- And the result is narrowed to 31 bits before the conversion,
        -- because a 32-bit unsigned can be 4,294,967,295 and a VHDL
        -- integer stops at 2,147,483,647. `to_integer` on a value that
        -- does not fit is an overflow, not a wrap.
        mixed := resize(unsigned(key(31 downto 0)) * kHashMix, 32);
        return to_integer(resize(mixed, 31)) mod TABLE_SLOTS;
    end function;

begin

    apply_done <= o_done;
    apply_status <= o_status;
    apply_cycles <= o_cycles;
    dump_valid <= o_dump_valid;
    dump_kind <= o_dump_kind;
    dump_side <= o_dump_side;
    dump_a <= o_dump_a;
    dump_b <= o_dump_b;
    dump_c <= o_dump_c;
    dump_state <= o_dump_state;
    dump_pos <= o_dump_pos;
    dump_done <= o_dump_done;

    process (clk)
        -- ---- The memory ------------------------------------------------
        --
        -- Process variables, not signals. A signal array is copied
        -- whole on every read, so a 4096-entry signal array would cost
        -- a megabyte-scale copy per clock cycle in simulation; a
        -- variable array is read in place. The memory is owned by this
        -- one process, which is also the right shape for the design:
        -- there is exactly one writer.

        variable orders : order_pool_t;
        variable levels : level_pool_t;
        variable id_tab : id_table_t;

        variable bid_slots : dense_slot_t;
        variable ask_slots : dense_slot_t;
        variable bid_bits : dense_bits_t;
        variable ask_bits : dense_bits_t;
        variable bid_hint : natural range 0 to DENSE_WORDS := 0;
        variable ask_hint : natural range 0 to DENSE_WORDS := 0;

        variable order_free : natural range 0 to MAX_ORDERS := 0;
        variable level_free : natural range 0 to MAX_LEVELS := 0;
        variable bid_head : natural range 0 to MAX_LEVELS := 0;
        variable ask_head : natural range 0 to MAX_LEVELS := 0;

        variable state : book_state_t := st_idle;

        -- The clock cycles the current apply has taken. The state
        -- machine is not idle while a message is being applied, so
        -- this is simply the number of cycles it has been out of
        -- st_idle.
        variable cycle_count : natural range 0 to natural'high := 0;

        -- The message being applied.
        variable op : op_t := op_none;
        variable msg_id : std_logic_vector(63 downto 0) := (others => '0');
        variable msg_new_id : std_logic_vector(63 downto 0) := (others => '0');
        variable msg_price : std_logic_vector(63 downto 0) := (others => '0');
        variable msg_size : std_logic_vector(63 downto 0) := (others => '0');
        variable msg_side : std_logic := '0';

        -- The index probe.
        variable probe_slot : natural range 0 to TABLE_SLOTS - 1 := 0;
        variable probe_steps : natural range 0 to TABLE_SLOTS := 0;
        variable probe_hit : std_logic := '0';
        -- Set when the probe has an answer: the key was found, or an
        -- unused slot was reached.
        variable probe_done : std_logic := '0';

        -- The level-list walk.
        variable walk_cur : natural range 0 to MAX_LEVELS := 0;
        variable walk_prev : natural range 0 to MAX_LEVELS := 0;
        variable walk_level : natural range 0 to MAX_LEVELS := 0;
        variable work_slot : natural range 0 to DENSE_SLOTS := 0;
        variable level_slot : natural range 0 to DENSE_SLOTS := 0;
        variable level_is_dense : boolean := false;

        -- The order being worked on.
        variable work_order : natural range 0 to MAX_ORDERS := 0;
        variable work_level : natural range 0 to MAX_LEVELS := 0;

        -- The dump walk. A sparse level is followed through l_next; a
        -- dense level is followed through the bitmap. The two sources
        -- are merged so the dump is still in price priority order.
        variable dump_cur_level : natural range 0 to MAX_LEVELS := 0;
        variable dump_cur_order : natural range 0 to MAX_ORDERS := 0;
        variable dump_walk_side : std_logic := '0';
        variable dump_position : natural := 0;
        variable dump_sparse_level : natural range 0 to MAX_LEVELS := 0;
        variable dump_dense_slot : natural range 0 to DENSE_SLOTS := 0;
        variable dump_cur_dense : std_logic := '0';
        variable picked_level : natural range 0 to MAX_LEVELS := 0;

    begin
        if rising_edge(clk) then
            if rst = '1' then
                -- Initialise the pools and the free lists.
                --
                -- The free list is threaded through the records' own
                -- `next` field, so a free record's `next` says which
                -- record is free after it. Handles are stored offset by
                -- one, so that zero means "none" and a real handle of
                -- zero is representable.
                for i in 0 to MAX_ORDERS - 2 loop
                    orders(i).q_next := i + 2;
                    orders(i).live := '0';
                end loop;
                orders(MAX_ORDERS - 1).q_next := 0;
                orders(MAX_ORDERS - 1).live := '0';
                order_free := 1;

                for i in 0 to MAX_LEVELS - 2 loop
                    levels(i).l_next := i + 2;
                    levels(i).live := '0';
                end loop;
                levels(MAX_LEVELS - 1).l_next := 0;
                levels(MAX_LEVELS - 1).live := '0';
                level_free := 1;

                for i in 0 to TABLE_SLOTS - 1 loop
                    id_tab(i).used := '0';
                    id_tab(i).live := '0';
                    id_tab(i).handle := 0;
                    id_tab(i).key := (others => '0');
                end loop;

                for i in 0 to DENSE_SLOTS - 1 loop
                    bid_slots(i) := 0;
                    ask_slots(i) := 0;
                end loop;
                for i in 0 to DENSE_WORDS - 1 loop
                    bid_bits(i) := (others => '0');
                    ask_bits(i) := (others => '0');
                end loop;

                bid_head := 0;
                ask_head := 0;
                bid_hint := 0;
                ask_hint := 0;
                dump_cur_level := 0;
                dump_cur_order := 0;
                dump_sparse_level := 0;
                dump_dense_slot := 0;
                dump_cur_dense := '0';
                state := st_idle;
                o_done  <= '0';
                o_status  <= bs_ok;
                o_dump_valid  <= '0';
                o_dump_done  <= '0';
            else
                o_done <= '0';
                o_dump_valid <= '0';
                o_dump_done <= '0';

                -- The state machine is not idle while a message is
                -- being applied, so this is the number of clock cycles
                -- the current apply has taken.
                --
                -- It is counted BEFORE the state machine runs, so the
                -- cycle the result is produced on is counted too: the
                -- number reported is the number of cycles from the
                -- message being presented to the result being ready.
                if state /= st_idle then
                    cycle_count := cycle_count + 1;
                end if;

                case state is

                    when st_idle =>
                        if dump_start = '1' then
                            dump_walk_side := '0';
                            dump_cur_level := 0;
                            dump_cur_order := 0;
                            dump_sparse_level := bid_head;
                            dump_dense_slot := best_dense_slot(bid_bits, '1', bid_hint);
                            dump_cur_dense := '0';
                            dump_position := 0;
                            state := st_dump_level;
                        elsif apply_start = '1' then
                            cycle_count := 0;
                            -- Dispatch on the tag.
                            --
                            -- The field indices are the ones the trace
                            -- format fixes, so the book reads the same
                            -- fields the trace records and the C++
                            -- decoder produces.
                            case apply_type is
                                when x"41" =>            -- 'A' Add Order
                                    op := op_add;
                                    msg_id := apply_fields(3);
                                    if side_is_bid(apply_fields(4)) then
                                        msg_side := '1';
                                    else
                                        msg_side := '0';
                                    end if;
                                    msg_size := apply_fields(5);
                                    msg_price := apply_fields(7);
                                when x"45" | x"43" =>  -- 'E' / 'C' executed
                                    op := op_execute;
                                    msg_id := apply_fields(3);
                                    msg_size := apply_fields(4);
                                when x"58" =>            -- 'X' cancel
                                    op := op_cancel;
                                    msg_id := apply_fields(3);
                                    msg_size := apply_fields(4);
                                when x"44" =>            -- 'D' delete
                                    op := op_remove;
                                    msg_id := apply_fields(3);
                                when x"55" =>            -- 'U' replace
                                    op := op_replace;
                                    msg_id := apply_fields(3);
                                    msg_new_id := apply_fields(4);
                                    msg_size := apply_fields(5);
                                    msg_price := apply_fields(6);
                                when others =>
                                    -- 'B', 'P', 'Q' and anything else:
                                    -- the specification says they do
                                    -- not affect the book.
                                    op := op_none;
                            end case;

                            if op = op_none then
                                o_status  <= bs_ok;
                                state := st_finish;
                            else
                                -- Probe the order-reference index.
                                -- The hash is a mix of the key; it does
                                -- not have to match the C++'s, because
                                -- only the RESULT has to match.
                                probe_slot := hash_slot(msg_id);
                                probe_steps := 0;
                                probe_done := '0';
                                state := st_probe;
                            end if;
                        end if;

                    when st_probe =>
                        -- Walk the index until the key is found or an
                        -- unused slot is reached.
                        --
                        -- The probe and the USE of its result are
                        -- separate steps, and they have to be. An
                        -- earlier version advanced to the next slot on
                        -- a collision and then fell straight through
                        -- to using it -- so a lookup that collided
                        -- read a slot that had never been examined and
                        -- took a handle from it that was not the
                        -- order's.
                        --
                        -- It never fired on a generated capture, because
                        -- the generator's order references are
                        -- sequential and the hash spreads them: a
                        -- collision needs two references that differ by
                        -- a multiple of the table size. The exchange's
                        -- references are not sequential, and it fired on
                        -- the first real capture. See docs/BUGS.md.
                        if probe_done = '0' then
                            if id_tab(probe_slot).used = '1' and
                               id_tab(probe_slot).key = msg_id then
                                probe_hit := id_tab(probe_slot).live;
                                probe_done := '1';
                            elsif id_tab(probe_slot).used = '0' then
                                probe_hit := '0';
                                probe_done := '1';
                            else
                                probe_steps := probe_steps + 1;
                                if probe_steps >= TABLE_SLOTS then
                                    -- The index is full. The C++ reports
                                    -- capacity exhaustion in the same
                                    -- situation.
                                    o_status <= bs_capacity_exhausted;
                                    state := st_finish;
                                else
                                    probe_slot := (probe_slot + 1) mod TABLE_SLOTS;
                                end if;
                            end if;
                        elsif op = op_add then
                            if probe_hit = '1' then
                                -- The reference is already live.
                                -- ITCH references are day-unique,
                                -- so a repeat is a replay or
                                -- sequence fault.
                                o_status <= bs_duplicate_order;
                                state := st_finish;
                            elsif msg_id = x"0000000000000000" then
                                -- The reference zero is reserved.
                                o_status <= bs_duplicate_order;
                                state := st_finish;
                            elsif msg_size = x"0000000000000000" then
                                -- Zero size rests no liquidity. The
                                -- venue drops these.
                                o_status <= bs_zero_size;
                                state := st_finish;
                            else
                                -- In-band prices go straight to their
                                -- grid slot; everything else walks the
                                -- sparse list.
                                walk_cur := 0;
                                walk_prev := 0;
                                work_slot := ladder_slot(msg_price);
                                if work_slot /= 0 then
                                    if msg_side = '1' then
                                        walk_level := bid_slots(work_slot - 1);
                                    else
                                        walk_level := ask_slots(work_slot - 1);
                                    end if;
                                    if walk_level /= 0 then
                                        state := st_new_order;
                                    else
                                        state := st_new_level;
                                    end if;
                                else
                                    if msg_side = '1' then
                                        walk_cur := bid_head;
                                    else
                                        walk_cur := ask_head;
                                    end if;
                                    state := st_walk;
                                end if;
                            end if;
                        else
                            -- A lookup: execute, cancel, remove or
                            -- replace.
                            if probe_hit = '0' then
                                o_status <= bs_unknown_order;
                                state := st_finish;
                            else
                                work_order := id_tab(probe_slot).handle;
                                work_level := orders(work_order - 1).level;
                                if op = op_replace then
                                    -- The replacement inherits the
                                    -- original's side: 'U' carries
                                    -- no side, no stock and no MPID.
                                    msg_side := orders(work_order - 1).side;
                                end if;
                                state := st_reduce;
                            end if;
                        end if;
                    when st_walk =>
                        -- Walk the price-ordered list until a level that
                        -- does not rank better than this price.
                        --
                        -- One level per cycle. The walk is the cost this
                        -- design is paying, and it is measured rather
                        -- than asserted.
                        if walk_cur = 0 then
                            -- The list ends: the price is new, and it
                            -- goes at the end.
                            walk_level := 0;
                            state := st_new_level;
                        elsif is_better(msg_side, levels(walk_cur - 1).price, msg_price) then
                            walk_prev := walk_cur;
                            walk_cur := levels(walk_cur - 1).l_next;
                            state := st_walk;
                        elsif levels(walk_cur - 1).price = msg_price then
                            -- The level exists.
                            walk_level := walk_cur;
                            state := st_new_order;
                        else
                            -- The price is new, and it goes here.
                            walk_level := 0;
                            state := st_new_level;
                        end if;

                    when st_new_level =>
                        -- Allocate a level and link it between the two
                        -- the walk found.
                        if level_free = 0 then
                            o_status  <= bs_capacity_exhausted;
                            state := st_finish;
                        else
                            walk_level := level_free;
                            level_free := levels(walk_level - 1).l_next;

                            levels(walk_level - 1).price := msg_price;
                            levels(walk_level - 1).head := 0;
                            levels(walk_level - 1).tail := 0;
                            levels(walk_level - 1).order_count := 0;
                            levels(walk_level - 1).aggregate := (others => '0');
                            levels(walk_level - 1).l_prev := 0;
                            levels(walk_level - 1).l_next := 0;
                            levels(walk_level - 1).side := msg_side;
                            levels(walk_level - 1).live := '1';

                            if work_slot /= 0 then
                                -- The level lives in the dense grid and
                                -- not in the sparse list.
                                if msg_side = '1' then
                                    bid_slots(work_slot - 1) := walk_level;
                                    bid_bits((work_slot - 1) / 64)((work_slot - 1) mod 64) := '1';
                                    if bid_hint = 0 or ((work_slot - 1) / 64) + 1 > bid_hint then
                                        bid_hint := ((work_slot - 1) / 64) + 1;
                                    end if;
                                else
                                    ask_slots(work_slot - 1) := walk_level;
                                    ask_bits((work_slot - 1) / 64)((work_slot - 1) mod 64) := '1';
                                    if ask_hint = 0 or ((work_slot - 1) / 64) + 1 < ask_hint then
                                        ask_hint := ((work_slot - 1) / 64) + 1;
                                    end if;
                                end if;
                            else
                                -- The level is outside the dense band:
                                -- link it into the sparse priority list.
                                levels(walk_level - 1).l_prev := walk_prev;
                                levels(walk_level - 1).l_next := walk_cur;

                                if walk_prev = 0 then
                                    if msg_side = '1' then
                                        bid_head := walk_level;
                                    else
                                        ask_head := walk_level;
                                    end if;
                                else
                                    levels(walk_prev - 1).l_next := walk_level;
                                end if;
                                if walk_cur /= 0 then
                                    levels(walk_cur - 1).l_prev := walk_level;
                                end if;
                            end if;

                            state := st_new_order;
                        end if;

                    when st_new_order =>
                        -- Allocate an order and put it at the BACK of
                        -- the level's queue.
                        --
                        -- The back, because price-time priority is
                        -- time priority within a price: a new order at
                        -- a price that already has orders sorts behind
                        -- all of them.
                        if order_free = 0 then
                            o_status  <= bs_capacity_exhausted;
                            state := st_finish;
                        else
                            work_order := order_free;
                            order_free := orders(work_order - 1).q_next;

                            orders(work_order - 1).id := msg_id;
                            orders(work_order - 1).price := msg_price;
                            orders(work_order - 1).size := msg_size;
                            orders(work_order - 1).state := st_resting;
                            orders(work_order - 1).side := msg_side;
                            orders(work_order - 1).level := walk_level;
                            orders(work_order - 1).q_prev := levels(walk_level - 1).tail;
                            orders(work_order - 1).q_next := 0;
                            orders(work_order - 1).live := '1';

                            if levels(walk_level - 1).tail = 0 then
                                levels(walk_level - 1).head := work_order;
                            else
                                orders(levels(walk_level - 1).tail - 1).q_next := work_order;
                            end if;
                            levels(walk_level - 1).tail := work_order;
                            levels(walk_level - 1).order_count :=
                                levels(walk_level - 1).order_count + 1;
                            levels(walk_level - 1).aggregate :=
                                std_logic_vector(unsigned(levels(walk_level - 1).aggregate) +
                                                unsigned(msg_size));

                            state := st_place;
                        end if;

                    when st_place =>
                        -- Write the order reference into the index.
                        id_tab(probe_slot).key := msg_id;
                        id_tab(probe_slot).handle := work_order;
                        id_tab(probe_slot).live := '1';
                        id_tab(probe_slot).used := '1';
                        o_status  <= bs_ok;
                        state := st_finish;

                    when st_reduce =>
                        -- Consume or cancel shares.
                        --
                        -- A replace is not a reduce: the specification
                        -- supersedes the original outright, whatever
                        -- its remaining size, and issues a NEW
                        -- reference. Reducing it by the replacement's
                        -- share count would be inventing a quantity
                        -- that is on neither message.
                        --
                        -- And a delete is not a reduce either. It
                        -- removes the order outright and discards
                        -- whatever is left of it, and it carries no
                        -- quantity at all -- which is exactly why
                        -- letting it fall through to the reduce path
                        -- would apply a STALE share count to it. The
                        -- order would survive, and so would the level
                        -- it was the last order behind.
                        if op = op_replace or op = op_remove then
                            orders(work_order - 1).state := st_cancelled;
                            state := st_detach;
                        elsif unsigned(msg_size) > unsigned(orders(work_order - 1).size) then
                            -- A reduce that would drive the remaining
                            -- size below zero is a protocol violation.
                            o_status  <= bs_over_reduce;
                            state := st_finish;
                        elsif msg_size = orders(work_order - 1).size then
                            -- The order is finished. Leave the size
                            -- intact: detaching subtracts it from the
                            -- level's aggregate, and clearing it first
                            -- would subtract zero and leave the level
                            -- inflated by this order's size for ever.
                            if op = op_execute then
                                orders(work_order - 1).state := st_executed;
                            else
                                orders(work_order - 1).state := st_cancelled;
                            end if;
                            state := st_detach;
                        else
                            -- A partial reduce. The order keeps its
                            -- position in the queue: a partially
                            -- filled order is not demoted to the back
                            -- of its price level.
                            orders(work_order - 1).size :=
                                std_logic_vector(unsigned(orders(work_order - 1).size) -
                                                unsigned(msg_size));
                            levels(work_level - 1).aggregate :=
                                std_logic_vector(unsigned(levels(work_level - 1).aggregate) -
                                                unsigned(msg_size));
                            if op = op_execute then
                                orders(work_order - 1).state := st_part_executed;
                            else
                                -- A partial cancel. Nothing was
                                -- executed, so the order goes back to
                                -- resting, and it keeps its place: a
                                -- cancellation is not a promotion.
                                orders(work_order - 1).state := st_resting;
                            end if;
                            o_status  <= bs_ok;
                            state := st_finish;
                        end if;

                    when st_detach =>
                        -- Take the order off the book.
                        --
                        -- Unlink it from its level's queue, take its
                        -- remaining size off the level's aggregate, and
                        -- if the level is now empty, unlink the level
                        -- too. A level with no orders behind it must
                        -- not be reported as the best price.
                        if orders(work_order - 1).q_prev = 0 then
                            levels(work_level - 1).head := orders(work_order - 1).q_next;
                        else
                            orders(orders(work_order - 1).q_prev - 1).q_next := orders(work_order - 1).q_next;
                        end if;
                        if orders(work_order - 1).q_next = 0 then
                            levels(work_level - 1).tail := orders(work_order - 1).q_prev;
                        else
                            orders(orders(work_order - 1).q_next - 1).q_prev := orders(work_order - 1).q_prev;
                        end if;

                        levels(work_level - 1).aggregate :=
                            std_logic_vector(unsigned(levels(work_level - 1).aggregate) -
                                            unsigned(orders(work_order - 1).size));
                        levels(work_level - 1).order_count :=
                            levels(work_level - 1).order_count - 1;

                        if levels(work_level - 1).order_count = 0 then
                            -- The level is empty: unlink it from whichever
                            -- structure owns it.
                            level_slot := ladder_slot(levels(work_level - 1).price);
                            level_is_dense := false;
                            if level_slot /= 0 then
                                if (levels(work_level - 1).side = '1' and
                                    bid_slots(level_slot - 1) = work_level) or
                                   (levels(work_level - 1).side = '0' and
                                    ask_slots(level_slot - 1) = work_level) then
                                    level_is_dense := true;
                                end if;
                            end if;
                            if level_is_dense then
                                if levels(work_level - 1).side = '1' then
                                    bid_slots(level_slot - 1) := 0;
                                    bid_bits((level_slot - 1) / 64)((level_slot - 1) mod 64) := '0';
                                    if bid_hint = ((level_slot - 1) / 64) + 1 and
                                       is_zero(bid_bits((level_slot - 1) / 64)) then
                                        bid_hint := 0;
                                        for w in ((level_slot - 1) / 64) - 1 downto 0 loop
                                            if not is_zero(bid_bits(w)) then
                                                bid_hint := w + 1;
                                                exit;
                                            end if;
                                        end loop;
                                    end if;
                                else
                                    ask_slots(level_slot - 1) := 0;
                                    ask_bits((level_slot - 1) / 64)((level_slot - 1) mod 64) := '0';
                                    if ask_hint = ((level_slot - 1) / 64) + 1 and
                                       is_zero(ask_bits((level_slot - 1) / 64)) then
                                        ask_hint := 0;
                                        for w in ((level_slot - 1) / 64) + 1 to DENSE_WORDS - 1 loop
                                            if not is_zero(ask_bits(w)) then
                                                ask_hint := w + 1;
                                                exit;
                                            end if;
                                        end loop;
                                    end if;
                                end if;
                            else
                                -- A sparse level: unlink it from the list.
                                if levels(work_level - 1).l_prev = 0 then
                                    if levels(work_level - 1).side = '1' then
                                        bid_head := levels(work_level - 1).l_next;
                                    else
                                        ask_head := levels(work_level - 1).l_next;
                                    end if;
                                else
                                    levels(levels(work_level - 1).l_prev - 1).l_next := levels(work_level - 1).l_next;
                                end if;
                                if levels(work_level - 1).l_next /= 0 then
                                    levels(levels(work_level - 1).l_next - 1).l_prev := levels(work_level - 1).l_prev;
                                end if;
                            end if;
                            levels(work_level - 1).live := '0';
                            levels(work_level - 1).l_next := level_free;
                            level_free := work_level;
                        end if;

                        -- The index entry stays, with its key, and is
                        -- marked not live. ITCH references are unique
                        -- for the day, so this reference will never be
                        -- looked up successfully again, and a table
                        -- that never erases cannot wedge.
                        id_tab(probe_slot).live := '0';

                        -- And the order goes back on the free list.
                        orders(work_order - 1).live := '0';
                        orders(work_order - 1).q_next := order_free;
                        order_free := work_order;

                        if op = op_replace then
                            -- A replace removes the original outright
                            -- and then adds the replacement, which sorts
                            -- behind everything already resting at the
                            -- new price because it has a NEW reference
                            -- and therefore NEW time priority.
                            op := op_add;
                            msg_id := msg_new_id;
                            probe_slot := hash_slot(msg_new_id);
                            probe_steps := 0;
                            probe_done := '0';
                            state := st_probe;
                        else
                            o_status  <= bs_ok;
                            state := st_finish;
                        end if;

                    when st_finish =>
                        o_done <= '1';
                        o_cycles <= cycle_count;
                        state := st_idle;

                    when st_dump_level =>
                        if dump_cur_level = 0 then
                            if dump_dense_slot = 0 and dump_sparse_level = 0 then
                                -- The side is finished.
                                if dump_walk_side = '0' then
                                    dump_walk_side := '1';
                                    dump_sparse_level := ask_head;
                                    dump_dense_slot := best_dense_slot(ask_bits, '0', ask_hint);
                                    state := st_dump_level;
                                else
                                    o_dump_done  <= '1';
                                    state := st_idle;
                                end if;
                            else
                                -- Pick the better of the next dense level
                                -- and the next sparse level.
                                if dump_dense_slot = 0 then
                                    dump_cur_level := dump_sparse_level;
                                    dump_cur_dense := '0';
                                elsif dump_sparse_level = 0 then
                                    if dump_walk_side = '0' then
                                        dump_cur_level := bid_slots(dump_dense_slot - 1);
                                    else
                                        dump_cur_level := ask_slots(dump_dense_slot - 1);
                                    end if;
                                    dump_cur_dense := '1';
                                else
                                    if dump_walk_side = '0' then
                                        picked_level := bid_slots(dump_dense_slot - 1);
                                        if is_better('1',
                                                     levels(picked_level - 1).price,
                                                     levels(dump_sparse_level - 1).price) then
                                            dump_cur_level := picked_level;
                                            dump_cur_dense := '1';
                                        else
                                            dump_cur_level := dump_sparse_level;
                                            dump_cur_dense := '0';
                                        end if;
                                    else
                                        picked_level := ask_slots(dump_dense_slot - 1);
                                        if is_better('0',
                                                     levels(picked_level - 1).price,
                                                     levels(dump_sparse_level - 1).price) then
                                            dump_cur_level := picked_level;
                                            dump_cur_dense := '1';
                                        else
                                            dump_cur_level := dump_sparse_level;
                                            dump_cur_dense := '0';
                                        end if;
                                    end if;
                                end if;

                                o_dump_valid  <= '1';
                                o_dump_kind  <= x"4C";
                                o_dump_side  <= side_char(levels(dump_cur_level - 1).side);
                                o_dump_a  <= levels(dump_cur_level - 1).price;
                                o_dump_b  <= levels(dump_cur_level - 1).aggregate;
                                o_dump_c  <= std_logic_vector(
                                    to_unsigned(levels(dump_cur_level - 1).order_count, 64));
                                dump_cur_order := levels(dump_cur_level - 1).head;
                                dump_position := 0;
                                state := st_dump_order;
                            end if;
                        end if;

                    when st_dump_order =>
                        if dump_cur_order = 0 then
                            if dump_cur_dense = '1' then
                                if levels(dump_cur_level - 1).side = '1' then
                                    dump_dense_slot := next_dense_slot(bid_bits, dump_dense_slot, '1');
                                else
                                    dump_dense_slot := next_dense_slot(ask_bits, dump_dense_slot, '0');
                                end if;
                            else
                                dump_sparse_level := levels(dump_cur_level - 1).l_next;
                            end if;
                            dump_cur_level := 0;
                            state := st_dump_level;
                        else
                            o_dump_valid  <= '1';
                            o_dump_kind  <= x"4F";
                            o_dump_side  <= side_char(orders(dump_cur_order - 1).side);
                            o_dump_a  <= orders(dump_cur_order - 1).id;
                            o_dump_b  <= orders(dump_cur_order - 1).price;
                            o_dump_c  <= orders(dump_cur_order - 1).size;
                            o_dump_state  <= orders(dump_cur_order - 1).state;
                            o_dump_pos  <= dump_position;
                            dump_position := dump_position + 1;
                            dump_cur_order := orders(dump_cur_order - 1).q_next;
                            state := st_dump_order;
                        end if;

                end case;
            end if;
        end if;
    end process;

end architecture rtl;











