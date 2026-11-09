-- Decoded-message FIFO between the decoder (fast, ~1 msg / 30 cycles)
-- and the book (slow, tens to hundreds of cycles / message).
--
-- This is the skid/elastic buffer the streaming top-level needs: the
-- book applies one message at a time and asserts apply_done, the
-- decoder produces done pulses at wire rate. Without the FIFO the
-- only options are dropping messages (a silent book corruption) or
-- stalling the byte pipeline mid-message. With it, backpressure is
-- honest: `can_accept` deasserts at DEPTH-1 (one slot reserved for
-- the message currently being decoded), the framer stops consuming
-- bytes, and throughput falls to the book's rate.
--
-- Storage is a synchronous signal array so GHDL/Yosys infer BRAM
-- (or distributed RAM at small depth). FWFT: `rd_data` is valid
-- while `rd_valid='1'`, pop is a one-cycle pulse.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
library work;
use work.hft_itch_pkg.all;

entity hft_msg_fifo is
    generic (
        DEPTH : positive := 8
    );
    port (
        clk        : in  std_logic;
        rst        : in  std_logic;
        wr_valid   : in  std_logic;
        wr_type    : in  std_logic_vector(7 downto 0);
        wr_fields  : in  field_array_t;
        can_accept : out std_logic;
        rd_valid   : out std_logic;
        rd_type    : out std_logic_vector(7 downto 0);
        rd_fields  : out field_array_t;
        rd_pop     : in  std_logic;
        count      : out natural range 0 to DEPTH
    );
end entity hft_msg_fifo;

architecture rtl of hft_msg_fifo is
    type msg_t is record
        typ : std_logic_vector(7 downto 0);
        flds : field_array_t;
    end record;
    type mem_t is array (0 to DEPTH - 1) of msg_t;
    signal mem : mem_t := (others => (typ => (others => '0'), flds => (others => (others => '0'))));
    signal wr_ptr : natural range 0 to DEPTH - 1 := 0;
    signal rd_ptr : natural range 0 to DEPTH - 1 := 0;
    signal cnt : natural range 0 to DEPTH := 0;
begin
    can_accept <= '1' when cnt < DEPTH - 1 else '0';
    rd_valid <= '1' when cnt > 0 else '0';
    rd_type <= mem(rd_ptr).typ;
    rd_fields <= mem(rd_ptr).flds;
    count <= cnt;

    process (clk)
    begin
        if rising_edge(clk) then
            if rst = '1' then
                wr_ptr <= 0;
                rd_ptr <= 0;
                cnt <= 0;
            else
                if wr_valid = '1' and cnt < DEPTH then
                    mem(wr_ptr).typ <= wr_type;
                    mem(wr_ptr).flds <= wr_fields;
                    if wr_ptr = DEPTH - 1 then
                        wr_ptr <= 0;
                    else
                        wr_ptr <= wr_ptr + 1;
                    end if;
                end if;
                if rd_pop = '1' and cnt > 0 then
                    if rd_ptr = DEPTH - 1 then
                        rd_ptr <= 0;
                    else
                        rd_ptr <= rd_ptr + 1;
                    end if;
                end if;
                if wr_valid = '1' and cnt < DEPTH and not (rd_pop = '1' and cnt > 0) then
                    cnt <= cnt + 1;
                elsif rd_pop = '1' and cnt > 0 and not (wr_valid = '1' and cnt < DEPTH) then
                    cnt <= cnt - 1;
                end if;
            end if;
        end if;
    end process;
end architecture rtl;
