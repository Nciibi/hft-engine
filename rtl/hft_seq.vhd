-- MoldUDP64 sequence tracker: the RTL counterpart of
-- `include/hft/itch/sequence.hpp` (SequenceTracker::observe_packet).
--
-- One observation per PACKET, not per message: the sequence field
-- applies to the first block only, the rest are implicitly
-- sequential. A heartbeat (count 0) carries the next expected
-- sequence and is a resync OFFER, not a message: it is judged
-- (ok/gap/duplicate) but never counted and never moves `expected`.
-- End-of-session (count 65535) likewise moves nothing.
--
-- Arithmetic is modular 64-bit, exactly as the C++: delta =
-- observed - expected; delta = 0 is ok, 0 < delta < 2^63 is a
-- forward gap, delta >= 2^63 is duplicate/stale (including the
-- ambiguous half-space point, refused rather than classified).
-- `resync` rebases `expected` to `observed + count` after an
-- explicit snapshot rebuild -- resyncing the counter alone does
-- not repair the book, which is why it is an explicit input and
-- not an automatic recovery.
--
-- Synthesizable: only unsigned 64-bit add/sub/compare and counters.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity hft_seq is
    port (
        clk         : in  std_logic;
        rst         : in  std_logic;
        -- Packet observation, pulsed for one cycle (from hft_moldudp64).
        pkt_valid   : in  std_logic;
        pkt_seq     : in  std_logic_vector(63 downto 0);
        pkt_count   : in  std_logic_vector(15 downto 0);
        pkt_heartbeat : in std_logic;
        pkt_eos     : in  std_logic;
        -- Explicit rebase after a snapshot rebuild.
        resync      : in  std_logic;
        resync_seq  : in  std_logic_vector(63 downto 0);
        resync_count: in  std_logic_vector(15 downto 0);
        -- Outputs (registered, held until next observation).
        seq_state   : out natural range 0 to 2;  -- 0 ok, 1 gap, 2 duplicate
        seq_clean   : out std_logic;
        expected    : out std_logic_vector(63 downto 0);
        accepted    : out std_logic_vector(63 downto 0);
        missing     : out std_logic_vector(63 downto 0);
        gaps        : out std_logic_vector(63 downto 0);
        rejects     : out std_logic_vector(63 downto 0);
        resyncs     : out std_logic_vector(63 downto 0)
    );
end entity hft_seq;

architecture rtl of hft_seq is
    signal r_state : natural range 0 to 2 := 0;
    signal r_expected : unsigned(63 downto 0) := (others => '0');
    signal r_accepted : unsigned(63 downto 0) := (others => '0');
    signal r_missing : unsigned(63 downto 0) := (others => '0');
    signal r_gaps : unsigned(63 downto 0) := (others => '0');
    signal r_rejects : unsigned(63 downto 0) := (others => '0');
    signal r_resyncs : unsigned(63 downto 0) := (others => '0');
    signal r_started : std_logic := '0';
begin
    seq_state <= r_state;
    seq_clean <= '1' when (r_missing = 0 and r_rejects = 0) else '0';
    expected <= std_logic_vector(r_expected);
    accepted <= std_logic_vector(r_accepted);
    missing <= std_logic_vector(r_missing);
    gaps <= std_logic_vector(r_gaps);
    rejects <= std_logic_vector(r_rejects);
    resyncs <= std_logic_vector(r_resyncs);

    process (clk)
        variable delta : unsigned(63 downto 0);
        variable cnt : unsigned(63 downto 0);
    begin
        if rising_edge(clk) then
            if rst = '1' then
                r_state <= 0;
                r_expected <= (others => '0');
                r_accepted <= (others => '0');
                r_missing <= (others => '0');
                r_gaps <= (others => '0');
                r_rejects <= (others => '0');
                r_resyncs <= (others => '0');
                r_started <= '0';
            elsif resync = '1' then
                r_expected <= unsigned(resync_seq) + unsigned(resync_count);
                r_resyncs <= r_resyncs + 1;
                r_started <= '1';
                r_state <= 0;
            elsif pkt_valid = '1' then
                cnt := resize(unsigned(pkt_count), 64);
                if pkt_eos = '1' then
                    null;  -- end-of-session: reported by framer, no state
                elsif r_started = '0' then
                    -- First packet of the session establishes the base,
                    -- whatever its sequence number is.
                    r_started <= '1';
                    if pkt_heartbeat = '0' then
                        r_expected <= unsigned(pkt_seq) + cnt;
                        r_accepted <= r_accepted + cnt;
                        r_state <= 0;
                    else
                        r_expected <= unsigned(pkt_seq);
                        r_state <= 0;
                    end if;
                elsif pkt_heartbeat = '1' then
                    delta := unsigned(pkt_seq) - r_expected;
                    if delta = 0 then
                        r_state <= 0;
                    elsif delta < x"8000000000000000" then
                        r_missing <= r_missing + delta;
                        r_gaps <= r_gaps + 1;
                        r_state <= 1;
                    else
                        r_rejects <= r_rejects + 1;
                        r_state <= 2;
                    end if;
                else
                    delta := unsigned(pkt_seq) - r_expected;
                    if delta = 0 then
                        r_state <= 0;
                        r_expected <= unsigned(pkt_seq) + cnt;
                        r_accepted <= r_accepted + cnt;
                    elsif delta < x"8000000000000000" then
                        r_missing <= r_missing + delta;
                        r_gaps <= r_gaps + 1;
                        r_expected <= unsigned(pkt_seq) + cnt;
                        r_accepted <= r_accepted + cnt;
                        r_state <= 1;
                    else
                        r_rejects <= r_rejects + 1;
                        r_state <= 2;
                    end if;
                end if;
            end if;
        end if;
    end process;
end architecture rtl;
