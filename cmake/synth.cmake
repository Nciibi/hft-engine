# Synthesis: area and timing, for the RTL model.
#
# The RTL is written as a behavioural model, and this is the check that
# says which parts of it are realisable hardware and which are not.
#
# The flow is the open one, and it needs no licence and no board:
#
#   1. ghdl --synth      the VHDL, to Verilog
#   2. yosys              to a target's primitives
#   3. nextpnr            place, route, and report the timing
#
# The numbers that come out are the two a hardware interview asks for:
# how big is it, and how fast does it run.
#
# Two of them are exactly what this repository wanted and could not have
# from the C++ side. A cycle count in RTL is exact and portable; a
# latency in software is a property of a machine. And an area figure is
# the one number that says whether any of this is realisable at all.
#
# There are two books. `hft_book` is the behavioural model: its pools
# are process variables and its dense bitmap scans use loop bounds GHDL
# requires to be static, so it is not synthesized by default. Pass
# HFT_SYNTH_BOOK=ON to reproduce that failure; the report always carries
# the row. `hft_book_hw` is the hardware core: synchronous arrays,
# inferred block RAM, and the same external interface. It needs an 85F
# because its state is about 2 Mbit, so the device is chosen per top.
# The report always carries the behavioural row, because "this part of
# the model is not hardware" is a thing a reader needs to know.
#
# Invoked as:
#   cmake -DGHDL=... -DYOSYS=... -DNEXTPNR=... -DRTL_DIR=... -DWORK=...
#         -DREPORT=... [-DTOP=...] [-DFREQ=100] -P synth.cmake

foreach(required GHDL YOSYS NEXTPNR RTL_DIR WORK REPORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} was not passed to the script")
    endif()
endforeach()

if(NOT DEFINED FREQ)
    set(FREQ 100)
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# The designs to synthesise by default. The framer and the decoder are
# the ones with a realisable boundary; the book is measured separately.
if(NOT DEFINED TOP)
    set(TOP hft_moldudp64 hft_itch_decode hft_book_hw)
endif()

# The sources, in dependency order: the packages, then the entities.
set(sources
    "${RTL_DIR}/hft_itch_pkg.vhd"
    "${RTL_DIR}/hft_trace_fmt.vhd"
    "${RTL_DIR}/hft_book_pkg.vhd"
    "${RTL_DIR}/hft_hex_reader.vhd"
    "${RTL_DIR}/hft_moldudp64.vhd"
    "${RTL_DIR}/hft_itch_decode.vhd"
    "${RTL_DIR}/hft_book.vhd"
    "${RTL_DIR}/hft_book_hw.vhd")

set(analyse "")
foreach(src IN LISTS sources)
    list(APPEND analyse "${src}")
endforeach()

execute_process(
    COMMAND "${GHDL}" -a --std=08 "--workdir=${WORK}" ${analyse}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "ghdl -a failed (${status}): ${output_text}${error_text}")
endif()

execute_process(
    COMMAND "${GHDL}" --version
    OUTPUT_VARIABLE ghdl_version
    ERROR_VARIABLE ghdl_error
    RESULT_VARIABLE ghdl_status)
if(NOT ghdl_status EQUAL 0)
    set(ghdl_version "unknown")
endif()
string(STRIP "${ghdl_version}" ghdl_version)
string(REGEX REPLACE "[\r\n].*" "" ghdl_version "${ghdl_version}")

execute_process(
    COMMAND "${YOSYS}" -V
    OUTPUT_VARIABLE yosys_version
    ERROR_VARIABLE yosys_error
    RESULT_VARIABLE yosys_status)
if(NOT yosys_status EQUAL 0)
    set(yosys_version "unknown")
endif()
string(STRIP "${yosys_version}" yosys_version)
string(REGEX REPLACE "[\r\n].*" "" yosys_version "${yosys_version}")

execute_process(
    COMMAND "${NEXTPNR}" --version
    OUTPUT_VARIABLE nextpnr_version
    ERROR_VARIABLE nextpnr_error
    RESULT_VARIABLE nextpnr_status)
if(NOT nextpnr_status EQUAL 0)
    set(nextpnr_version "unknown")
endif()
set(nextpnr_version "${nextpnr_version}${nextpnr_error}")
string(STRIP "${nextpnr_version}" nextpnr_version)
string(REGEX REPLACE "[\r\n].*" "" nextpnr_version "${nextpnr_version}")

set(report "")
set(report "${report}# Synthesis\n\n")
set(report "${report}Target: Lattice ECP5, CABGA381. The framer and decoder use an\n")
set(report "${report}LFE5U-25F; the hardware book uses an LFE5U-85F because its state is\n")
set(report "${report}about 2 Mbit. Out of context: no pin plan, no bitstream.\n\n")
set(report "${report}Target frequency: ${FREQ} MHz.\n\n")
set(report "${report}Tools: ${ghdl_version}; ${yosys_version}; ${nextpnr_version}.\n\n")
set(report "${report}| Design | Device | LUT4 | FF | BRAM | Target | Fmax | Notes |\n")
set(report "${report}|---|---|---:|---:|---:|---:|---:|---|\n")

set(book_synthesised FALSE)

foreach(top IN LISTS TOP)
    if(top STREQUAL "hft_book")
        set(book_synthesised TRUE)
    endif()

    if(top STREQUAL "hft_book_hw")
        set(device_name "LFE5U-85F")
        set(device_part "85k")
    else()
        set(device_name "LFE5U-25F")
        set(device_part "25k")
    endif()

    # 1. GHDL to Verilog.
    execute_process(
        COMMAND "${GHDL}" --synth --std=08 "--workdir=${WORK}" --out=verilog ${top}
        OUTPUT_FILE "${WORK}/${top}.v"
        ERROR_VARIABLE error_text
        RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        if(NOT top STREQUAL "hft_book")
            message(FATAL_ERROR "ghdl --synth failed on ${top}: ${error_text}")
        endif()
        set(note "synthesis failed")
        if(error_text MATCHES "limits of range are not constant")
            set(note "behavioural model: bitmap scan has non-constant loop bounds")
        elseif(NOT error_text STREQUAL "")
            string(REGEX REPLACE "[\r\n]+" " " note "${error_text}")
            string(REPLACE "|" "/" note "${note}")
            string(SUBSTRING "${note}" 0 90 note)
        endif()
        set(report "${report}| ${top} | - | - | - | ${FREQ} MHz | - | ${note} |\n")
        continue()
    endif()

    # 2. Yosys to the target's primitives.
    execute_process(
        COMMAND "${YOSYS}" -p
                "read_verilog -sv ${top}.v; synth_ecp5 -top ${top} -json ${top}.json"
        WORKING_DIRECTORY "${WORK}"
        OUTPUT_VARIABLE yosys_output
        ERROR_VARIABLE error_text
        RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        # If GHDL got as far as Verilog for the book, its process-
        # variable pools expand to about 122 MB and the scanner's input
        # buffer cannot hold it. Either way the memory never becomes a
        # RAM; this is the behavioural-model result, not an area figure.
        set(yosys_text "${yosys_output}${error_text}")
        if(NOT top STREQUAL "hft_book")
            message(FATAL_ERROR "yosys failed on ${top}: ${yosys_text}")
        endif()
        set(note "synthesis failed")
        if(yosys_text MATCHES "input buffer overflow")
            set(note "behavioural memory: the pools expand to 122 MB of Verilog")
        elseif(NOT yosys_text STREQUAL "")
            string(REGEX REPLACE "[\r\n]+" " " note "${yosys_text}")
            string(REPLACE "|" "/" note "${note}")
            string(SUBSTRING "${note}" 0 90 note)
        endif()
        set(report "${report}| ${top} | - | - | - | ${FREQ} MHz | - | ${note} |\n")
        continue()
    endif()

    set(lut4 "-")
    set(ff "-")
    set(bram "-")
    if(yosys_output MATCHES "([0-9]+)[ \t]+LUT4")
        set(lut4 "${CMAKE_MATCH_1}")
    endif()
    if(yosys_output MATCHES "([0-9]+)[ \t]+TRELLIS_FF")
        set(ff "${CMAKE_MATCH_1}")
    endif()
    if(yosys_output MATCHES "([0-9]+)[ \t]+DP16KD")
        set(bram "${CMAKE_MATCH_1}")
    endif()

    # 3. Place and route. Out of context: these blocks have wide
    # parallel interfaces that are a modelling convenience rather than a
    # pin plan, and a design that cannot be pinned is not a design that
    # failed to meet a timing target.
    execute_process(
        COMMAND "${NEXTPNR}" --json "${top}.json"
                --write "${top}_pnr.json"
                --report "${top}_pnr_report.json"
                --25k --package CABGA381 --freq ${FREQ} --seed 1
                --out-of-context --timing-allow-fail
        WORKING_DIRECTORY "${WORK}"
        OUTPUT_VARIABLE pnr_stdout
        ERROR_VARIABLE pnr_stderr
        RESULT_VARIABLE status)

    # nextpnr writes its log to STDERR, so both streams have to be
    # searched for the timing line. The last line is the post-route
    # figure; the first is an estimate from placement.
    set(pnr_output "${pnr_stdout}${pnr_stderr}")

    set(fmax "")
    string(REGEX MATCHALL "Max frequency for clock 'clk': [0-9.]+ MHz" fmax_lines "${pnr_output}")
    if(fmax_lines)
        list(GET fmax_lines -1 fmax_line)
        string(REGEX REPLACE ".*: ([0-9.]+) MHz" "\\1" fmax "${fmax_line}")
    endif()

    # Device utilisation, not Yosys cell counts: one LUT4 cell can become
    # several packaged LUT4s once L6MUX21 and PFUMX are packed with it.
    if(pnr_output MATCHES "Total LUT4s:[ \t]+([0-9]+)/")
        set(lut4 "${CMAKE_MATCH_1}")
    endif()
    if(pnr_output MATCHES "TRELLIS_FF:[ \t]+([0-9]+)/")
        set(ff "${CMAKE_MATCH_1}")
    endif()
    if(pnr_output MATCHES "DP16KD:[ \t]+([0-9]+)/")
        set(bram "${CMAKE_MATCH_1}")
    endif()

    if(NOT status EQUAL 0)
        message(FATAL_ERROR "nextpnr failed on ${top}: ${pnr_stderr}")
    endif()

    set(notes "ok")
    if(fmax STREQUAL "")
        set(notes "no timing figure; see the nextpnr log")
    elseif(fmax LESS FREQ)
        set(notes "target ${FREQ} MHz not met; achieved figure reported")
    else()
        set(notes "met ${FREQ} MHz target")
    endif()

    if(fmax STREQUAL "")
        set(fmax "-")
    else()
        set(fmax "${fmax} MHz")
    endif()

    set(report "${report}| ${top} | ${lut4} | ${ff} | ${bram} | ${FREQ} MHz | ${fmax} | ${notes} |\n")
endforeach()

if(NOT book_synthesised)
    set(report "${report}| hft_book | - | - | - | ${FREQ} MHz | - | behavioural model; not synthesised (see rtl/README.md) |\n")
endif()

file(WRITE "${REPORT}" "${report}")
message(STATUS "${report}")
