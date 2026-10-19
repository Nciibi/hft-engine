-- MoldUDP64 framing: a byte stream in, ITCH frames out.
--
-- This is the RTL side's implementation of the capture walk, written
-- from the Nasdaq MoldUDP64 Protocol Specification. The C++ side has
-- its own, in `src/rtl/capture_walk.hpp`, and the two are compared by
-- the differential.
--
-- The framing is deliberately NOT shared with the C++ side. If both
-- sides of the comparison used one reader, the framing would be that
-- reader's on both sides and a mistake in it would be invisible --
-- both would be reading frames the same wrong way.
--
-- ---- Layout ---------------------------------------------------------
--
--   Downstream Packet Header, 20 bytes
--     0  10  Session
--    10   8  Sequence Number, big endian
--    18   2  Message Count
--    20  ..  Message Blocks
--
--   Message Block
--     0   2  Length, big endian, EXCLUDING these two bytes
--     2  ..  Message data
--
-- A Message Block is byte-identical to an ITCH frame: the same two-byte
-- length prefix in front of the same body. The length read here is the
-- length the decoder reads again.
--
-- There is NO CHECKSUM in MoldUDP64. It is an unreliable transport
-- wrapper; integrity comes from SOUP, layered above it.
--
-- ---- The interface --------------------------------------------------
--
-- The framer takes a byte stream and produces a frame stream. A frame
-- begins with a one-cycle `frame_start` pulse carrying the declared
-- body length, and its body bytes follow on `out_valid`/`out_byte`,
-- one per cycle. A frame that runs out of input before its body is
-- complete is reported with `frame_trunc`.
--
-- `frame_has_len` says whether a length prefix was read at all. It
-- matters because the two truncation cases are different records: a
-- frame whose length prefix could not be read is truncated at length
-- zero with no type, while a frame whose body ran short is truncated
-- at its declared length with no type.
--
-- There is no backpressure. The framer consumes one byte per cycle and
-- forwards the body bytes at the same rate, so the decoder never has
-- to stall it, and the source never has to wait for it.

library ieee;
use ieee.std_logic_1164.all;

entity hft_moldudp64 is
    port (
        clk           : in  std_logic;
        rst           : in  std_logic;

        -- Byte stream in.
        in_valid      : in  std_logic;
        in_byte       : in  std_logic_vector(7 downto 0);

        -- Frame body bytes out.
        out_valid     : out std_logic;
        out_byte      : out std_logic_vector(7 downto 0);

        -- Frame control.
        frame_start   : out std_logic;
        frame_len     : out natural range 0 to 65535;
        frame_has_len : out std_logic;
        frame_trunc   : out std_logic;

        end_of_capture : out std_logic
    );
end entity hft_moldudp64;

architecture rtl of hft_moldudp64 is

    type state_t is (st_header, st_block_len, st_body, st_end);
    signal state : state_t := st_header;

    -- Bytes of the 20-byte packet header read so far.
    signal hdr_pos : natural range 0 to 19 := 0;
    -- The Message Count, and how many blocks of this packet are left.
    signal count : natural range 0 to 65535 := 0;
    signal blocks_left : natural range 0 to 65535 := 0;
    -- The block length, assembled from its two bytes.
    signal len_hi : natural range 0 to 255 := 0;
    signal len_lo : natural range 0 to 255 := 0;
    -- Body bytes still to come for the block in flight.
    signal body_left : natural range 0 to 65535 := 0;

    signal o_valid : std_logic := '0';
    signal o_byte : std_logic_vector(7 downto 0) := (others => '0');
    signal o_start : std_logic := '0';
    signal o_len : natural range 0 to 65535 := 0;
    signal o_has_len : std_logic := '0';
    signal o_trunc : std_logic := '0';
    signal o_end : std_logic := '0';

begin

    out_valid <= o_valid;
    out_byte <= o_byte;
    frame_start <= o_start;
    frame_len <= o_len;
    frame_has_len <= o_has_len;
    frame_trunc <= o_trunc;
    end_of_capture <= o_end;

    process (clk)
    begin
        if rising_edge(clk) then
            if rst = '1' then
                state <= st_header;
                hdr_pos <= 0;
                count <= 0;
                blocks_left <= 0;
                len_hi <= 0;
                len_lo <= 0;
                body_left <= 0;
                o_valid <= '0';
                o_byte <= (others => '0');
                o_start <= '0';
                o_len <= 0;
                o_has_len <= '0';
                o_trunc <= '0';
                o_end <= '0';
            else
                -- Every control output is a one-cycle pulse by default.
                o_valid <= '0';
                o_start <= '0';
                o_trunc <= '0';

                if in_valid = '1' then
                    case state is
                        when st_header =>
                            if hdr_pos = 18 then
                                len_hi <= to_integer(unsigned(in_byte));
                            elsif hdr_pos = 19 then
                                count <= len_hi * 256 + to_integer(unsigned(in_byte));
                                hdr_pos <= 0;
                                if (len_hi * 256 + to_integer(unsigned(in_byte))) = 65535 then
                                    -- End of session.
                                    state <= st_end;
                                    o_end <= '1';
                                elsif (len_hi * 256 + to_integer(unsigned(in_byte))) = 0 then
                                    -- A heartbeat carries the next expected
                                    -- sequence number and no messages. It
                                    -- is part of the transport, not the
                                    -- feed, so it carries no frame.
                                    state <= st_header;
                                else
                                    blocks_left <= len_hi * 256 + to_integer(unsigned(in_byte));
                                    len_hi <= 0;
                                    state <= st_block_len;
                                end if;
                            else
                                hdr_pos <= hdr_pos + 1;
                            end if;

                        when st_block_len =>
                            if len_hi = 0 and len_lo = 0 and blocks_left = blocks_left then
                                null;
                            end if;
                            if len_lo = 0 and len_hi = 0 then
                                -- first byte of the length prefix
                                len_hi <= to_integer(unsigned(in_byte));
                            else
                                len_lo <= to_integer(unsigned(in_byte));
                                o_len <= len_hi * 256 + to_integer(unsigned(in_byte));
                                o_has_len <= '1';
                                o_start <= '1';
                                body_left <= len_hi * 256 + to_integer(unsigned(in_byte));
                                state <= st_body;
                            end if;

                        when st_body =>
                            -- The byte in hand is a body byte: it is
                            -- forwarded and counted.
                            o_valid <= '1';
                            o_byte <= in_byte;
                            if body_left <= 1 then
                                body_left <= 0;
                                if blocks_left <= 1 then
                                    blocks_left <= 0;
                                    state <= st_header;
                                else
                                    blocks_left <= blocks_left - 1;
                                    len_hi <= 0;
                                    len_lo <= 0;
                                    state <= st_block_len;
                                end if;
                            else
                                body_left <= body_left - 1;
                            end if;

                        when st_end =>
                            null;
                    end case;
                else
                    -- The input has run out. What that means depends on
                    -- where the framer was.
                    case state is
                        when st_header =>
                            -- The capture stopped in the middle of a
                            -- packet header. There is no frame here,
                            -- only the end of the stream.
                            state <= st_end;
                            o_end <= '1';

                        when st_block_len =>
                            -- The length prefix itself could not be
                            -- read. The frame is truncated at length
                            -- zero, with no type.
                            state <= st_end;
                            o_end <= '1';
                            o_start <= '1';
                            o_len <= 0;
                            o_has_len <= '0';

                        when st_body =>
                            -- The body ran short. The frame is
                            -- truncated at its declared length, with
                            -- no type.
                            state <= st_end;
                            o_end <= '1';
                            o_trunc <= '1';

                        when st_end =>
                            null;
                    end case;
                end if;
            end if;
        end if;
    end process;

end architecture rtl;
