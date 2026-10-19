-- The ITCH 5.0 field tables, transcribed from the Nasdaq TotalView-ITCH
-- 5.0 interface specification.
--
-- This is the RTL side's copy of the protocol, and it is a SEPARATE
-- transcription from the one in `include/hft/itch/protocol.hpp`. That is
-- the entire point of the differential: two implementations written
-- from the same published document, compared field by field.
--
-- A shared header would defeat the exercise. If the two sides shared
-- one table, a mistake in it would appear on both sides and the
-- comparison would report agreement it had not checked -- which is
-- precisely how the Add Order price/size mix-up survived 512 green
-- checks in the C++ engine alone.
--
-- The published field tables, one per message:
--
--   'A' Add Order (1.3.1)
--     1  2  Stock Locate          5  6  Timestamp
--     3  2  Tracking Number      11  8  Order Reference Number
--    19  1  Buy/Sell Indicator   20  4  Shares
--    24  8  Stock                32  4  Price                 -> 36 bytes
--
--   'E' Order Executed
--     1  2  Stock Locate          5  6  Timestamp
--     3  2  Tracking Number      11  8  Order Reference Number
--    19  4  Shares               23  8  Match Number      -> 31 bytes
--
--   NO Printable byte. The message ends at the match number, and a
--   decoder that read a Printable byte there would read the next
--   message's length prefix as a flag. This was found on real market
--   data, after the table had been wrong here and in the C++ engine
--   for the whole life of the project; see docs/BUGS.md.
--
--   'C' Order Executed With Price (1.4.2)
--     as 'E', plus 32  4  Execution Price                   -> 36 bytes
--
--   'X' Order Cancel
--     1  2  Stock Locate          5  6  Timestamp
--     3  2  Tracking Number      11  8  Order Reference Number
--    19  4  Shares                                        -> 23 bytes
--
--   'D' Order Delete
--     1  2  Stock Locate          5  6  Timestamp
--     3  2  Tracking Number      11  8  Order Reference Number
--                                                           -> 19 bytes
--
--   'U' Order Replace (4.4.5)
--     1  2  Stock Locate          5  6  Timestamp
--     3  2  Tracking Number      11  8  Original Reference
--    19  8  New Reference        27  4  Shares
--    31  4  Price                                           -> 35 bytes
--
--   'B' Broken Trade (4.5.3)
--     1  2  Stock Locate          5  6  Timestamp
--     3  2  Tracking Number      11  8  Match Number        -> 19 bytes
--
--   'P' Trade, non-cross (1.5.1)
--     1  2  Stock Locate          5  6  Timestamp
--     3  2  Tracking Number      11  8  Order Reference Number
--    19  1  Buy/Sell Indicator   20  4  Shares
--    32  4  Price                36  8  Match Number        -> 44 bytes
--
--   'Q' Cross Trade (1.5.2)
--     1  2  Stock Locate          5  6  Timestamp
--     3  2  Tracking Number      11  8  Shares      <- EIGHT bytes
--    27  4  Price                31  8  Match Number
--    39  1  Cross Type                                      -> 40 bytes
--
-- Two of these tables are shorter than the wire, and deliberately so.
-- 'P' has a stock symbol at offsets 24..31 and 'Q' has one at 19..26,
-- and neither is carried: the decoder steps over them to reach the
-- price. The trace records what the decoder extracts, so neither
-- symbol appears in it.
--
-- The 'Q' share count is EIGHT bytes, against four in every other
-- message in the protocol. Getting that width wrong shifts every
-- following field.

library ieee;
use ieee.std_logic_1164.all;

package hft_itch_pkg is

    -- What the decoder decided about a frame.
    --
    -- st_truncated covers both truncation cases: a length prefix that
    -- could not be read, and a body that ran out early. A consumer of
    -- the trace asks the same question about both -- "is there more to
    -- come?" -- and gets the same answer.
    type decode_status_t is (st_ok, st_skipped, st_bad_length, st_malformed, st_truncated);

    -- The index into the field tables. 9 means "a type this build does
    -- not decode", which is a skip rather than an error.
    subtype type_index_t is natural range 0 to 9;
    constant kUnknownType : type_index_t := 9;

    -- The field tables are sixteen entries wide, not nine.
    --
    -- The row STRIDE is what matters, and it is a power of two on
    -- purpose. A two-dimensional constant array is addressed as
    -- `row * stride + column`, and with a stride of nine that address
    -- is a MULTIPLICATION -- which the first synthesis run put
    -- straight onto the decoder's critical path, as an 18x18
    -- multiplier, for no reason a hardware engineer would accept.
    --
    -- With a stride of sixteen the address is a shift, and the decoder
    -- gets a cycle back. The unused slots cost nothing: the table is
    -- constants.
    subtype field_index_t is natural range 0 to 15;
    constant kMaxFields : natural := 9;

    -- The extracted fields, each up to eight bytes wide.
    type field_array_t is array (field_index_t) of std_logic_vector(63 downto 0);

    -- The first line of a trace file. A comment, so a reader can skip
    -- it and a human can recognise the file.
    constant kTraceHeader : string := "# hft-rtl-trace v1";

    type offset_table_t is array (type_index_t, field_index_t) of natural range 0 to 64;
    type width_table_t is array (type_index_t, field_index_t) of natural range 0 to 8;
    -- The END offset of each field, precomputed.
    --
    -- The parser needs to know whether a byte falls inside the current
    -- field, and the obvious way to ask is
    --
    --     off >= start  and  off < start + width
    --
    -- which puts a 7-bit adder in front of a 16-bit comparator. The
    -- first synthesis run said so: the decoder closed at 32.5 MHz and
    -- the critical path was a carry chain that added two table
    -- outputs together.
    --
    -- The end offset is a CONSTANT of the table, so it can simply be
    -- tabulated. The table carries one more number per field and the
    -- adder leaves the critical path.
    type end_table_t is array (type_index_t, field_index_t) of natural range 0 to 64;
    type count_table_t is array (type_index_t) of natural range 0 to 9;
    type length_table_t is array (type_index_t) of natural range 0 to 64;
    type flag_table_t is array (type_index_t) of boolean;

    -- Start offset of each field, in the message body. Zero means the
    -- slot is unused: a type with seven fields has no eighth.
    constant kFieldOffset : offset_table_t := (
        -- 'A' Add Order
        0 => (1, 3, 5, 11, 19, 20, 24, 32, 0, 0, 0, 0, 0, 0, 0, 0),
        -- 'E' Order Executed
        1 => (1, 3, 5, 11, 19, 23, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        -- 'C' Order Executed At Price
        2 => (1, 3, 5, 11, 19, 23, 31, 32, 0, 0, 0, 0, 0, 0, 0, 0),
        -- 'X' Order Cancel
        3 => (1, 3, 5, 11, 19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        -- 'D' Order Delete
        4 => (1, 3, 5, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        -- 'U' Order Replace
        5 => (1, 3, 5, 11, 19, 27, 31, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        -- 'B' Broken Trade
        6 => (1, 3, 5, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        -- 'P' Trade
        7 => (1, 3, 5, 11, 19, 20, 32, 36, 0, 0, 0, 0, 0, 0, 0, 0),
        -- 'Q' Cross Trade
        8 => (1, 3, 5, 11, 27, 31, 39, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        -- not a type this build decodes
        9 => (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));

    -- Width of each field, in bytes.
    constant kFieldWidth : width_table_t := (
        0 => (2, 2, 6, 8, 1, 4, 8, 4, 0, 0, 0, 0, 0, 0, 0, 0),
        1 => (2, 2, 6, 8, 4, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        2 => (2, 2, 6, 8, 4, 8, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0),
        3 => (2, 2, 6, 8, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        4 => (2, 2, 6, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        5 => (2, 2, 6, 8, 8, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        6 => (2, 2, 6, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        7 => (2, 2, 6, 8, 1, 4, 4, 8, 0, 0, 0, 0, 0, 0, 0, 0),
        8 => (2, 2, 6, 8, 8, 4, 8, 1, 0, 0, 0, 0, 0, 0, 0, 0),
        9 => (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));

    -- The end offset of each field: the offset of the byte after it.
    constant kFieldEnd : end_table_t := (
        0 => (3, 5, 11, 19, 20, 24, 32, 36, 0, 0, 0, 0, 0, 0, 0, 0),
        1 => (3, 5, 11, 19, 23, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        2 => (3, 5, 11, 19, 23, 31, 32, 36, 0, 0, 0, 0, 0, 0, 0, 0),
        3 => (3, 5, 11, 19, 23, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        4 => (3, 5, 11, 19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        5 => (3, 5, 11, 19, 27, 31, 35, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        6 => (3, 5, 11, 19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        7 => (3, 5, 11, 19, 20, 24, 36, 44, 0, 0, 0, 0, 0, 0, 0, 0),
        8 => (3, 5, 11, 19, 27, 31, 39, 40, 0, 0, 0, 0, 0, 0, 0, 0),
        9 => (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));

    -- How many fields each type carries.
    constant kFieldCount : count_table_t := (8, 6, 8, 5, 4, 7, 4, 8, 7, 0);

    -- The body length each type requires, in bytes. A frame whose
    -- declared length disagrees with this is a BAD LENGTH, not a
    -- message with different fields.
    constant kBodyLength : length_table_t := (36, 31, 36, 23, 19, 35, 19, 44, 40, 0);

    -- Whether the message is keyed on an order reference, and a
    -- reference of zero is therefore malformed.
    --
    -- It is NOT the same for every message, and pretending otherwise is
    -- how a correct frame gets rejected:
    --
    --   'A' 'E' 'C' 'X' 'D' 'U'  order reference -- must be live
    --   'P' Trade                order reference, but the binary feeds
    --                             populate it with ZERO by design
    --   'B' Broken Trade         a MATCH number, not an order
    --   'Q' Cross Trade          an eight-byte share count, which the
    --                             specification says may be zero when
    --                             interest is insufficient to cross
    constant kOrderKeyed : flag_table_t := (true, true, true, true, true, true, false, false, false, false);

    -- The table index for a tag byte.
    function type_index_of(tag : std_logic_vector(7 downto 0)) return type_index_t;

    -- The tag byte for a table index. The inverse of the above.
    --
    -- The return type is unconstrained on purpose. A function's result
    -- is unconstrained, so a constrained array type mark is not legal
    -- here -- `std_logic_vector(7 downto 0)` is a parse error, and
    -- `std_logic_vector` is the correct spelling.
    function type_tag(t : type_index_t) return std_logic_vector;

    -- A short name for a type, for a failure message.
    function type_name(t : type_index_t) return string;

    -- Is this a side byte the specification allows? 'B' or 'S' and
    -- nothing else. An unrecognised byte is a protocol violation that
    -- must surface rather than be coerced into a default side.
    function side_is_valid(v : std_logic_vector(63 downto 0)) return boolean;

end package hft_itch_pkg;

package body hft_itch_pkg is

    -- The number of fields a type's width table actually describes.
    --
    -- This exists because a count that disagrees with the widths is a
    -- mistake that HIDES. When the Order Executed message lost its
    -- Printable byte, the width row was updated and the count was not,
    -- and the two sides still agreed -- because the extra field
    -- rendered as an EMPTY STRING, the trace's tokeniser dropped it,
    -- and a record with six fields passed a check that expected six.
    --
    -- The count is therefore derived from the widths and compared with
    -- the declared count, at elaboration, so a table that disagrees
    -- with itself stops the build instead of quietly agreeing with the
    -- other side.
    function described_field_count(t : type_index_t) return natural is
        variable n : natural := 0;
    begin
        for i in field_index_t loop
            if kFieldWidth(t, i) = 0 then
                return n;
            end if;
            n := n + 1;
        end loop;
        return n;
    end function;

    function type_index_of(tag : std_logic_vector(7 downto 0)) return type_index_t is
    begin
        case tag is
            when x"41" => return 0;  -- 'A'
            when x"45" => return 1;  -- 'E'
            when x"43" => return 2;  -- 'C'
            when x"58" => return 3;  -- 'X'
            when x"44" => return 4;  -- 'D'
            when x"55" => return 5;  -- 'U'
            when x"42" => return 6;  -- 'B'
            when x"50" => return 7;  -- 'P'
            when x"51" => return 8;  -- 'Q'
            when others => return kUnknownType;
        end case;
    end function;

    function type_tag(t : type_index_t) return std_logic_vector is
        -- A named constant rather than an aggregate, because the
        -- return type is unconstrained and an `others` choice needs a
        -- length to resolve against.
        constant kZero : std_logic_vector(7 downto 0) := (others => '0');
    begin
        case t is
            when 0 => return x"41";
            when 1 => return x"45";
            when 2 => return x"43";
            when 3 => return x"58";
            when 4 => return x"44";
            when 5 => return x"55";
            when 6 => return x"42";
            when 7 => return x"50";
            when 8 => return x"51";
            when others => return kZero;
        end case;
    end function;

    function type_name(t : type_index_t) return string is
    begin
        case t is
            when 0 => return "Add Order";
            when 1 => return "Order Executed";
            when 2 => return "Order Executed At Price";
            when 3 => return "Order Cancel";
            when 4 => return "Order Delete";
            when 5 => return "Order Replace";
            when 6 => return "Broken Trade";
            when 7 => return "Trade";
            when 8 => return "Cross Trade";
            when others => return "unknown";
        end case;
    end function;

    function side_is_valid(v : std_logic_vector(63 downto 0)) return boolean is
    begin
        return v(7 downto 0) = x"42" or v(7 downto 0) = x"53";
    end function;

end package body hft_itch_pkg;





