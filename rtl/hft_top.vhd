-- Streaming system top-level: byte stream in, order book out.
--
--   in_byte/in_valid/in_eof  (AXI-stream; in_ready = backpressure)
--     -> hft_moldudp64 (framing, packet seq/count)
--     -> hft_itch_decode (single-pass field extraction)
--     -> hft_msg_fifo  (elastic buffer, DEPTH-1 usable)
--     -> hft_book_hw   (synthesizable sparse book, BRAM)
--     -> dump_* + book_checksum (in hardware)
--   hft_seq observes every packet header in parallel.
--
-- This replaces the testbench-only decode-then-apply array
-- (tb_book phase 1/2): messages flow decoder -> FIFO -> book in
-- lockstep, and when the FIFO fills the framer stops consuming
-- bytes (in_ready='0', source holds), so throughput falls to the
-- book's rate instead of dropping messages. The book applies one
-- message at a time (apply_start pulse, wait apply_done).
--
-- Deliberately NOT in this top: Ethernet/UDP (board MAC/PHY vendor
-- IP + pins, no bitstream in this repo), order entry (ITCH is an
-- outbound-only feed), and downstream decision logic (OMS, risk,
-- Avellaneda--Stoikov quoting, adverse selection) which in the C++
-- engine sit behind the book, not in the market-data path. See
-- rtl/README.md "What is not in the FPGA".

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
library work;
use work.hft_itch_pkg.all;
use work.hft_book_pkg.all;

entity hft_top is
    generic (
        MAX_ORDERS  : positive := 4096;
        MAX_LEVELS  : positive := 2048;
        TABLE_SLOTS : positive := 8192;
        FIFO_DEPTH  : positive := 8
    );
    port (
        clk       : in  std_logic;
        rst       : in  std_logic;
        in_valid  : in  std_logic;
        in_ready  : out std_logic;
        in_eof    : in  std_logic;
        in_byte   : in  std_logic_vector(7 downto 0);
        -- Book dump + in-hardware checksum.
        dump_start : in  std_logic;
        dump_valid : out std_logic;
        dump_kind  : out std_logic_vector(7 downto 0);
        dump_side  : out std_logic_vector(7 downto 0);
        dump_a     : out std_logic_vector(63 downto 0);
        dump_b     : out std_logic_vector(63 downto 0);
        dump_c     : out std_logic_vector(63 downto 0);
        dump_state : out order_state_t;
        dump_pos   : out natural;
        dump_done  : out std_logic;
        book_checksum : out std_logic_vector(63 downto 0);
        book_checksum_valid : out std_logic;
        -- Diagnostics.
        end_of_capture : out std_logic;
        fifo_count   : out natural range 0 to FIFO_DEPTH;
        book_busy    : out std_logic;
        seq_state    : out natural range 0 to 2;
        seq_clean    : out std_logic;
        seq_expected : out std_logic_vector(63 downto 0);
        seq_accepted : out std_logic_vector(63 downto 0);
        seq_missing  : out std_logic_vector(63 downto 0);
        seq_gaps     : out std_logic_vector(63 downto 0);
        seq_rejects  : out std_logic_vector(63 downto 0);
        last_symbol  : out std_logic_vector(63 downto 0);
        last_symbol_valid : out std_logic
    );
end entity hft_top;

architecture rtl of hft_top is
    signal fr_ready : std_logic := '0';
    signal fifo_can : std_logic := '0';
    signal fifo_cnt : natural range 0 to FIFO_DEPTH := 0;
    signal f_out_valid : std_logic := '0';
    signal f_out_byte : std_logic_vector(7 downto 0) := (others => '0');
    signal frame_start : std_logic := '0';
    signal frame_len : natural range 0 to 65535 := 0;
    signal frame_has_len : std_logic := '0';
    signal frame_trunc : std_logic := '0';
    signal end_cap : std_logic := '0';
    signal pkt_valid : std_logic := '0';
    signal pkt_seq : std_logic_vector(63 downto 0) := (others => '0');
    signal pkt_count : std_logic_vector(15 downto 0) := (others => '0');
    signal pkt_hb : std_logic := '0';
    signal pkt_eos : std_logic := '0';

    signal dec_done : std_logic := '0';
    signal dec_status : decode_status_t := st_truncated;
    signal dec_type : std_logic_vector(7 downto 0) := (others => '0');
    signal dec_has_type : std_logic := '0';
    signal dec_len : natural range 0 to 65535 := 0;
    signal dec_count : natural range 0 to 9 := 0;
    signal dec_fields : field_array_t := (others => (others => '0'));
    signal dec_symbol : std_logic_vector(63 downto 0) := (others => '0');
    signal dec_has_symbol : std_logic := '0';
    signal dec_cycles : natural range 0 to 65535 := 0;

    signal fifo_rd_valid : std_logic := '0';
    signal fifo_rd_type : std_logic_vector(7 downto 0) := (others => '0');
    signal fifo_rd_fields : field_array_t := (others => (others => '0'));
    signal fifo_rd_pop : std_logic := '0';

    signal apply_start : std_logic := '0';
    signal apply_type_r : std_logic_vector(7 downto 0) := (others => '0');
    signal apply_fields_r : field_array_t := (others => (others => '0'));
    signal apply_done : std_logic := '0';
    signal apply_status : book_status_t := bs_ok;
    signal apply_cycles : natural range 0 to natural'high := 0;

    signal book_busy_r : std_logic := '0';
    signal last_sym_r : std_logic_vector(63 downto 0) := (others => '0');
    signal last_sym_valid_r : std_logic := '0';
begin
    framer : entity work.hft_moldudp64
        port map (
            clk => clk, rst => rst,
            in_valid => in_valid, in_ready => in_ready,
            in_eof => in_eof, in_byte => in_byte,
            downstream_ready => fifo_can,
            out_valid => f_out_valid, out_byte => f_out_byte,
            frame_start => frame_start, frame_len => frame_len,
            frame_has_len => frame_has_len, frame_trunc => frame_trunc,
            pkt_seq_valid => pkt_valid, pkt_seq => pkt_seq,
            pkt_count => pkt_count, pkt_heartbeat => pkt_hb,
            pkt_eos => pkt_eos,
            end_of_capture => end_cap);

    decoder : entity work.hft_itch_decode
        port map (
            clk => clk, rst => rst,
            frame_start => frame_start, frame_len => frame_len,
            frame_has_len => frame_has_len, frame_trunc => frame_trunc,
            byte_valid => f_out_valid, byte_in => f_out_byte,
            done => dec_done, status => dec_status,
            type_byte => dec_type, has_type => dec_has_type,
            declared_len => dec_len, field_count => dec_count,
            fields => dec_fields, symbol => dec_symbol,
            has_symbol => dec_has_symbol, cycles => dec_cycles);

    fifo : entity work.hft_msg_fifo
        generic map (DEPTH => FIFO_DEPTH)
        port map (
            clk => clk, rst => rst,
            wr_valid => dec_done, wr_type => dec_type,
            wr_fields => dec_fields, can_accept => fifo_can,
            rd_valid => fifo_rd_valid, rd_type => fifo_rd_type,
            rd_fields => fifo_rd_fields, rd_pop => fifo_rd_pop,
            count => fifo_cnt);

    seq : entity work.hft_seq
        port map (
            clk => clk, rst => rst,
            pkt_valid => pkt_valid, pkt_seq => pkt_seq,
            pkt_count => pkt_count, pkt_heartbeat => pkt_hb,
            pkt_eos => pkt_eos, resync => '0',
            resync_seq => (others => '0'), resync_count => (others => '0'),
            seq_state => seq_state, seq_clean => seq_clean,
            expected => seq_expected, accepted => seq_accepted,
            missing => seq_missing, gaps => seq_gaps,
            rejects => seq_rejects, resyncs => open);

    book : entity work.hft_book_hw
        generic map (
            MAX_ORDERS => MAX_ORDERS, MAX_LEVELS => MAX_LEVELS,
            TABLE_SLOTS => TABLE_SLOTS)
        port map (
            clk => clk, rst => rst,
            apply_start => apply_start, apply_type => fifo_rd_type,
            apply_fields => fifo_rd_fields, apply_done => apply_done,
            apply_status => apply_status, apply_cycles => apply_cycles,
            dump_start => dump_start, dump_valid => dump_valid,
            dump_kind => dump_kind, dump_side => dump_side,
            dump_a => dump_a, dump_b => dump_b, dump_c => dump_c,
            dump_state => dump_state, dump_pos => dump_pos,
            dump_done => dump_done, book_checksum => book_checksum,
            book_checksum_valid => book_checksum_valid);

    end_of_capture <= end_cap;
    fifo_count <= fifo_cnt;
    book_busy <= book_busy_r;
    last_symbol <= last_sym_r;
    last_symbol_valid <= last_sym_valid_r;

    -- Book issue logic: pop+apply when idle and FIFO non-empty,
    -- hold apply_start one cycle (book latches it), wait apply_done.
    process (clk)
    begin
        if rising_edge(clk) then
            if rst = '1' then
                apply_start <= '0';
                fifo_rd_pop <= '0';
                book_busy_r <= '0';
                last_sym_r <= (others => '0');
                last_sym_valid_r <= '0';
            else
                apply_start <= '0';
                fifo_rd_pop <= '0';
                if dec_done = '1' then
                    last_sym_r <= dec_symbol;
                    last_sym_valid_r <= dec_has_symbol;
                end if;
                if book_busy_r = '0' then
                    if fifo_rd_valid = '1' then
                        apply_start <= '1';
                        fifo_rd_pop <= '1';
                        book_busy_r <= '1';
                    end if;
                elsif apply_done = '1' then
                    book_busy_r <= '0';
                end if;
            end if;
        end if;
    end process;
end architecture rtl;
