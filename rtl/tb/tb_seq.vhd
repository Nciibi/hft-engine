-- Sequence tracker unit test: hand-driven packets (ok, gap,
-- duplicate, heartbeat, resync, wrap-adjacent). Mirrors the C++
-- SequenceTracker cases in tests/test_units.cpp.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb_seq is
end entity tb_seq;

architecture sim of tb_seq is
    signal clk : std_logic := '0';
    signal rst : std_logic := '1';
    signal pkt_valid : std_logic := '0';
    signal pkt_seq : std_logic_vector(63 downto 0) := (others => '0');
    signal pkt_count : std_logic_vector(15 downto 0) := (others => '0');
    signal pkt_hb : std_logic := '0';
    signal pkt_eos : std_logic := '0';
    signal resync : std_logic := '0';
    signal resync_seq : std_logic_vector(63 downto 0) := (others => '0');
    signal resync_count : std_logic_vector(15 downto 0) := (others => '0');
    signal seq_state : natural range 0 to 2 := 0;
    signal seq_clean : std_logic := '0';
    signal expected_o : std_logic_vector(63 downto 0) := (others => '0');
    signal accepted_o : std_logic_vector(63 downto 0) := (others => '0');
    signal missing_o : std_logic_vector(63 downto 0) := (others => '0');
    signal gaps_o : std_logic_vector(63 downto 0) := (others => '0');
    signal rejects_o : std_logic_vector(63 downto 0) := (others => '0');
    signal resyncs_o : std_logic_vector(63 downto 0) := (others => '0');
    signal sim_done : std_logic := '0';

    procedure packet(seq : natural; cnt : natural; hb : std_logic) is
    begin
        wait until falling_edge(clk);
        pkt_valid <= '1';
        pkt_seq <= std_logic_vector(to_unsigned(seq, 64));
        pkt_count <= std_logic_vector(to_unsigned(cnt, 16));
        pkt_hb <= hb;
        pkt_eos <= '0';
        wait until falling_edge(clk);
        pkt_valid <= '0';
    end procedure;
begin
    dut : entity work.hft_seq
        port map (
            clk => clk, rst => rst, pkt_valid => pkt_valid,
            pkt_seq => pkt_seq, pkt_count => pkt_count,
            pkt_heartbeat => pkt_hb, pkt_eos => pkt_eos,
            resync => resync, resync_seq => resync_seq,
            resync_count => resync_count, seq_state => seq_state,
            seq_clean => seq_clean, expected => expected_o,
            accepted => accepted_o, missing => missing_o,
            gaps => gaps_o, rejects => rejects_o, resyncs => resyncs_o);

    clock_gen : process
    begin
        while sim_done = '0' loop
            clk <= '0'; wait for 5 ns;
            clk <= '1'; wait for 5 ns;
        end loop;
        wait;
    end process;

    driver : process
    begin
        wait for 40 ns;
        rst <= '0';
        wait until falling_edge(clk);
        -- Base packet seq=100 count=3 -> ok, expected 103, accepted 3
        packet(100, 3, '0');
        assert seq_state = 0 report "base packet not ok" severity failure;
        assert expected_o = std_logic_vector(to_unsigned(103, 64)) report "base expected wrong" severity failure;
        -- Contiguous seq=103 count=2 -> ok, expected 105
        packet(103, 2, '0');
        assert seq_state = 0 report "contiguous not ok" severity failure;
        assert expected_o = std_logic_vector(to_unsigned(105, 64)) report "contiguous expected wrong" severity failure;
        -- Gap seq=108 count=1 -> gap, missing 3, expected 109
        packet(108, 1, '0');
        assert seq_state = 1 report "gap not detected" severity failure;
        assert missing_o = std_logic_vector(to_unsigned(3, 64)) report "gap missing count wrong" severity failure;
        assert gaps_o = std_logic_vector(to_unsigned(1, 64)) report "gap count wrong" severity failure;
        assert seq_clean = '0' report "clean after gap" severity failure;
        -- Duplicate seq=100 count=1 -> duplicate, rejects 1, expected stays 109
        packet(100, 1, '0');
        assert seq_state = 2 report "duplicate not detected" severity failure;
        assert rejects_o = std_logic_vector(to_unsigned(1, 64)) report "reject count wrong" severity failure;
        assert expected_o = std_logic_vector(to_unsigned(109, 64)) report "duplicate moved expected" severity failure;
        -- Heartbeat seq=109 -> ok, nothing moves
        packet(109, 0, '1');
        assert seq_state = 0 report "heartbeat ok case failed" severity failure;
        assert accepted_o = std_logic_vector(to_unsigned(6, 64)) report "heartbeat counted" severity failure;
        -- Heartbeat seq=115 (sender ahead) -> gap, missing += 6
        packet(115, 0, '1');
        assert seq_state = 1 report "heartbeat gap not detected" severity failure;
        assert missing_o = std_logic_vector(to_unsigned(9, 64)) report "heartbeat gap missing wrong" severity failure;
        -- Resync to seq=1000 count=5 -> expected 1005, resyncs 1
        wait until falling_edge(clk);
        resync <= '1';
        resync_seq <= std_logic_vector(to_unsigned(1000, 64));
        resync_count <= std_logic_vector(to_unsigned(5, 16));
        wait until falling_edge(clk);
        resync <= '0';
        assert expected_o = std_logic_vector(to_unsigned(1005, 64)) report "resync expected wrong" severity failure;
        assert resyncs_o = std_logic_vector(to_unsigned(1, 64)) report "resync count wrong" severity failure;
        -- Rebase to just before the 64-bit wrap: expected = 2^64-2.
        wait until falling_edge(clk);
        resync <= '1';
        resync_seq <= x"FFFFFFFFFFFFFFFC";
        resync_count <= std_logic_vector(to_unsigned(2, 16));
        wait until falling_edge(clk);
        resync <= '0';
        assert expected_o = x"FFFFFFFFFFFFFFFE" report "wrap rebase wrong" severity failure;
        -- Packet at 2^64-2 count=2 -> ok, expected wraps/refills to 0.
        wait until falling_edge(clk);
        pkt_valid <= '1';
        pkt_seq <= x"FFFFFFFFFFFFFFFE";
        pkt_count <= std_logic_vector(to_unsigned(2, 16));
        pkt_hb <= '0';
        pkt_eos <= '0';
        wait until falling_edge(clk);
        pkt_valid <= '0';
        assert seq_state = 0 report "wrap packet not ok" severity failure;
        assert expected_o = std_logic_vector(to_unsigned(0, 64)) report "wrap expected wrong" severity failure;
        report "tb_seq PASS";
        sim_done <= '1';
        wait;
    end process;
end architecture sim;
