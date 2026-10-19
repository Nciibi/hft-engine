-- The hex sidecar reader: a file of hex, as a byte stream.
--
-- The RTL side reads the capture through a hex dump rather than
-- through the binary file itself, because reading binary in VHDL is a
-- known friction point and a hex dump is the cheapest thing that
-- works. `textio` reads it with a two-character loop, and a human can
-- read it when a comparison fails.
--
-- The sidecar is a faithful dump -- every byte of the capture, in
-- order, nothing added -- so the RTL side is provably looking at the
-- same bytes the C++ side did.
--
-- ---- The contract ----------------------------------------------------
--
-- One byte per clock, with no gaps. A gap would be indistinguishable
-- from the end of the stream to a consumer that reads a byte when one
-- is valid, so the reader never produces one: when the current line is
-- spent it reads the next one in the same cycle, and an empty line is
-- skipped rather than reported.
--
-- `byte_eof` rises one cycle after the last byte, and `byte_valid` is
-- low from then on. A consumer that treats "no byte" as "the end"
-- needs the explicit `byte_eof` to tell an empty stream from a
-- finished one, which is the same distinction the framer's `in_eof`
-- makes.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

library work;
use work.hft_trace_fmt.all;

entity hft_hex_reader is
    generic (
        PATH : string := "capture.hex"
    );
    port (
        clk         : in  std_logic;
        rst         : in  std_logic;
        byte_valid  : out std_logic;
        byte_out    : out std_logic_vector(7 downto 0);
        byte_eof    : out std_logic
    );
end entity hft_hex_reader;

architecture rtl of hft_hex_reader is

    signal o_valid : std_logic := '0';
    signal o_byte : std_logic_vector(7 downto 0) := (others => '0');
    signal o_eof : std_logic := '0';

begin

    byte_valid <= o_valid;
    byte_out <= o_byte;
    byte_eof <= o_eof;

    process (clk)
        file hex_file : text open read_mode is PATH;
        variable l : line;
        variable line_len : natural := 0;
        variable line_pos : natural := 0;
        variable eof_reached : boolean := false;
        variable hi : natural;
        variable lo : natural;
    begin
        if rising_edge(clk) then
            if rst = '1' then
                o_valid <= '0';
                o_eof <= '0';
            else
                if not eof_reached then
                    -- Read the next non-empty line if this one is
                    -- spent. An empty line is skipped, so a trailing
                    -- newline cannot look like a zero-length line
                    -- rather than the end.
                    while (line_len = 0) and (not eof_reached) loop
                        if endfile(hex_file) then
                            eof_reached := true;
                        else
                            readline(hex_file, l);
                            line_len := l.all'length;
                            line_pos := 0;
                        end if;
                    end loop;
                end if;

                if eof_reached then
                    o_valid <= '0';
                    o_eof <= '1';
                else
                    hi := hex_val(l.all(line_pos + 1));
                    lo := hex_val(l.all(line_pos + 2));
                    line_pos := line_pos + 2;
                    o_byte <= std_logic_vector(to_unsigned(hi * 16 + lo, 8));
                    o_valid <= '1';
                    o_eof <= '0';
                end if;
            end if;
        end if;
    end process;

end architecture rtl;
