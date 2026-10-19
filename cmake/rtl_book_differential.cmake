# The order-book differential: the C++ book and the RTL book, over the
# same capture, compared field by field.
#
# This is the same shape as the decode differential, one layer up. The
# decode path proved that two implementations agree on what the WIRE
# says; this proves that two implementations agree on what the BOOK
# does with it -- the price-time priority, the queue order within a
# price, the level aggregates, and the order count at every level.
#
# The comparison is a DUMP and not a checksum. A checksum says "these
# two states are equal"; a dump says "these two states are equal, and
# if they are not, here is the order and the field that differ". The
# difference matters when the answer is no -- and this repository has
# shipped a wrong answer before, in a field of one message, that a
# checksum would have reported as "these differ" and nothing else.
#
# The chain is five steps and they have to happen in this order:
#
#   1. The capture is generated, once, and written out twice: as the
#      binary the C++ side reads and as the hex sidecar the RTL side
#      reads. Both are the same bytes.
#   2. The C++ side replays it and dumps the book.
#   3. The RTL is analysed. GHDL will not re-analyse a changed file on
#      its own, so this is not optional.
#   4. The RTL model decodes the same capture, applies every message to
#      its own book, and dumps that.
#   5. The two dumps are compared.
#
# Invoked as:
#   cmake -DHFT_RTL_TRACE=... -DHFT_BOOK_DUMP=... -DHFT_BOOK_CHECK=...
#         -DGHDL=... -DRTL_DIR=... -DWORK=... -DRECORDS=... -P rtl_book_differential.cmake

foreach(required HFT_RTL_TRACE HFT_BOOK_DUMP HFT_BOOK_CHECK GHDL RTL_DIR WORK RECORDS)
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
string(REPLACE "\\" "/" rtl_dump_path "${WORK}/rtl.dump")

# ---- 1. The capture, in both forms ------------------------------------

execute_process(
    COMMAND "${HFT_RTL_TRACE}" --capture "${WORK}/capture.itch" --hex "${WORK}/capture.hex"
            --out "${WORK}/trace.txt" --records "${RECORDS}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_rtl_trace failed (${status}): ${output_text}${error_text}")
endif()

# ---- 2. The C++ side's book -------------------------------------------

execute_process(
    COMMAND "${HFT_BOOK_DUMP}" "${WORK}/capture.itch" --out "${WORK}/cpp.dump"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_book_dump failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  c++ book .......... ${output_text}")

# ---- 3. Analyse the RTL -----------------------------------------------

set(rtl_sources
    "${RTL_DIR}/hft_itch_pkg.vhd"
    "${RTL_DIR}/hft_trace_fmt.vhd"
    "${RTL_DIR}/hft_book_pkg.vhd"
    "${RTL_DIR}/hft_hex_reader.vhd"
    "${RTL_DIR}/hft_moldudp64.vhd"
    "${RTL_DIR}/hft_itch_decode.vhd"
    "${RTL_DIR}/hft_book.vhd"
    "${RTL_DIR}/tb/tb_book.vhd")

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

# ---- 4. The RTL side's book --------------------------------------------

execute_process(
    COMMAND "${GHDL}" -r --std=08 "--workdir=${WORK}/ghdl" tb_book
            "-gHEX_PATH=${hex_path}" "-gOUT_PATH=${rtl_dump_path}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the RTL simulation failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  rtl book .......... ${output_text}")

# ---- 5. Compare ---------------------------------------------------------

execute_process(
    COMMAND "${HFT_BOOK_CHECK}" "${WORK}/cpp.dump" "${WORK}/rtl.dump"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR
        "THE RTL AND THE C++ BOOK DISAGREE.\n${output_text}${error_text}")
endif()
message(STATUS "  differential ...... the two books agree on every order, level and field")
