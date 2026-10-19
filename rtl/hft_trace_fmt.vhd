-- Rendering a decoded message as a trace record.
--
-- The trace format is specified in `rtl/TRACE_FORMAT.md`. This package
-- is the RTL side's implementation of the rendering half of it, and the
-- C++ side's implementation is `src/rtl/trace.hpp`. The two must
-- produce byte-identical text, which is what the differential checks.
--
-- Two rules the format fixes, and which are easy to get wrong:
--
--   * A field value is rendered at its WIDTH ON THE WIRE, in lowercase
--     hex, most significant byte first. ITCH is big endian, so a
--     4-byte share count of 100 is 00000064 on the wire and is
--     rendered 00000064.
--
--   * The declared length is a zero-padded four-digit DECIMAL, and the
--     record index is a plain decimal. The length is padded so that a
--     change in its width cannot change the line's shape.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

library work;
use work.hft_itch_pkg.all;

package hft_trace_fmt is

    -- The value of one hex character, 0..15. Used by the testbench to
    -- read the hex sidecar.
    function hex_val(c : character) return natural;

    -- The status character the format uses.
    function status_char(s : decode_status_t) return character;

    -- A decimal, with no leading space. `integer'image` gives one, and
    -- this removes the space it puts in front of a non-negative value.
    function dec(v : natural) return string;

    -- A decimal, zero-padded to at least four digits. A value too large
    -- for four digits is rendered in full rather than truncated.
    function dec4(v : natural) return string;

    -- One byte as two lowercase hex characters.
    function hex_byte(b : std_logic_vector(7 downto 0)) return string;

    -- A field value, rendered at its wire width.
    --
    -- The value is left-aligned in the 64-bit vector: the field's bytes
    -- occupy the top `width` bytes, most significant first, which is
    -- where the decoder leaves them.
    function hex_field(v : std_logic_vector(63 downto 0); width : natural) return string;

    -- The type token: two hex characters, or "--" when the decoder
    -- never reached the tag.
    function type_token(t : std_logic_vector(7 downto 0); has_type : boolean) return string;

end package hft_trace_fmt;

package body hft_trace_fmt is

    function hex_val(c : character) return natural is
    begin
        case c is
            when '0' to '9' => return character'pos(c) - character'pos('0');
            when 'a' to 'f' => return 10 + character'pos(c) - character'pos('a');
            when 'A' to 'F' => return 10 + character'pos(c) - character'pos('A');
            when others => return 0;
        end case;
    end function;

    function hex_digit(n : natural) return character is
    begin
        case n is
            when 0 => return '0';
            when 1 => return '1';
            when 2 => return '2';
            when 3 => return '3';
            when 4 => return '4';
            when 5 => return '5';
            when 6 => return '6';
            when 7 => return '7';
            when 8 => return '8';
            when 9 => return '9';
            when 10 => return 'a';
            when 11 => return 'b';
            when 12 => return 'c';
            when 13 => return 'd';
            when 14 => return 'e';
            when others => return 'f';
        end case;
    end function;

    function status_char(s : decode_status_t) return character is
    begin
        case s is
            when st_ok => return 'D';
            when st_skipped => return 'S';
            when st_bad_length => return 'L';
            when st_malformed => return 'M';
            when st_truncated => return 'T';
        end case;
    end function;

    function dec(v : natural) return string is
        constant kMaxWidth : positive := 32;
        variable img : string(1 to kMaxWidth) := (others => ' ');
        variable out_s : string(1 to kMaxWidth);
        variable n : natural := 0;
    begin
        img := integer'image(v);
        for i in img'range loop
            if img(i) /= ' ' then
                n := n + 1;
                out_s(n) := img(i);
            end if;
        end loop;
        return out_s(1 to n);
    end function;

    function dec4(v : natural) return string is
        constant kMaxWidth : positive := 32;
        variable img : string(1 to kMaxWidth) := (others => ' ');
        variable digits : string(1 to kMaxWidth) := (others => '0');
        variable n : natural := 0;
        variable padded : string(1 to 4) := (others => '0');
    begin
        img := integer'image(v);
        for i in img'range loop
            if img(i) /= ' ' then
                n := n + 1;
                digits(n) := img(i);
            end if;
        end loop;
        if n > 4 then
            return digits(1 to n);
        end if;
        for i in 1 to n loop
            padded(4 - n + i) := digits(i);
        end loop;
        return padded;
    end function;

    function hex_byte(b : std_logic_vector(7 downto 0)) return string is
        variable s : string(1 to 2);
    begin
        s(1) := hex_digit(to_integer(unsigned(b(7 downto 4))));
        s(2) := hex_digit(to_integer(unsigned(b(3 downto 0))));
        return s;
    end function;

    function hex_field(v : std_logic_vector(63 downto 0); width : natural) return string is
        variable s : string(1 to 16) := (others => '0');
        variable nibble : natural;
        variable bit_index : natural;
    begin
        for i in 1 to width loop
            for k in 0 to 1 loop
                nibble := 0;
                for b in 0 to 3 loop
                    bit_index := 63 - 8 * (i - 1) - (k * 4 + b);
                    case v(bit_index) is
                        when '0' =>
                            nibble := nibble * 2;
                        when '1' =>
                            nibble := nibble * 2 + 1;
                        when others =>
                            -- An unknown bit. The trace format has no
                            -- way to spell one, and the decoder never
                            -- produces one, so this is a hard failure
                            -- rather than a silent zero.
                            assert false
                                report "hex_field: a field bit is neither '0' nor '1'"
                                severity failure;
                    end case;
                end loop;
                if k = 0 then
                    s(2 * i - 1) := hex_digit(nibble);
                else
                    s(2 * i) := hex_digit(nibble);
                end if;
            end loop;
        end loop;
        return s(1 to 2 * width);
    end function;

    function type_token(t : std_logic_vector(7 downto 0); has_type : boolean) return string is
    begin
        if not has_type then
            return "--";
        end if;
        return hex_byte(t);
    end function;

end package body hft_trace_fmt;



