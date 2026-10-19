-- The order-book differential testbench.
--
-- Reads a capture through the RTL decode path, applies every decoded
-- message to the RTL order book, and writes the resulting book as a
-- dump in exactly the format the C++ side writes. The two dumps are
-- then compared record by record and field by field.
--
-- ---- Why it decodes first and applies second ------------------------
--
-- The book is much slower than the decoder: finding a price is a walk
-- down the price-ordered level list, and the walk is one clock cycle per
-- level. On a book a few hundred levels deep that is hundreds of cycles
-- per message, against a decoder that produces a message every forty.
--
-- So the two are run in sequence rather than in lockstep: the decode
-- pass fills an array of messages, and the apply pass then walks that
-- array through the book one message at a time. The book is therefore
-- measured on its own, with no decoder in the loop and no queue
-- between them, which is also how a hardware design would be
-- characterised: one block, fed a known stimulus.
--
-- The alternative -- backpressure all the way from the book to the
-- capture reader -- would mean the decoder stalling mid-message and
-- the framer stalling mid-body, and a skid buffer between them. That
-- is a real piece of design and it is the right one for a system, but
-- it is a different piece of design from the one being measured here.
--
-- Usage:
--   ghdl -r tb_book -gHEX_PATH=<file> -gOUT_PATH=<file>

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

library work;
use work.hft_itch_pkg.all;
use work.hft_book_pkg.all;
use work.hft_trace_fmt.all;

use std.textio.all;
use std.textio;

entity tb_book is
    generic (
        HEX_PATH : string := "capture.hex";
        OUT_PATH : string := "book.dump";
        MAX_MSGS : positive := 8192
    );
end entity tb_book;

architecture sim of tb_book is

    constant kClockPeriod : time := 10 ns;

    -- A decoded message, held for the apply pass.
    type msg_t is record
        msg_type : std_logic_vector(7 downto 0);
        status   : decode_status_t;
        len      : natural;
        count    : natural;
        fields   : field_array_t;
    end record;

    type msg_array_t is array (0 to MAX_MSGS - 1) of msg_t;

    signal clk : std_logic := '0';
    signal rst : std_logic := '1';
    signal sim_done : std_logic := '0';

    -- The capture reader.
    signal in_valid : std_logic := '0';
    signal in_byte : std_logic_vector(7 downto 0) := (others => '0');
    signal in_eof : std_logic := '0';

    -- The framer to the decoder.
    signal out_valid : std_logic := '0';
    signal out_byte : std_logic_vector(7 downto 0) := (others => '0');
    signal frame_start : std_logic := '0';
    signal frame_len : natural range 0 to 65535 := 0;
    signal frame_has_len : std_logic := '0';
    signal frame_trunc : std_logic := '0';
    signal end_of_capture : std_logic := '0';

    -- The decoder's result.
    signal dec_done : std_logic := '0';
    signal dec_status : decode_status_t := st_truncated;
    signal dec_type : std_logic_vector(7 downto 0) := (others => '0');
    signal dec_has_type : std_logic := '0';
    signal dec_len : natural range 0 to 65535 := 0;
    signal dec_count : natural range 0 to 9 := 0;
    signal dec_fields : field_array_t := (others => (others => '0'));
    signal dec_cycles : natural range 0 to 65535 := 0;

    -- The book's result.
    signal apply_start : std_logic := '0';
    signal dump_start : std_logic := '0';
    signal apply_type : std_logic_vector(7 downto 0) := (others => '0');
    signal apply_fields : field_array_t := (others => (others => '0'));
    signal apply_done : std_logic := '0';
    signal apply_status : book_status_t := bs_ok;
    signal dump_valid : std_logic := '0';
    signal dump_kind : std_logic_vector(7 downto 0) := (others => '0');
    signal dump_side : std_logic_vector(7 downto 0) := (others => '0');
    signal dump_a : std_logic_vector(63 downto 0) := (others => '0');
    signal dump_b : std_logic_vector(63 downto 0) := (others => '0');
    signal dump_c : std_logic_vector(63 downto 0) := (others => '0');
    signal dump_state : order_state_t := st_resting;
    signal dump_pos : natural := 0;
    signal dump_done : std_logic := '0';

begin

    reader : entity work.hft_hex_reader
        generic map (
            PATH => HEX_PATH)
        port map (
            clk => clk,
            rst => rst,
            byte_valid => in_valid,
            byte_out => in_byte,
            byte_eof => in_eof);

    framer : entity work.hft_moldudp64
        port map (
            clk => clk,
            rst => rst,
            in_valid => in_valid,
            in_eof => in_eof,
            in_byte => in_byte,
            out_valid => out_valid,
            out_byte => out_byte,
            frame_start => frame_start,
            frame_len => frame_len,
            frame_has_len => frame_has_len,
            frame_trunc => frame_trunc,
            end_of_capture => end_of_capture);

    decoder : entity work.hft_itch_decode
        port map (
            clk => clk,
            rst => rst,
            frame_start => frame_start,
            frame_len => frame_len,
            frame_has_len => frame_has_len,
            frame_trunc => frame_trunc,
            byte_valid => out_valid,
            byte_in => out_byte,
            done => dec_done,
            status => dec_status,
            type_byte => dec_type,
            has_type => dec_has_type,
            declared_len => dec_len,
            field_count => dec_count,
            fields => dec_fields,
            cycles => dec_cycles);

    book : entity work.hft_book
        port map (
            clk => clk,
            rst => rst,
            apply_start => apply_start,
            apply_type => apply_type,
            apply_fields => apply_fields,
            apply_done => apply_done,
            apply_status => apply_status,
            dump_start => dump_start,
            dump_valid => dump_valid,
            dump_kind => dump_kind,
            dump_side => dump_side,
            dump_a => dump_a,
            dump_b => dump_b,
            dump_c => dump_c,
            dump_state => dump_state,
            dump_pos => dump_pos,
            dump_done => dump_done);

    -- ---- The clock and the reset --------------------------------------

    clock_gen : process
    begin
        while sim_done = '0' loop
            clk <= '0';
            wait for kClockPeriod / 2;
            clk <= '1';
            wait for kClockPeriod / 2;
        end loop;
        wait;
    end process;

    reset_gen : process
    begin
        rst <= '1';
        wait for 4 * kClockPeriod;
        rst <= '0';
        wait;
    end process;

    -- ---- The three phases ---------------------------------------------
    --
    -- 1. decode the capture into an array of messages
    -- 2. apply every message to the book
    -- 3. dump the book and write it out
    --
    -- They are in one process because the phases are sequential and
    -- the array is a process variable: a signal array would be copied
    -- whole on every read, which for a few thousand messages is a
    -- megabyte-scale copy per clock cycle.

    driver : process
        variable msgs : msg_array_t;
        variable msg_count : natural := 0;
        variable apply_index : natural := 0;
        variable checksum : std_logic_vector(63 downto 0) := fnv1a_basis;
        variable dump_index : natural := 0;
        variable apply_cycles : natural := 0;
        variable l : line;

        variable stop_pending : boolean := false;
        variable issued : boolean := false;
        file book_file : text open write_mode is OUT_PATH;
    begin
        -- Phase 1: decode.
        loop
            wait until falling_edge(clk);
            if dec_done = '1' then
                if msg_count < MAX_MSGS then
                    msgs(msg_count).msg_type := dec_type;
                    msgs(msg_count).status := dec_status;
                    msgs(msg_count).len := dec_len;
                    msgs(msg_count).count := dec_count;
                    msgs(msg_count).fields := dec_fields;
                    msg_count := msg_count + 1;
                end if;
            end if;
            if end_of_capture = '1' and not stop_pending then
                -- The framer has seen the end of the capture. The
                -- decoder may still owe a record for the frame that
                -- was cut in half, so the stop is deferred by one
                -- cycle.
                stop_pending := true;
            elsif stop_pending then
                exit;
            end if;
        end loop;

        report "rtl messages     " & integer'image(msg_count);

        -- Phase 2: apply.
        --
        -- One message at a time, and the wait is for the book to say
        -- it is done. The cycles the book spends are counted, because
        -- that is the number the C++ cannot produce: a cycle count is
        -- a property of the design, not of the machine.
        --
        -- pply_start is a ONE-CYCLE pulse. The book latches it and
        -- leaves its idle state, and if it were held high the book
        -- would apply the same message again when it came back round.
        while apply_index < msg_count loop
            wait until falling_edge(clk);
            if apply_done = '1' then
                apply_cycles := apply_cycles + 1;
                apply_index := apply_index + 1;
                issued := false;
                apply_start <= '0';
            elsif not issued then
                apply_type <= msgs(apply_index).msg_type;
                apply_fields <= msgs(apply_index).fields;
                apply_start <= '1';
                issued := true;
            else
                apply_start <= '0';
            end if;
        end loop;
        wait until falling_edge(clk);
        apply_start <= '0';

        report "rtl book cycles  " & integer'image(apply_cycles);

        -- Phase 3: dump.
        dump_start <= '1';
        wait until falling_edge(clk);
        dump_start <= '0';

        textio.write(l, "# hft-book-dump v1");
        writeline(book_file, l);

        loop
            wait until falling_edge(clk);
            if dump_valid = '1' then
                if dump_kind = x"4C" then
                    -- A level.
                    textio.write(l, integer'image(dump_index));
                    textio.write(l, ' ');
                    textio.write(l, 'L');
                    textio.write(l, ' ');
                    textio.write(l, character'val(to_integer(unsigned(dump_side))));
                    textio.write(l, ' ');
                    textio.write(l, hex_u64(dump_a));
                    textio.write(l, ' ');
                    textio.write(l, integer'image(to_integer(unsigned(dump_c))));
                    textio.write(l, ' ');
                    textio.write(l, hex_u64(dump_b));
                    writeline(book_file, l);
                else
                    -- An order.
                    textio.write(l, integer'image(dump_index));
                    textio.write(l, ' ');
                    textio.write(l, 'O');
                    textio.write(l, ' ');
                    textio.write(l, character'val(to_integer(unsigned(dump_side))));
                    textio.write(l, ' ');
                    textio.write(l, hex_u64(dump_a));
                    textio.write(l, ' ');
                    textio.write(l, hex_u64(dump_b));
                    textio.write(l, ' ');
                    textio.write(l, hex_u64(dump_c));
                    textio.write(l, ' ');
                    textio.write(l, integer'image(dump_state));
                    textio.write(l, ' ');
                    textio.write(l, integer'image(dump_pos));
                    writeline(book_file, l);

                    -- The checksum folds the orders, in this order:
                    -- the identifier, the price, the size and the
                    -- state, each as a 64-bit value.
                    checksum := fnv1a_u64(checksum, dump_a);
                    checksum := fnv1a_u64(checksum, dump_b);
                    checksum := fnv1a_u64(checksum, dump_c);
                    checksum := fnv1a_u64(checksum,
                                           std_logic_vector(to_unsigned(dump_state, 64)));
                end if;
                dump_index := dump_index + 1;
            end if;
            if dump_done = '1' then
                exit;
            end if;
        end loop;

        textio.write(l, "# checksum ");
        textio.write(l, hex_u64(checksum));
        writeline(book_file, l);
        file_close(book_file);

        report "rtl dump records " & integer'image(dump_index);
        report "rtl checksum     " & hex_u64(checksum);
        sim_done <= '1';
        wait;
    end process;

end architecture sim;






