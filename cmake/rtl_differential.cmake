# The differential: the C++ decoder and the RTL model, over the same
# capture, compared field by field.
#
# This is the whole point of the hardware path, and it is the check the
# rest of this repository does not have. Every other check here
# compares the engine against something inside the repository -- the
# generator, a naive reference model, another build of the same code --
# and the Add Order misdecode survived all of them, because the
# generator wrote the same wrong layout the decoder read.
#
# The RTL model is written from the published specification and shares
# no code with the C++ side: its own field tables, its own framing, its
# own decode rules. When the two traces agree, that is two independent
# implementations saying the same thing about the same bytes.
#
# The chain is four steps and they have to happen in this order:
#
#   1. The C++ side writes a capture and its trace of it.
#   2. The RTL is analysed. GHDL will not re-analyse a changed file on
#      its own, so this is not optional.
#   3. The RTL model runs over the same capture and writes its trace.
#   4. The two traces are compared. A difference is a failure, and the
#      report names the record and the field.
#
# Invoked as:
#   cmake -DHFT_RTL_TRACE=... -DHFT_RTL_CHECK=... -DGHDL=... -DRTL_DIR=...
#         -DWORK=... -DRECORDS=... -P rtl_differential.cmake

foreach(required HFT_RTL_TRACE HFT_RTL_CHECK GHDL RTL_DIR WORK RECORDS)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} was not passed to the script")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(MAKE_DIRECTORY "${WORK}/ghdl")

# GHDL is fussy about backslashes in a generic's value, and CMake on
# Windows produces them. Forward slashes work everywhere.
string(REPLACE "\\" "/" hex_path "${WORK}/capture.hex")
string(REPLACE "\\" "/" rtl_trace_path "${WORK}/rtl.trace")
string(REPLACE "\\" "/" work_forward "${WORK}")

# ---- 1. The capture, and the C++ side's trace of it -------------------

execute_process(
    COMMAND "${HFT_RTL_TRACE}" --out "${WORK}/cpp.trace" --hex "${WORK}/capture.hex"
            --records "${RECORDS}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_rtl_trace failed (${status}): ${output_text}${error_text}")
endif()

# ---- 2. Analyse the RTL ----------------------------------------------
#
# In dependency order: the packages, then the entities, then the
# testbench. GHDL binds an entity to the architecture that was current
# when it was analysed, so a stale library produces a baffling
# "architecture is obsoleted by entity" error rather than a useful one.

set(rtl_sources
    "${RTL_DIR}/hft_itch_pkg.vhd"
    "${RTL_DIR}/hft_trace_fmt.vhd"
    "${RTL_DIR}/hft_moldudp64.vhd"
    "${RTL_DIR}/hft_itch_decode.vhd"
    "${RTL_DIR}/tb/tb_trace.vhd")

foreach(src IN LISTS rtl_sources)
    execute_process(
        COMMAND "${GHDL}" -a --std=08 "--workdir=${WORK}/ghdl" "${src}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output_text
        ERROR_VARIABLE error_text)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "ghdl -a failed on ${src} (${status}): ${output_text}${error_text}")
    endif()
endforeach()

# ---- 3. Run the RTL model over the same capture -----------------------

execute_process(
    COMMAND "${GHDL}" -r --std=08 "--workdir=${WORK}/ghdl" tb_trace
            "-gHEX_PATH=${hex_path}" "-gOUT_PATH=${rtl_trace_path}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the RTL simulation failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  rtl simulation .... ${output_text}")

# ---- 4. Compare --------------------------------------------------------

execute_process(
    COMMAND "${HFT_RTL_CHECK}" "${WORK}/cpp.trace" "${WORK}/rtl.trace"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR
        "THE RTL AND THE C++ DECODER DISAGREE.\n${output_text}${error_text}")
endif()
message(STATUS "  differential ...... the two implementations agree on every record and field")
