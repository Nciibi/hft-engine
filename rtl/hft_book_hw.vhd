-- The order book: a synthesizable hardware core.
--
-- The behavioural model in `hft_book.vhd` keeps its pools and its price
-- ladder in process variables. That is the right shape for a simulator,
-- and the wrong shape for a FPGA: GHDL expands the variables into
-- registers, the dense bitmap scans have loop bounds the synthesiser
-- rejects, and no block RAM is inferred.
--
-- This entity is the hardware counterpart. It keeps the same external
-- interface and the same dump format, so the C++ differential can check
-- it without knowing which implementation produced the dump. The
-- storage lives in synchronous, BRAM-friendly arrays and a small FSM
-- sequences one access at a time.
--
-- The hardware ladder is the SPARSE one. A dense grid over the raw
-- 64-bit price space needs far more memory than an ECP5 has; the dense
-- grid remains the simulation model used for the cycle measurements.
-- This core handles arbitrary raw prices, which is what the sparse
-- fallback in the behavioural model does.
--
-- A second simplification is deliberate and visible: order and level
-- handles are allocated monotonically and are not recycled. Detaching
-- an order marks it dead and removes it from the aggregate, but the
-- order record is left in the queue so that the queue links do not
-- have to be repaired on every removal. The dump skips dead records.
-- The book therefore has a finite total-order budget rather than a
-- finite live-order budget. `MAX_ORDERS` and `MAX_LEVELS` are chosen
-- for the capture the differential runs; a production core would put
-- the free lists back in.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

library work;
use work.hft_itch_pkg.all;
use work.hft_book_pkg.all;

entity hft_book_hw is
    generic (
        MAX_ORDERS  : positive := 4096;
        MAX_LEVELS  : positive := 2048;
        TABLE_SLOTS : positive := 8192;
        -- Accepted for interface compatibility with the behavioural
        -- model. The hardware core is the sparse ladder.
        DENSE       : boolean := false;
        PRICE_FLOOR : natural := 0;
        PRICE_TICK  : positive := 1;
        DENSE_SLOTS : positive := 4096
    );
    port (
        clk : in std_logic;
        rst : in std_logic;

        apply_start  : in std_logic;
        apply_type   : in std_logic_vector(7 downto 0);
        apply_fields : in field_array_t;

        apply_done   : out std_logic;
        apply_status : out book_status_t;
        apply_cycles : out natural range 0 to natural'high;

        dump_start : in std_logic;
        dump_valid : out std_logic;
        dump_kind  : out std_logic_vector(7 downto 0);
        dump_side  : out std_logic_vector(7 downto 0);
        dump_a     : out std_logic_vector(63 downto 0);
        dump_b     : out std_logic_vector(63 downto 0);
        dump_c     : out std_logic_vector(63 downto 0);
        dump_state : out order_state_t;
        dump_pos   : out natural;
        dump_done  : out std_logic
    );
end entity hft_book_hw;

architecture rtl of hft_book_hw is

    type order_rec_t is record
        id    : std_logic_vector(63 downto 0);
        price : std_logic_vector(63 downto 0);
        size  : std_logic_vector(63 downto 0);
        state : order_state_t;
        side  : std_logic;
        level : natural range 0 to MAX_LEVELS;
        q_prev : natural range 0 to MAX_ORDERS;
        q_next : natural range 0 to MAX_ORDERS;
        live  : std_logic;
    end record;

    type level_rec_t is record
        price       : std_logic_vector(63 downto 0);
        head        : natural range 0 to MAX_ORDERS;
        tail        : natural range 0 to MAX_ORDERS;
        order_count : natural range 0 to MAX_ORDERS;
        aggregate   : std_logic_vector(63 downto 0);
        l_prev      : natural range 0 to MAX_LEVELS;
        l_next      : natural range 0 to MAX_LEVELS;
        side        : std_logic;
        live        : std_logic;
    end record;

    type id_entry_t is record
        key    : std_logic_vector(63 downto 0);
        handle : natural range 0 to MAX_ORDERS;
        live   : std_logic;
        used   : std_logic;
    end record;

    type order_pool_t is array (0 to MAX_ORDERS - 1) of order_rec_t;
    type level_pool_t is array (0 to MAX_LEVELS - 1) of level_rec_t;
    type id_table_t is array (0 to TABLE_SLOTS - 1) of id_entry_t;

    function zero_order return order_rec_t is
        variable r : order_rec_t;
    begin
        r.id := (others => '0');
        r.price := (others => '0');
        r.size := (others => '0');
        r.state := st_resting;
        r.side := '0';
        r.level := 0;
        r.q_prev := 0;
        r.q_next := 0;
        r.live := '0';
        return r;
    end function;

    function zero_level return level_rec_t is
        variable r : level_rec_t;
    begin
        r.price := (others => '0');
        r.head := 0;
        r.tail := 0;
        r.order_count := 0;
        r.aggregate := (others => '0');
        r.l_prev := 0;
        r.l_next := 0;
        r.side := '0';
        r.live := '0';
        return r;
    end function;

    function zero_id return id_entry_t is
        variable r : id_entry_t;
    begin
        r.key := (others => '0');
        r.handle := 0;
        r.live := '0';
        r.used := '0';
        return r;
    end function;

    function init_orders return order_pool_t is
        variable p : order_pool_t;
    begin
        for i in 0 to MAX_ORDERS - 1 loop
            p(i) := zero_order;
        end loop;
        return p;
    end function;

    function init_levels return level_pool_t is
        variable p : level_pool_t;
    begin
        for i in 0 to MAX_LEVELS - 1 loop
            p(i) := zero_level;
        end loop;
        return p;
    end function;

    function init_ids return id_table_t is
        variable p : id_table_t;
    begin
        for i in 0 to TABLE_SLOTS - 1 loop
            p(i) := zero_id;
        end loop;
        return p;
    end function;

    signal orders : order_pool_t := init_orders;
    signal levels : level_pool_t := init_levels;
    signal id_tab : id_table_t := init_ids;

    signal ord_rdata : order_rec_t := zero_order;
    signal lvl_rdata : level_rec_t := zero_level;
    signal id_rdata  : id_entry_t := zero_id;

    -- One fabric register after the EBR output. The EBR clock-to-q
    -- alone is more than half a 100 MHz period on this part, so the
    -- state machine never consumes the array outputs directly.
    signal ord_pre : order_rec_t := zero_order;
    signal lvl_pre : level_rec_t := zero_level;
    signal id_pre  : id_entry_t := zero_id;

    -- A second fabric register keeps the first next to the BRAM and
    -- lets the state-control logic live next to the wide consumers.
    signal ord_q : order_rec_t := zero_order;
    signal lvl_q : level_rec_t := zero_level;
    signal id_q  : id_entry_t := zero_id;

    function is_better(side : std_logic;
                       candidate : std_logic_vector(63 downto 0);
                       reference : std_logic_vector(63 downto 0)) return boolean is
    begin
        if side = '1' then
            return unsigned(candidate) > unsigned(reference);
        end if;
        return unsigned(candidate) < unsigned(reference);
    end function;

    function sat_sub(a : std_logic_vector(63 downto 0);
                     b : std_logic_vector(63 downto 0)) return std_logic_vector is
    begin
        if unsigned(a) >= unsigned(b) then
            return std_logic_vector(unsigned(a) - unsigned(b));
        end if;
        return x"0000000000000000";
    end function;

    function side_is_bid(side_byte : std_logic_vector(63 downto 0)) return boolean is
    begin
        return side_byte(7 downto 0) = x"42";
    end function;

    function side_char(side : std_logic) return std_logic_vector is
    begin
        if side = '1' then
            return x"42";
        end if;
        return x"41";
    end function;

    function hash_slot(key : std_logic_vector(63 downto 0)) return natural is
        constant kHashMix : unsigned(31 downto 0) := to_unsigned(16#7FEB352D#, 32);
        variable mixed : unsigned(31 downto 0);
    begin
        mixed := resize(unsigned(key(31 downto 0)) * kHashMix, 32);
        return to_integer(resize(mixed, 31)) mod TABLE_SLOTS;
    end function;

    type op_t is (op_none, op_add, op_execute, op_cancel, op_remove, op_replace);

    type state_t is (
        st_idle,
        st_read_wait,
        st_read_data,
        st_read_pp,
        st_probe_use,
        st_probe_result,
        st_walk_use,
        st_new_level_alloc,
        st_new_level_link,
        st_link_prev_use,
        st_link_next_use,
        st_order_write,
        st_order_tail_use,
        st_level_write,
        st_lookup_order_use,
        st_partial_level_use,
        st_detach_level_use,
        st_detach_unlink_prev_use,
        st_detach_unlink_next_use,
        st_detach_finish,
        st_finish,
        st_dump_level_use,
        st_dump_order_use,
        st_dump_next_level
    );

    type detach_after_t is (da_finish, da_replace);

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
        variable state : state_t := st_idle;
        variable read_ret : state_t := st_idle;

        variable ord_raddr : natural range 0 to MAX_ORDERS - 1 := 0;
        variable lvl_raddr : natural range 0 to MAX_LEVELS - 1 := 0;
        variable id_raddr : natural range 0 to TABLE_SLOTS - 1 := 0;
        variable ord_waddr : natural range 0 to MAX_ORDERS - 1 := 0;
        variable lvl_waddr : natural range 0 to MAX_LEVELS - 1 := 0;
        variable id_waddr : natural range 0 to TABLE_SLOTS - 1 := 0;
        variable ord_we : std_logic := '0';
        variable lvl_we : std_logic := '0';
        variable id_we : std_logic := '0';
        variable ord_wdata : order_rec_t := zero_order;
        variable lvl_wdata : level_rec_t := zero_level;
        variable id_wdata : id_entry_t := zero_id;

        variable op : op_t := op_none;
        variable stat : book_status_t := bs_ok;
        variable msg_id : std_logic_vector(63 downto 0) := (others => '0');
        variable msg_new_id : std_logic_vector(63 downto 0) := (others => '0');
        variable msg_price : std_logic_vector(63 downto 0) := (others => '0');
        variable msg_size : std_logic_vector(63 downto 0) := (others => '0');
        variable msg_side : std_logic := '0';

        variable probe_slot : natural range 0 to TABLE_SLOTS - 1 := 0;
        variable probe_steps : natural range 0 to TABLE_SLOTS := 0;
        variable probe_hit : std_logic := '0';

        variable walk_cur : natural range 0 to MAX_LEVELS := 0;
        variable walk_prev : natural range 0 to MAX_LEVELS := 0;
        variable work_order : natural range 0 to MAX_ORDERS := 0;
        variable work_level : natural range 0 to MAX_LEVELS := 0;
        variable work_lvl : level_rec_t := zero_level;
        variable work_ord : order_rec_t := zero_order;

        variable next_order : natural range 1 to MAX_ORDERS + 1 := 1;
        variable next_level : natural range 1 to MAX_LEVELS + 1 := 1;
        variable bid_head : natural range 0 to MAX_LEVELS := 0;
        variable ask_head : natural range 0 to MAX_LEVELS := 0;

        variable cycle_count : natural range 0 to natural'high := 0;

        variable detach_order : natural range 0 to MAX_ORDERS := 0;
        variable detach_level : natural range 0 to MAX_LEVELS := 0;
        variable detach_size : std_logic_vector(63 downto 0) := (others => '0');
        variable detach_side : std_logic := '0';
        variable detach_new_state : order_state_t := st_cancelled;
        variable detach_after : detach_after_t := da_finish;
        variable detach_lvl : level_rec_t := zero_level;

        variable dump_cur_level : natural range 0 to MAX_LEVELS := 0;
        variable dump_cur_order : natural range 0 to MAX_ORDERS := 0;
        variable dump_next_level : natural range 0 to MAX_LEVELS := 0;
        variable dump_walk_side : std_logic := '0';
        variable dump_position : natural := 0;
    begin
        if rst = '1' then
            state := st_idle;
            next_order := 1;
            next_level := 1;
            bid_head := 0;
            ask_head := 0;
            cycle_count := 0;
            o_done <= '0';
            o_status <= bs_ok;
            o_dump_valid <= '0';
            o_dump_done <= '0';
            ord_we := '0';
            lvl_we := '0';
            id_we := '0';
        elsif rising_edge(clk) then
            -- Synchronous reads and writes. The address variables are
            -- updated after these statements, so a read sees the address
            -- presented in the previous cycle.
            ord_rdata <= orders(ord_raddr);
            lvl_rdata <= levels(lvl_raddr);
            id_rdata <= id_tab(id_raddr);

            if ord_we = '1' then
                orders(ord_waddr) <= ord_wdata;
            end if;
            if lvl_we = '1' then
                levels(lvl_waddr) <= lvl_wdata;
            end if;
            if id_we = '1' then
                id_tab(id_waddr) <= id_wdata;
            end if;

            ord_we := '0';
            lvl_we := '0';
            id_we := '0';
            o_done <= '0';
            o_dump_valid <= '0';
            o_dump_done <= '0';

            if state /= st_idle then
                cycle_count := cycle_count + 1;
            end if;

            case state is

                when st_idle =>
                    if dump_start = '1' then
                        dump_next_level := 0;
                        if bid_head /= 0 then
                            dump_walk_side := '1';
                            dump_cur_level := bid_head;
                        elsif ask_head /= 0 then
                            dump_walk_side := '0';
                            dump_cur_level := ask_head;
                        else
                            o_dump_done <= '1';
                            dump_cur_level := 0;
                        end if;
                        if dump_cur_level /= 0 then
                            dump_position := 0;
                            lvl_raddr := dump_cur_level - 1;
                            read_ret := st_dump_level_use;
                            state := st_read_wait;
                        end if;
                    elsif apply_start = '1' then
                        cycle_count := 0;
                        case apply_type is
                            when x"41" =>
                                op := op_add;
                                msg_id := apply_fields(3);
                                if side_is_bid(apply_fields(4)) then
                                    msg_side := '1';
                                else
                                    msg_side := '0';
                                end if;
                                msg_size := apply_fields(5);
                                msg_price := apply_fields(7);
                            when x"45" | x"43" =>
                                op := op_execute;
                                msg_id := apply_fields(3);
                                msg_size := apply_fields(4);
                            when x"58" =>
                                op := op_cancel;
                                msg_id := apply_fields(3);
                                msg_size := apply_fields(4);
                            when x"44" =>
                                op := op_remove;
                                msg_id := apply_fields(3);
                            when x"55" =>
                                op := op_replace;
                                msg_id := apply_fields(3);
                                msg_new_id := apply_fields(4);
                                msg_size := apply_fields(5);
                                msg_price := apply_fields(6);
                            when others =>
                                op := op_none;
                        end case;
                        if op = op_none then
                            stat := bs_ok;
                            state := st_finish;
                        else
                            probe_slot := hash_slot(msg_id);
                            probe_steps := 0;
                            probe_hit := '0';
                            id_raddr := probe_slot;
                            read_ret := st_probe_use;
                            state := st_read_wait;
                        end if;
                    end if;

                when st_read_wait =>
                    state := st_read_data;

                when st_read_data =>
                    id_pre <= id_rdata;
                    ord_pre <= ord_rdata;
                    lvl_pre <= lvl_rdata;
                    state := st_read_pp;

                when st_read_pp =>
                    id_q <= id_pre;
                    ord_q <= ord_pre;
                    lvl_q <= lvl_pre;
                    state := read_ret;

                when st_probe_use =>
                    if id_q.used = '0' or id_q.key = msg_id then
                        if id_q.used = '1' and id_q.key = msg_id then
                            probe_hit := id_q.live;
                        else
                            probe_hit := '0';
                        end if;
                        state := st_probe_result;
                    else
                        probe_steps := probe_steps + 1;
                        if probe_steps >= TABLE_SLOTS then
                            stat := bs_capacity_exhausted;
                            state := st_finish;
                        else
                            probe_slot := (probe_slot + 1) mod TABLE_SLOTS;
                            id_raddr := probe_slot;
                            read_ret := st_probe_use;
                            state := st_read_wait;
                        end if;
                    end if;

                when st_probe_result =>
                    if op = op_add then
                        if probe_hit = '1' or msg_id = x"0000000000000000" then
                            stat := bs_duplicate_order;
                            state := st_finish;
                        elsif msg_size = x"0000000000000000" then
                            stat := bs_zero_size;
                            state := st_finish;
                        elsif next_order > MAX_ORDERS then
                            stat := bs_capacity_exhausted;
                            state := st_finish;
                        else
                            walk_prev := 0;
                            if msg_side = '1' then
                                walk_cur := bid_head;
                            else
                                walk_cur := ask_head;
                            end if;
                            if walk_cur = 0 then
                                state := st_new_level_alloc;
                            else
                                lvl_raddr := walk_cur - 1;
                                read_ret := st_walk_use;
                                state := st_read_wait;
                            end if;
                        end if;
                    else
                        if probe_hit = '0' then
                            stat := bs_unknown_order;
                            state := st_finish;
                        else
                            work_order := id_q.handle;
                            ord_raddr := work_order - 1;
                            read_ret := st_lookup_order_use;
                            state := st_read_wait;
                        end if;
                    end if;

                when st_walk_use =>
                    if is_better(msg_side, lvl_q.price, msg_price) then
                        walk_prev := walk_cur;
                        walk_cur := lvl_q.l_next;
                        if walk_cur = 0 then
                            state := st_new_level_alloc;
                        else
                            lvl_raddr := walk_cur - 1;
                            read_ret := st_walk_use;
                            state := st_read_wait;
                        end if;
                    elsif lvl_q.price = msg_price then
                        work_level := walk_cur;
                        work_lvl := lvl_q;
                        state := st_order_write;
                    else
                        state := st_new_level_alloc;
                    end if;

                when st_new_level_alloc =>
                    if next_level > MAX_LEVELS then
                        stat := bs_capacity_exhausted;
                        state := st_finish;
                    else
                        work_level := next_level;
                        next_level := next_level + 1;
                        work_lvl := zero_level;
                        work_lvl.price := msg_price;
                        work_lvl.side := msg_side;
                        work_lvl.l_prev := walk_prev;
                        work_lvl.l_next := walk_cur;
                        work_lvl.live := '1';
                        lvl_waddr := work_level - 1;
                        lvl_wdata := work_lvl;
                        lvl_we := '1';
                        state := st_new_level_link;
                    end if;

                when st_new_level_link =>
                    if walk_prev = 0 then
                        if work_lvl.side = '1' then
                            bid_head := work_level;
                        else
                            ask_head := work_level;
                        end if;
                        if walk_cur /= 0 then
                            lvl_raddr := walk_cur - 1;
                            read_ret := st_link_next_use;
                            state := st_read_wait;
                        else
                            state := st_order_write;
                        end if;
                    else
                        lvl_raddr := walk_prev - 1;
                        read_ret := st_link_prev_use;
                        state := st_read_wait;
                    end if;

                when st_link_prev_use =>
                    lvl_wdata := lvl_q;
                    lvl_wdata.l_next := work_level;
                    lvl_waddr := walk_prev - 1;
                    lvl_we := '1';
                    if walk_cur /= 0 then
                        lvl_raddr := walk_cur - 1;
                        read_ret := st_link_next_use;
                        state := st_read_wait;
                    else
                        state := st_order_write;
                    end if;

                when st_link_next_use =>
                    lvl_wdata := lvl_q;
                    lvl_wdata.l_prev := work_level;
                    lvl_waddr := walk_cur - 1;
                    lvl_we := '1';
                    state := st_order_write;

                when st_order_write =>
                    work_order := next_order;
                    next_order := next_order + 1;
                    work_ord := zero_order;
                    work_ord.id := msg_id;
                    work_ord.price := msg_price;
                    work_ord.size := msg_size;
                    work_ord.state := st_resting;
                    work_ord.side := msg_side;
                    work_ord.level := work_level;
                    work_ord.q_prev := work_lvl.tail;
                    work_ord.q_next := 0;
                    work_ord.live := '1';
                    ord_waddr := work_order - 1;
                    ord_wdata := work_ord;
                    ord_we := '1';
                    if work_lvl.tail /= 0 then
                        ord_raddr := work_lvl.tail - 1;
                        read_ret := st_order_tail_use;
                        state := st_read_wait;
                    else
                        work_lvl.head := work_order;
                        work_lvl.tail := work_order;
                        work_lvl.order_count := 1;
                        work_lvl.aggregate := msg_size;
                        state := st_level_write;
                    end if;

                when st_order_tail_use =>
                    ord_wdata := ord_q;
                    ord_wdata.q_next := work_order;
                    ord_waddr := work_lvl.tail - 1;
                    ord_we := '1';
                    work_lvl.tail := work_order;
                    if work_lvl.head = 0 then
                        work_lvl.head := work_order;
                    end if;
                    work_lvl.order_count := work_lvl.order_count + 1;
                    work_lvl.aggregate := std_logic_vector(
                        unsigned(work_lvl.aggregate) + unsigned(msg_size));
                    state := st_level_write;

                when st_level_write =>
                    lvl_waddr := work_level - 1;
                    lvl_wdata := work_lvl;
                    lvl_we := '1';
                    id_waddr := probe_slot;
                    id_wdata.key := msg_id;
                    id_wdata.handle := work_order;
                    id_wdata.live := '1';
                    id_wdata.used := '1';
                    id_we := '1';
                    stat := bs_ok;
                    state := st_finish;

                when st_lookup_order_use =>
                    if ord_q.live = '0' then
                        stat := bs_unknown_order;
                        state := st_finish;
                    elsif op = op_remove then
                        detach_order := work_order;
                        detach_level := ord_q.level;
                        detach_size := ord_q.size;
                        detach_side := ord_q.side;
                        detach_new_state := st_cancelled;
                        detach_after := da_finish;
                        ord_wdata := ord_q;
                        ord_wdata.state := st_cancelled;
                        ord_wdata.live := '0';
                        ord_waddr := work_order - 1;
                        ord_we := '1';
                        id_wdata := id_q;
                        id_wdata.live := '0';
                        id_waddr := probe_slot;
                        id_we := '1';
                        lvl_raddr := ord_q.level - 1;
                        read_ret := st_detach_level_use;
                        state := st_read_wait;
                    elsif op = op_execute or op = op_cancel then
                        if unsigned(msg_size) > unsigned(ord_q.size) then
                            stat := bs_over_reduce;
                            state := st_finish;
                        elsif msg_size = ord_q.size then
                            detach_order := work_order;
                            detach_level := ord_q.level;
                            detach_size := ord_q.size;
                            detach_side := ord_q.side;
                            detach_after := da_finish;
                            if op = op_execute then
                                detach_new_state := st_executed;
                            else
                                detach_new_state := st_cancelled;
                            end if;
                            ord_wdata := ord_q;
                            ord_wdata.state := detach_new_state;
                            ord_wdata.live := '0';
                            ord_waddr := work_order - 1;
                            ord_we := '1';
                            id_wdata := id_q;
                            id_wdata.live := '0';
                            id_waddr := probe_slot;
                            id_we := '1';
                            lvl_raddr := ord_q.level - 1;
                            read_ret := st_detach_level_use;
                            state := st_read_wait;
                        else
                            ord_wdata := ord_q;
                            ord_wdata.size := std_logic_vector(
                                unsigned(ord_q.size) - unsigned(msg_size));
                            if op = op_execute then
                                ord_wdata.state := st_part_executed;
                            else
                                ord_wdata.state := st_resting;
                            end if;
                            ord_waddr := work_order - 1;
                            ord_we := '1';
                            lvl_raddr := ord_q.level - 1;
                            read_ret := st_partial_level_use;
                            state := st_read_wait;
                        end if;
                    else
                        detach_order := work_order;
                        detach_level := ord_q.level;
                        detach_size := ord_q.size;
                        detach_side := ord_q.side;
                        detach_new_state := st_superseded;
                        detach_after := da_replace;
                        ord_wdata := ord_q;
                        ord_wdata.state := st_superseded;
                        ord_wdata.live := '0';
                        ord_waddr := work_order - 1;
                        ord_we := '1';
                        id_wdata := id_q;
                        id_wdata.live := '0';
                        id_waddr := probe_slot;
                        id_we := '1';
                        lvl_raddr := ord_q.level - 1;
                        read_ret := st_detach_level_use;
                        state := st_read_wait;
                    end if;

                when st_partial_level_use =>
                    lvl_wdata := lvl_q;
                    lvl_wdata.aggregate := sat_sub(lvl_q.aggregate, msg_size);
                    lvl_waddr := ord_q.level - 1;
                    lvl_we := '1';
                    stat := bs_ok;
                    state := st_finish;

                when st_detach_level_use =>
                    lvl_wdata := lvl_q;
                    if lvl_q.order_count = 0 then
                        stat := bs_unknown_order;
                        state := st_finish;
                    else
                        lvl_wdata.order_count := lvl_q.order_count - 1;
                        lvl_wdata.aggregate := sat_sub(lvl_q.aggregate, detach_size);
                        lvl_waddr := detach_level - 1;
                        lvl_we := '1';
                        if lvl_wdata.order_count = 0 then
                            detach_lvl := lvl_wdata;
                            detach_lvl.live := '0';
                            lvl_wdata := detach_lvl;
                            if lvl_q.l_prev /= 0 then
                                lvl_raddr := lvl_q.l_prev - 1;
                                read_ret := st_detach_unlink_prev_use;
                                state := st_read_wait;
                            else
                                if lvl_q.side = '1' then
                                    bid_head := lvl_q.l_next;
                                else
                                    ask_head := lvl_q.l_next;
                                end if;
                                if lvl_q.l_next /= 0 then
                                    lvl_raddr := lvl_q.l_next - 1;
                                    read_ret := st_detach_unlink_next_use;
                                    state := st_read_wait;
                                else
                                    state := st_detach_finish;
                                end if;
                            end if;
                        else
                            state := st_detach_finish;
                        end if;
                    end if;

                when st_detach_unlink_prev_use =>
                    lvl_wdata := lvl_q;
                    lvl_wdata.l_next := detach_lvl.l_next;
                    lvl_waddr := detach_lvl.l_prev - 1;
                    lvl_we := '1';
                    if detach_lvl.l_next /= 0 then
                        lvl_raddr := detach_lvl.l_next - 1;
                        read_ret := st_detach_unlink_next_use;
                        state := st_read_wait;
                    else
                        state := st_detach_finish;
                    end if;

                when st_detach_unlink_next_use =>
                    lvl_wdata := lvl_q;
                    lvl_wdata.l_prev := detach_lvl.l_prev;
                    lvl_waddr := detach_lvl.l_next - 1;
                    lvl_we := '1';
                    state := st_detach_finish;

                when st_detach_finish =>
                    if detach_after = da_replace then
                        op := op_add;
                        msg_id := msg_new_id;
                        msg_side := detach_side;
                        probe_slot := hash_slot(msg_new_id);
                        probe_steps := 0;
                        probe_hit := '0';
                        id_raddr := probe_slot;
                        read_ret := st_probe_use;
                        state := st_read_wait;
                    else
                        stat := bs_ok;
                        state := st_finish;
                    end if;

                when st_finish =>
                    o_status <= stat;
                    o_cycles <= cycle_count;
                    o_done <= '1';
                    state := st_idle;

                when st_dump_level_use =>
                    o_dump_valid <= '1';
                    o_dump_kind <= x"4C";
                    o_dump_side <= side_char(lvl_q.side);
                    o_dump_a <= lvl_q.price;
                    o_dump_b <= lvl_q.aggregate;
                    o_dump_c <= std_logic_vector(
                        to_unsigned(lvl_q.order_count, 64));
                    o_dump_pos <= dump_position;
                    dump_cur_order := lvl_q.head;
                    dump_next_level := lvl_q.l_next;
                    dump_position := 0;
                    if dump_cur_order /= 0 then
                        ord_raddr := dump_cur_order - 1;
                        read_ret := st_dump_order_use;
                        state := st_read_wait;
                    else
                        state := st_dump_next_level;
                    end if;

                when st_dump_order_use =>
                    if ord_q.live = '1' then
                        o_dump_valid <= '1';
                        o_dump_kind <= x"4F";
                        o_dump_side <= side_char(ord_q.side);
                        o_dump_a <= ord_q.id;
                        o_dump_b <= ord_q.price;
                        o_dump_c <= ord_q.size;
                        o_dump_state <= ord_q.state;
                        o_dump_pos <= dump_position;
                        dump_position := dump_position + 1;
                    end if;
                    dump_cur_order := ord_q.q_next;
                    if dump_cur_order /= 0 then
                        ord_raddr := dump_cur_order - 1;
                        read_ret := st_dump_order_use;
                        state := st_read_wait;
                    else
                        state := st_dump_next_level;
                    end if;

                when st_dump_next_level =>
                    if dump_next_level /= 0 then
                        dump_cur_level := dump_next_level;
                        dump_position := 0;
                        lvl_raddr := dump_cur_level - 1;
                        read_ret := st_dump_level_use;
                        state := st_read_wait;
                    elsif dump_walk_side = '1' then
                        dump_walk_side := '0';
                        dump_cur_level := ask_head;
                        dump_position := 0;
                        if dump_cur_level /= 0 then
                            lvl_raddr := dump_cur_level - 1;
                            read_ret := st_dump_level_use;
                            state := st_read_wait;
                        else
                            o_dump_done <= '1';
                            state := st_idle;
                        end if;
                    else
                        o_dump_done <= '1';
                        state := st_idle;
                    end if;

            end case;
        end if;
    end process;

end architecture rtl;
