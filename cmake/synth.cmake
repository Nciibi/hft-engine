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
# The order book is expected to FAIL, and it does: its pools are
# process variables, so the synthesis expands them into 122 MB of
# Verilog and the memory never becomes a RAM. That is measured and
# reported rather than hidden, because "this part of the model is not
# hardware yet" is a thing a reader needs to know.
#
# Invoked as:
#   cmake -DGHDL=... -DYOSYS=... -DNEXTPNR=... -DRTL_DIR=... -DWORK=...
#         -DREPORT=... -DTOP=... -P synth.cmake

foreach(required GHDL YOSYS NEXTPNR RTL_DIR WORK REPORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} was not passed to the script")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# The designs to synthesise. The framer and the decoder are the ones
# with a realisable boundary; the book is here to be measured.
if(NOT DEFINED TOP)
    set(TOP hft_moldudp64 hft_itch_decode hft_book)
endif()

# The sources, in dependency order: the packages, then the entities.
set(sources
    "${RTL_DIR}/hft_itch_pkg.vhd"
    "${RTL_DIR}/hft_trace_fmt.vhd"
    "${RTL_DIR}/hft_book_pkg.vhd"
    "${RTL_DIR}/hft_hex_reader.vhd"
    "${RTL_DIR}/hft_moldudp64.vhd"
    "${RTL_DIR}/hft_itch_decode.vhd"
    "${RTL_DIR}/hft_book.vhd")

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

set(report "")
set(report "${report}# Synthesis\n\n")
set(report "${report}Target: Lattice ECP5, LFE5U-25F, CABGA381.\n\n")
set(report "${report}| Design | LUT4 | FF | BRAM | Fmax | Notes |\n")
set(report "${report}|---|---|---|---|---|---|\n")

foreach(top IN LISTS TOP)
    # 1. GHDL to Verilog.
    execute_process(
        COMMAND "${GHDL}" --synth --std=08 "--workdir=${WORK}" --out=verilog ${top}
        OUTPUT_FILE "${WORK}/${top}.v"
        ERROR_VARIABLE error_text
        RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        set(report "${report}| ${top} | — | — | — | — | synthesis failed |\n")
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
        # The book's Verilog is 122 MB and the scanner's input buffer
        # cannot hold it. That IS the measurement: the pools are
        # process variables, so the synthesis expands them into flip-
        # flops and the memory never becomes a RAM.
        set(note "behavioural memory: the pools do not become a RAM")
        if(error_text MATCHES "input buffer overflow")
            set(note "behavioural memory: the pools expand to 122 MB of Verilog")
        endif()
        set(report "${report}| ${top} | — | — | — | — | ${note} |\n")
        continue()
    endif()

    set(lut4 "—")
    set(ff "—")
    set(bram "—")
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
        COMMAND "${NEXTPNR}" --json "${top}.json" --textcfg "${top}.config"
                --25k --package CABGA381 --freq 200 --seed 1 --out-of-context
        WORKING_DIRECTORY "${WORK}"
        OUTPUT_VARIABLE pnr_stdout
        ERROR_VARIABLE pnr_stderr
        RESULT_VARIABLE status)

    # nextpnr writes its log to STDERR, so both streams have to be
    # searched for the timing line.
    set(pnr_output "${pnr_stdout}${pnr_stderr}")

    set(fmax "—")
    if(pnr_output MATCHES "Max frequency for clock 'clk': ([0-9.]+) MHz")
        set(fmax "${CMAKE_MATCH_1}")
    endif()

    set(notes "ok")
    if(NOT status EQUAL 0)
        set(notes "timing target not met; the achieved figure is reported")
    endif()

    set(report "${report}| ${top} | ${lut4} | ${ff} | ${bram} | ${fmax} MHz | ${notes} |\n")
endforeach()

file(WRITE "${REPORT}" "${report}")
message(STATUS "${report}")
