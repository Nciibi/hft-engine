-- Streaming top-level testbench: hex sidecar -> hft_top (framer,
-- decoder, FIFO with backpressure, synthesizable book, sequence
-- tracker, in-hardware checksum) -> book dump.
--
-- Unlike tb_book (decode-then-apply array), this runs the whole
-- pipeline in lockstep: bytes are only consumed when in_ready='1',
-- so the FIFO/backpressure path is exercised for real. The dump
-- and checksum must match the C++ reference (checked by
-- cmake/rtl_top_differential.cmake via hft_book_check).

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
library work;
use work.hft_itch_pkg.all;
use work.hft_book_pkg.all;
use work.hft_trace_fmt.all;
use std.textio.all;
use std.textio;

entity tb_top is
    generic (
        HEX_PATH : string := "capture.hex";
        OUT_PATH : string := "book.dump"
    );
end entity tb_top;

architecture sim of tb_top is
    constant kClockPeriod : time := 10 ns;
    constant kDumpHeader : string := "# hft-book-dump v1";
    constant kChecksumTag : string := "# checksum ";
    signal clk : std_logic := '0';
    signal rst : std_logic := '1';
    signal sim_done : std_logic := '0';
    signal in_valid : std_logic := '0';
    signal in_ready : std_logic := '0';
    signal in_eof : std_logic := '0';
    signal in_byte : std_logic_vector(7 downto 0) := (others => '0');
    signal dump_start : std_logic := '0';
    signal dump_valid : std_logic := '0';
    signal dump_kind : std_logic_vector(7 downto 0) := (others => '0');
    signal dump_side : std_logic_vector(7 downto 0) := (others => '0');
    signal dump_a : std_logic_vector(63 downto 0) := (others => '0');
    signal dump_b : std_logic_vector(63 downto 0) := (others => '0');
    signal dump_c : std_logic_vector(63 downto 0) := (others => '0');
    signal dump_state : order_state_t := st_resting;
    signal dump_pos : natural := 0;
    signal dump_done : std_logic := '0';
    signal book_checksum : std_logic_vector(63 downto 0) := (others => '0');
    signal book_checksum_valid : std_logic := '0';
    signal end_cap : std_logic := '0';
    signal fifo_count : natural range 0 to 8 := 0;
    signal book_busy : std_logic := '0';
    signal seq_state_s : natural range 0 to 2 := 0;
    signal seq_clean_s : std_logic := '0';
    signal seq_expected_s : std_logic_vector(63 downto 0) := (others => '0');
    signal seq_accepted_s : std_logic_vector(63 downto 0) := (others => '0');
    signal seq_missing_s : std_logic_vector(63 downto 0) := (others => '0');
    signal seq_gaps_s : std_logic_vector(63 downto 0) := (others => '0');
    signal seq_rejects_s : std_logic_vector(63 downto 0) := (others => '0');
    signal last_symbol_s : std_logic_vector(63 downto 0) := (others => '0');
    signal last_symbol_valid_s : std_logic := '0';
begin
    reader : entity work.hft_hex_reader
        generic map (PATH => HEX_PATH)
        port map (
            clk => clk, rst => rst, byte_ready => in_ready,
            byte_valid => in_valid, byte_out => in_byte,
            byte_eof => in_eof);

    top : entity work.hft_top
        port map (
            clk => clk, rst => rst,
            in_valid => in_valid, in_ready => in_ready,
            in_eof => in_eof, in_byte => in_byte,
            dump_start => dump_start, dump_valid => dump_valid,
            dump_kind => dump_kind, dump_side => dump_side,
            dump_a => dump_a, dump_b => dump_b, dump_c => dump_c,
            dump_state => dump_state, dump_pos => dump_pos,
            dump_done => dump_done, book_checksum => book_checksum,
            book_checksum_valid => book_checksum_valid,
            end_of_capture => end_cap, fifo_count => fifo_count,
            book_busy => book_busy, seq_state => seq_state_s,
            seq_clean => seq_clean_s, seq_expected => seq_expected_s,
            seq_accepted => seq_accepted_s, seq_missing => seq_missing_s,
            seq_gaps => seq_gaps_s, seq_rejects => seq_rejects_s,
            last_symbol => last_symbol_s,
            last_symbol_valid => last_symbol_valid_s);

    clock_gen : process
    begin
        while sim_done = '0' loop
            clk <= '0'; wait for kClockPeriod / 2;
            clk <= '1'; wait for kClockPeriod / 2;
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

    driver : process
        variable l : line;
        variable dump_index : natural := 0;
        variable checksum : std_logic_vector(63 downto 0) := fnv1a_basis;
        variable got_checksum : boolean := false;
        variable dbg_count : natural := 0;
        file book_file : text open write_mode is OUT_PATH;
    begin
        -- Wait for capture end, then for FIFO + book to drain.
        report "driver start end_cap=" & std_logic'image(end_cap) & " fifo=" & integer'image(fifo_count);
        while end_cap = '0' loop
            wait until falling_edge(clk);
            dbg_count := dbg_count + 1;
            if (dbg_count mod 1000) = 0 then
                report "dbg fifo=" & integer'image(fifo_count) & " busy=" & std_logic'image(book_busy)
                     & " in_valid=" & std_logic'image(in_valid) & " in_ready=" & std_logic'image(in_ready)
                     & " in_eof=" & std_logic'image(in_eof);
            end if;
        end loop;
        report "driver saw end_cap";
        while fifo_count > 0 or book_busy = '1' loop
            wait until falling_edge(clk);
        end loop;
        wait until falling_edge(clk);
        wait until falling_edge(clk);

        report "rtl top seq accepted " & integer'image(to_integer(unsigned(seq_accepted_s)));
        report "rtl top seq missing  " & integer'image(to_integer(unsigned(seq_missing_s)));
        report "rtl top seq gaps     " & integer'image(to_integer(unsigned(seq_gaps_s)));
        report "rtl top seq rejects  " & integer'image(to_integer(unsigned(seq_rejects_s)));

        dump_start <= '1';
        wait until falling_edge(clk);
        dump_start <= '0';
        textio.write(l, kDumpHeader);
        writeline(book_file, l);
        loop
            wait until falling_edge(clk);
            if dump_valid = '1' then
                if dump_kind = x"4C" then
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
                    checksum := fnv1a_u64(checksum, dump_a);
                    checksum := fnv1a_u64(checksum, dump_b);
                    checksum := fnv1a_u64(checksum, dump_c);
                    checksum := fnv1a_u64(checksum,
                        std_logic_vector(to_unsigned(dump_state, 64)));
                end if;
                dump_index := dump_index + 1;
            end if;
            if book_checksum_valid = '1' then
                got_checksum := true;
                assert book_checksum = checksum
                    report "in-hardware checksum disagrees with testbench checksum"
                    severity failure;
                report "rtl top hw checksum ok";
            end if;
            if dump_done = '1' then
                exit;
            end if;
        end loop;
        assert got_checksum report "hardware checksum never valid" severity failure;
        textio.write(l, kChecksumTag);
        textio.write(l, hex_u64(checksum));
        writeline(book_file, l);
        file_close(book_file);
        report "rtl top dump records " & integer'image(dump_index);
        report "rtl top checksum     " & hex_u64(checksum);
        sim_done <= '1';
        wait;
    end process;
end architecture sim;
