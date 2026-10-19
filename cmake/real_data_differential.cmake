# The real-data differential: the C++ engine and the RTL model, over
# REAL Nasdaq market data.
#
# Everything else in this repository's differential suite runs over
# GENERATED captures. That is a limitation with a name, and the name is
# that a generator and a decoder which share a mistake will agree with
# each other and both be wrong.
#
# The Order Executed field table is the proof. It carried a Printable
# byte that the message does not have, and the message was 31 bytes
# where the table said 32. Every test in the repository agreed with
# the table, because the generator wrote the same bytes the decoder
# read. The first real message from the exchange disagreed with both,
# and 147,395 of them were being dropped as bad lengths.
#
# This is that check, as a test.
#
# The data is fetched, not committed: the exchange's files are several
# gigabytes and are the exchange's property. The fetch is skipped when
# the network is not there, and the test says so rather than failing:
# a missing network is not a wrong answer.
#
# The chain is:
#
#   1. Fetch a slice of the real archive.
#   2. Frame it into a MoldUDP64 capture, starting at the first Add
#      Order so that the capture is trading rather than directory.
#   3. INSPECT it: every message type this build decodes must have the
#      length the field table says. This is the check that finds a
#      table that the wire disagrees with.
#   4. DECODE it on both sides and compare, field by field.
#   5. APPLY it to both books and compare, record by record.
#
# Invoked as:
#   cmake -DHFT_RTL_TRACE=... -DHFT_BOOK_DUMP=... -DHFT_BOOK_CHECK=...
#         -DHFT_RTL_CHECK=... -DHFT_CAPTURE_INSPECT=... -DHFT_ITCH_CONVERT=...
#         -DGHDL=... -DRTL_DIR=... -DWORK=... -DRAW=...
#         -DFETCH=<script> -P real_data_differential.cmake

foreach(required HFT_RTL_TRACE HFT_RTL_CHECK HFT_BOOK_DUMP HFT_BOOK_CHECK HFT_CAPTURE_INSPECT
                HFT_ITCH_CONVERT GHDL RTL_DIR WORK RAW FETCH)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} was not passed to the script")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(MAKE_DIRECTORY "${WORK}/ghdl")

string(REPLACE "\\" "/" hex_path "${WORK}/capture.hex")
string(REPLACE "\\" "/" rtl_trace_path "${WORK}/rtl.trace")
string(REPLACE "\\" "/" rtl_dump_path "${WORK}/rtl.dump")

# ---- 1. The real data ------------------------------------------------

execute_process(
    COMMAND "${FETCH}" "${WORK}/raw.itch"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(STATUS "SKIPPED: the real market data could not be fetched.")
    message(STATUS "  ${output_text}${error_text}")
    message(STATUS "  The fetch needs the network. Everything else in this suite")
    message(STATUS "  runs over generated captures and does not.")
    return()
endif()
message(STATUS "  real data ........ ${output_text}")

# ---- 2. Frame it into a capture ---------------------------------------
#
# Starting at the first Add Order: a real session opens with megabytes
# of directory and system messages, and a capture of only those
# exercises nothing.

execute_process(
    COMMAND "${HFT_ITCH_CONVERT}" "${WORK}/raw.itch" "${WORK}/capture.itch"
            --skip-to A --max-bytes 2097152
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_itch_convert failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  capture .......... ${output_text}")

# ---- 3. The field tables against the wire -----------------------------

execute_process(
    COMMAND "${HFT_CAPTURE_INSPECT}" "${WORK}/capture.itch"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR
        "THE FIELD TABLES DISAGREE WITH THE REAL WIRE.\n${output_text}${error_text}")
endif()
message(STATUS "  field tables ..... every decoded type has the length the table says")

# ---- 4. Decode, on both sides ----------------------------------------

execute_process(
    COMMAND "${HFT_RTL_TRACE}" "${WORK}/capture.itch" --out "${WORK}/cpp.trace"
            --hex "${WORK}/capture.hex"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_rtl_trace failed (${status}): ${output_text}${error_text}")
endif()

set(rtl_sources
    "${RTL_DIR}/hft_itch_pkg.vhd"
    "${RTL_DIR}/hft_trace_fmt.vhd"
    "${RTL_DIR}/hft_book_pkg.vhd"
    "${RTL_DIR}/hft_hex_reader.vhd"
    "${RTL_DIR}/hft_moldudp64.vhd"
    "${RTL_DIR}/hft_itch_decode.vhd"
    "${RTL_DIR}/hft_book.vhd"
    "${RTL_DIR}/tb/tb_trace.vhd"
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

execute_process(
    COMMAND "${GHDL}" -r --std=08 "--workdir=${WORK}/ghdl" tb_trace
            "-gHEX_PATH=${hex_path}" "-gOUT_PATH=${rtl_trace_path}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the RTL decode failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  rtl decode ....... ${output_text}")

execute_process(
    COMMAND "${HFT_RTL_CHECK}" "${WORK}/cpp.trace" "${WORK}/rtl.trace"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR
        "THE RTL AND THE C++ DECODER DISAGREE ON REAL DATA.\n${output_text}${error_text}")
endif()
message(STATUS "  differential ...... the two decoders agree on every field of every real message")

# ---- 5. The books, on both sides --------------------------------------
#
# A smaller slice for the books: the RTL book's price walk is
# O(depth), and a real book is thousands of levels deep, so the book
# differential costs more per message than the decode one.

execute_process(
    COMMAND "${HFT_ITCH_CONVERT}" "${WORK}/raw.itch" "${WORK}/book_capture.itch"
            --skip-to A --max-bytes 131072
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_itch_convert failed (${status}): ${output_text}${error_text}")
endif()

execute_process(
    COMMAND "${HFT_BOOK_DUMP}" "${WORK}/book_capture.itch" --out "${WORK}/cpp.dump"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_book_dump failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  c++ book .......... ${output_text}")

execute_process(
    COMMAND "${HFT_RTL_TRACE}" "${WORK}/book_capture.itch" --out "${WORK}/book_cpp.trace"
            --hex "${WORK}/book.hex"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_rtl_trace failed (${status}): ${output_text}${error_text}")
endif()

execute_process(
    COMMAND "${GHDL}" -r --std=08 "--workdir=${WORK}/ghdl" tb_book
            "-gHEX_PATH=${WORK}/book.hex" "-gOUT_PATH=${rtl_dump_path}"
            "-gMAX_MSGS=16384" "-gORDERS=32768" "-gLEVELS=8192" "-gSLOTS=65536"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the RTL book failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  rtl book .......... ${output_text}")

execute_process(
    COMMAND "${HFT_BOOK_CHECK}" "${WORK}/cpp.dump" "${WORK}/rtl.dump"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR
        "THE RTL AND THE C++ BOOK DISAGREE ON REAL DATA.\n${output_text}${error_text}")
endif()
message(STATUS "  differential ...... the two books agree on every order, level and field")

