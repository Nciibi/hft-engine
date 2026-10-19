-- The differential testbench: a capture, in hardware, to a trace.
--
-- This is the RTL side of the differential between the engine's decoder
-- and the RTL model. It reads the same capture the C++ side reads --
-- through a hex sidecar, because reading a binary file in VHDL is a
-- known friction point and a hex dump is the cheapest thing that works
-- -- runs it through the framer and the decoder, and writes the result
-- as a trace in exactly the format `rtl/TRACE_FORMAT.md` specifies.
--
-- The two traces are then compared by `hft_rtl_check`, and the
-- comparison is positional: field 0 against field 0, and so on. A
-- disagreement names the record and the field.
--
-- The point of the exercise is that neither side shares anything with
-- the other. The field tables are transcribed twice, from the same
-- published specification. The framing is written twice. The only
-- thing they share is the capture and the format.
--
-- Usage:
--   ghdl -r tb_trace -gHEX_PATH=<file> -gOUT_PATH=<file>

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

library work;
use work.hft_itch_pkg.all;
use work.hft_trace_fmt.all;

use std.textio.all;

entity tb_trace is
    generic (
        HEX_PATH : string := "capture.hex";
        OUT_PATH : string := "trace.txt"
    );
end entity tb_trace;

architecture sim of tb_trace is

    constant kClockPeriod : time := 10 ns;

    signal clk : std_logic := '0';
    signal rst : std_logic := '1';
    signal sim_done : std_logic := '0';

    -- Byte stream into the framer.
    signal in_valid : std_logic := '0';
    signal in_eof : std_logic := '0';
    signal in_byte : std_logic_vector(7 downto 0) := (others => '0');

    -- Framer to decoder.
    signal out_valid : std_logic := '0';
    signal out_byte : std_logic_vector(7 downto 0) := (others => '0');
    signal frame_start : std_logic := '0';
    signal frame_len : natural range 0 to 65535 := 0;
    signal frame_has_len : std_logic := '0';
    signal frame_trunc : std_logic := '0';
    signal end_of_capture : std_logic := '0';

    -- Decoder result.
    signal done : std_logic := '0';
    signal status : decode_status_t := st_truncated;
    signal type_byte : std_logic_vector(7 downto 0) := (others => '0');
    signal has_type : std_logic := '0';
    signal declared_len : natural range 0 to 65535 := 0;
    signal field_count : natural range 0 to 9 := 0;
    signal fields : field_array_t := (others => (others => '0'));
    signal cycles : natural range 0 to 65535 := 0;

    -- The width of one field of the message that was decoded. The
    -- trace renders every field at its width ON THE WIRE, and that
    -- width comes from the field table rather than from the decoder.
    function field_width(t : std_logic_vector(7 downto 0);
                          type_known : boolean;
                          idx : natural) return natural is
        variable ti : type_index_t;
    begin
        if not type_known then
            return 0;
        end if;
        ti := type_index_of(t);
        if ti = kUnknownType then
            return 0;
        end if;
        if idx >= kFieldCount(ti) then
            return 0;
        end if;
        return kFieldWidth(ti, idx);
    end function;

begin

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
            done => done,
            status => status,
            type_byte => type_byte,
            has_type => has_type,
            declared_len => declared_len,
            field_count => field_count,
            fields => fields,
            cycles => cycles);

    -- ---- The clock ----------------------------------------------------

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

    -- ---- The reset ----------------------------------------------------

    reset_gen : process
    begin
        rst <= '1';
        wait for 4 * kClockPeriod;
        rst <= '0';
        wait;
    end process;

    -- ---- The byte source ----------------------------------------------
    --
    -- The hex sidecar: lowercase hex, two characters per byte, 64 bytes
    -- to a line. It is a faithful dump of the capture, so both sides
    -- are provably looking at the same bytes.

    driver : process
        file hex_file : text open read_mode is HEX_PATH;
        variable l : line;
        variable line_len : natural := 0;
        variable line_pos : natural := 0;
        variable eof_reached : boolean := false;
        variable b : std_logic_vector(7 downto 0);
        variable at_eof : boolean;

        procedure next_byte(variable out_b : out std_logic_vector(7 downto 0);
                            variable out_eof : out boolean) is
            variable hi : natural;
            variable lo : natural;
        begin
            out_eof := eof_reached;
            out_b := (others => '0');
            if eof_reached then
                return;
            end if;

            -- Refill the line if it is spent. An empty line is skipped:
            -- a trailing newline at the end of the file would otherwise
            -- look like a zero-length line rather than the end.
            while line_pos >= line_len loop
                if endfile(hex_file) then
                    eof_reached := true;
                    out_eof := true;
                    return;
                end if;
                readline(hex_file, l);
                line_len := l.all'length;
                line_pos := 0;
            end loop;

            hi := hex_val(l.all(line_pos + 1));
            lo := hex_val(l.all(line_pos + 2));
            line_pos := line_pos + 2;
            out_b := std_logic_vector(to_unsigned(hi * 16 + lo, 8));
        end procedure;

    begin
        in_valid <= '0';
        in_eof <= '0';
        in_byte <= (others => '0');
        wait until rst = '0';
        wait until falling_edge(clk);

        loop
            next_byte(b, at_eof);
            if at_eof then
                -- The input is finished. The framer needs to see one
                -- more cycle with no valid byte before it can tell
                -- that the capture ended.
                in_valid <= '0';
                in_eof <= '1';
                in_byte <= (others => '0');
                wait until falling_edge(clk);
                wait until falling_edge(clk);
                wait;
            end if;
            in_valid <= '1';
            in_byte <= b;
            wait until falling_edge(clk);
        end loop;
    end process;

    -- ---- The trace writer ---------------------------------------------

    monitor : process
        file trace_file : text open write_mode is OUT_PATH;
        variable l : line;
        variable index : natural := 0;
        variable n_ok : natural := 0;
        variable n_skipped : natural := 0;
        variable n_bad_length : natural := 0;
        variable n_malformed : natural := 0;
        variable n_truncated : natural := 0;
        variable total_cycles : natural := 0;
        variable stop_pending : boolean := false;

        -- Write one record, in the format `rtl/TRACE_FORMAT.md`
        -- specifies.
        procedure write_record is
        begin
            write(l, dec(index));
            write(l, ' ');
            write(l, status_char(status));
            write(l, ' ');
            write(l, type_token(type_byte, has_type = '1'));
            write(l, ' ');
            write(l, dec4(declared_len));
            for i in 0 to field_count - 1 loop
                write(l, ' ');
                write(l, hex_field(fields(i), field_width(type_byte, has_type = '1', i)));
            end loop;
            writeline(trace_file, l);

            case status is
                when st_ok => n_ok := n_ok + 1;
                when st_skipped => n_skipped := n_skipped + 1;
                when st_bad_length => n_bad_length := n_bad_length + 1;
                when st_malformed => n_malformed := n_malformed + 1;
                when st_truncated => n_truncated := n_truncated + 1;
            end case;
            total_cycles := total_cycles + cycles;
            index := index + 1;
        end procedure;
    begin
        write(l, kTraceHeader);
        writeline(trace_file, l);

        while sim_done = '0' loop
            wait until falling_edge(clk);

            if done = '1' then
                write_record;
            end if;

            -- The framer says the capture has ended. The decoder may
            -- still owe a record for the frame that was cut in half, so
            -- the stop is deferred by one cycle: the framer raises its
            -- truncation flag and its end-of-capture flag together, and
            -- the decoder answers the flag on the next edge.
            if end_of_capture = '1' and not stop_pending then
                stop_pending := true;
            elsif stop_pending then
                sim_done <= '1';
                report "rtl trace records " & integer'image(index);
                report "rtl decoded      " & integer'image(n_ok);
                report "rtl skipped      " & integer'image(n_skipped);
                report "rtl bad length   & integer'image(n_bad_length);
                report "rtl malformed    " & integer'image(n_malformed);
                report "rtl truncated    " & integer'image(n_truncated);
                report "rtl decode cycles" & integer'image(total_cycles);
            end if;
        end loop;

        wait;
    end process;

end architecture sim;




