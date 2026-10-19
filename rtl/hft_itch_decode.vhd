-- The ITCH decoder: a message body in, the extracted fields out.
--
-- This is the RTL side's implementation of the decode rules, written
-- from the Nasdaq TotalView-ITCH 5.0 interface specification and from
-- `rtl/TRACE_FORMAT.md`. The C++ side has its own, in
-- `src/itch/decode.cpp`, and the two are compared field by field by the
-- differential.
--
-- ---- The shape of it -------------------------------------------------
--
-- The decoder is a single-pass streaming parser. It is handed a message
-- body one byte at a time and extracts each field as the bytes of that
-- field arrive. It does not buffer the body and decode it afterwards,
-- because a design that buffers is a design with a second copy of the
-- message in it, and the whole point of doing this in hardware is that
-- the message never has to exist anywhere but in the pipeline.
--
-- The field table in `hft_itch_pkg` says where each field starts and
-- how wide it is, so the parser does not need a state machine per
-- message type. It tracks the current field and the offset it is
-- expecting, and a byte that falls in a gap -- the stock symbol in
-- 'P' and 'Q' -- belongs to no field and is discarded.
--
-- The parser takes one byte per cycle, so a message costs as many
-- cycles as it has bytes. That is the cycle count the measurement
-- phase will publish, and it is exact: a property of the design, not
-- of the machine running it.
--
-- ---- The decisions, in order ------------------------------------------
--
-- The order matters, because the trace records which decision was
-- reached:
--
--   1. No length prefix at all -> TRUNCATED, length 0, no type.
--   2. A length prefix of zero  -> BAD LENGTH, length 0, no type.
--   3. A type this build does not decode -> SKIPPED, with the type.
--   4. A length that disagrees with the type -> BAD LENGTH, with the
--      type.
--   5. An order-keyed message with a zero reference -> MALFORMED.
--   6. An Add Order whose side byte is not 'B' or 'S' -> MALFORMED.
--   7. Otherwise -> DECODED, with the fields.
--
-- The type is recorded only once the whole body is present, because
-- that is when the decoder reads it. A truncated body has no type even
-- though its first byte is on the wire, and two implementations that
-- disagree about that are disagreeing about where the line between
-- "not enough data yet" and "this frame is wrong" is drawn.

library ieee;
use ieee.std_logic_1164.all;

library work;
use work.hft_itch_pkg.all;

entity hft_itch_decode is
    port (
        clk           : in  std_logic;
        rst           : in  std_logic;

        -- Frame control, from the framer.
        frame_start   : in  std_logic;
        frame_len     : in  natural range 0 to 65535;
        frame_has_len : in  std_logic;
        frame_trunc   : in  std_logic;

        -- The body bytes.
        byte_valid    : in  std_logic;
        byte_in       : in  std_logic_vector(7 downto 0);

        -- The result.
        done          : out std_logic;
        status        : out hft_itch_pkg.decode_status_t;
        type_byte     : out std_logic_vector(7 downto 0);
        has_type      : out std_logic;
        declared_len  : out natural range 0 to 65535;
        field_count   : out natural range 0 to 9;
        fields        : out hft_itch_pkg.field_array_t;
        cycles        : out natural range 0 to 65535
    );
end entity hft_itch_decode;

architecture rtl of hft_itch_decode is

    signal busy : std_logic := '0';

    signal r_done : std_logic := '0';
    signal r_status : hft_itch_pkg.decode_status_t := hft_itch_pkg.st_truncated;
    signal r_type : std_logic_vector(7 downto 0) := (others => '0');
    signal r_has_type : std_logic := '0';
    signal r_declared : natural range 0 to 65535 := 0;
    signal r_field_count : natural range 0 to 9 := 0;
    signal r_fields : hft_itch_pkg.field_array_t := (others => (others => '0'));
    signal r_cycles : natural range 0 to 65535 := 0;

begin

    done <= r_done;
    status <= r_status;
    type_byte <= r_type;
    has_type <= r_has_type;
    declared_len <= r_declared;
    field_count <= r_field_count;
    fields <= r_fields;
    cycles <= r_cycles;

    process (clk)
        -- The parser's own state. These are registers, held between
        -- clock edges like any other.
        variable type_idx : hft_itch_pkg.type_index_t := hft_itch_pkg.kUnknownType;
        variable fld_idx : natural range 0 to 9 := 0;
        variable in_field : natural range 0 to 8 := 0;
        variable acc : std_logic_vector(63 downto 0) := (others => '0');
        variable off : natural range 0 to 65535 := 0;
        variable remaining : natural range 0 to 65535 := 0;
        variable declared : natural range 0 to 65535 := 0;
        variable flds : hft_itch_pkg.field_array_t := (others => (others => '0'));
        variable cyc : natural range 0 to 65535 := 0;
    begin
        if rising_edge(clk) then
            if rst = '1' then
                busy <= '0';
                r_done <= '0';
                r_status <= hft_itch_pkg.st_truncated;
                r_type <= (others => '0');
                r_has_type <= '0';
                r_declared <= 0;
                r_field_count <= 0;
                r_fields <= (others => (others => '0'));
                r_cycles <= 0;
            else
                r_done <= '0';

                if frame_start = '1' then
                    cyc := 0;
                    off := 0;
                    fld_idx := 0;
                    in_field := 0;
                    acc := (others => '0');
                    flds := (others => (others => '0'));
                    type_idx := hft_itch_pkg.kUnknownType;
                    declared := frame_len;
                    r_declared <= frame_len;

                    if frame_has_len = '0' then
                        -- The length prefix could not be read at all.
                        -- The frame is truncated at length zero, with
                        -- no type: there is no tag byte on the wire.
                        busy <= '0';
                        r_status <= hft_itch_pkg.st_truncated;
                        r_type <= (others => '0');
                        r_has_type <= '0';
                        r_field_count <= 0;
                        r_cycles <= 0;
                        r_done <= '1';
                    elsif frame_len = 0 then
                        -- A body of zero bytes. The decoder stops before
                        -- the tag, so there is no type -- but this is a
                        -- BAD LENGTH, not a truncation: the frame
                        -- declared a length and it was wrong.
                        busy <= '0';
                        r_status <= hft_itch_pkg.st_bad_length;
                        r_type <= (others => '0');
                        r_has_type <= '0';
                        r_field_count <= 0;
                        r_cycles <= 0;
                        r_done <= '1';
                    else
                        busy <= '1';
                        remaining := frame_len;
                    end if;

                elsif busy = '1' then
                    cyc := cyc + 1;

                    if frame_trunc = '1' then
                        -- The body ran short. The frame is truncated at
                        -- its declared length, with no type.
                        busy <= '0';
                        r_status <= hft_itch_pkg.st_truncated;
                        r_type <= (others => '0');
                        r_has_type <= '0';
                        r_field_count <= 0;
                        r_cycles <= cyc;
                        r_done <= '1';

                    elsif byte_valid = '1' then
                        if off = 0 then
                            r_type <= byte_in;
                            r_has_type <= '1';
                            type_idx := hft_itch_pkg.type_index_of(byte_in);
                        end if;

                        if type_idx /= hft_itch_pkg.kUnknownType and
                           fld_idx < hft_itch_pkg.kFieldCount(type_idx) then
                            if off >= hft_itch_pkg.kFieldOffset(type_idx, fld_idx) and
                               off < hft_itch_pkg.kFieldOffset(type_idx, fld_idx) +
                                    hft_itch_pkg.kFieldWidth(type_idx, fld_idx) then
                                if in_field = 0 then
                                    acc := (others => '0');
                                end if;
                                acc := acc(55 downto 0) & byte_in;
                                in_field := in_field + 1;
                                if in_field = hft_itch_pkg.kFieldWidth(type_idx, fld_idx) then
                                    flds(fld_idx) := acc;
                                    fld_idx := fld_idx + 1;
                                    in_field := 0;
                                end if;
                            end if;
                        end if;

                        if remaining = 1 then
                            -- The last body byte. The body is complete,
                            -- and the type is now known.
                            busy <= '0';
                            r_cycles <= cyc;

                            if type_idx = hft_itch_pkg.kUnknownType then
                                -- A well-formed frame of a type this
                                -- build does not decode. The type is
                                -- recorded, because a handler that
                                -- skipped a frame needs to know what it
                                -- skipped.
                                r_status <= hft_itch_pkg.st_skipped;
                                r_has_type <= '1';
                                r_field_count <= 0;
                            elsif declared /= hft_itch_pkg.kBodyLength(type_idx) then
                                -- The declared length disagrees with the
                                -- type. The frame is not read as though
                                -- its fields were laid out differently.
                                r_status <= hft_itch_pkg.st_bad_length;
                                r_has_type <= '1';
                                r_field_count <= 0;
                            elsif hft_itch_pkg.kOrderKeyed(type_idx) and
                                  flds(3) = x"0000000000000000" then
                                -- The message is keyed on an order
                                -- reference and the reference is the
                                -- reserved zero.
                                r_status <= hft_itch_pkg.st_malformed;
                                r_has_type <= '1';
                                r_field_count <= 0;
                            elsif type_idx = 0 and not hft_itch_pkg.side_is_valid(flds(4)) then
                                -- An Add Order whose side byte is
                                -- neither 'B' nor 'S'. Silently reading
                                -- an unknown side as a bid corrupts the
                                -- book in a way no downstream test would
                                -- catch.
                                r_status <= hft_itch_pkg.st_malformed;
                                r_has_type <= '1';
                                r_field_count <= 0;
                            else
                                r_status <= hft_itch_pkg.st_ok;
                                r_has_type <= '1';
                                r_field_count <= hft_itch_pkg.kFieldCount(type_idx);
                                r_fields <= flds;
                            end if;

                            r_done <= '1';
                        end if;

                        off := off + 1;
                        remaining := remaining - 1;
                    end if;
                end if;
            end if;
        end if;
    end process;

end architecture rtl;
