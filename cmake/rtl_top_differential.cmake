# The streaming top-level differential: byte stream -> hft_top ->
# book dump, compared against the C++ sparse reference.
#
# Unlike rtl_book_differential (decode-then-apply array), this runs
# the pipeline in lockstep with backpressure: bytes are consumed
# only when in_ready='1', so the FIFO, the framer stall and the
# book issue logic are exercised for real. It also runs the
# sequence-tracker unit test (tb_seq) and checks the in-hardware
# FNV checksum against the testbench's fold.
#
# Invoked as:
#   cmake -DHFT_RTL_TRACE=... -DHFT_BOOK_DUMP=... -DHFT_BOOK_CHECK=...
#         -DGHDL=... -DRTL_DIR=... -DWORK=... -DRECORDS=...
#         -P rtl_top_differential.cmake

foreach(required HFT_RTL_TRACE HFT_BOOK_DUMP HFT_BOOK_CHECK GHDL RTL_DIR WORK RECORDS)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} was not passed to the script")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(MAKE_DIRECTORY "${WORK}/ghdl")

string(REPLACE "\\" "/" hex_path "${WORK}/capture.hex")
string(REPLACE "\\" "/" rtl_top_path "${WORK}/rtl_top.dump")

execute_process(
    COMMAND "${HFT_RTL_TRACE}" --capture "${WORK}/capture.itch" --hex "${WORK}/capture.hex"
            --out "${WORK}/trace.txt" --records "${RECORDS}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_rtl_trace failed (${status}): ${output_text}${error_text}")
endif()

execute_process(
    COMMAND "${HFT_BOOK_DUMP}" "${WORK}/capture.itch" --out "${WORK}/cpp.dump"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_book_dump failed (${status}): ${output_text}${error_text}")
endif()

set(rtl_sources
    "${RTL_DIR}/hft_itch_pkg.vhd"
    "${RTL_DIR}/hft_trace_fmt.vhd"
    "${RTL_DIR}/hft_book_pkg.vhd"
    "${RTL_DIR}/hft_hex_reader.vhd"
    "${RTL_DIR}/hft_moldudp64.vhd"
    "${RTL_DIR}/hft_itch_decode.vhd"
    "${RTL_DIR}/hft_book.vhd"
    "${RTL_DIR}/hft_book_hw.vhd"
    "${RTL_DIR}/hft_seq.vhd"
    "${RTL_DIR}/hft_msg_fifo.vhd"
    "${RTL_DIR}/hft_top.vhd"
    "${RTL_DIR}/tb/tb_seq.vhd"
    "${RTL_DIR}/tb/tb_top.vhd")

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

# Sequence tracker unit test: ok, gap, duplicate, heartbeat, resync, wrap.
execute_process(
    COMMAND "${GHDL}" -r --std=08 "--workdir=${WORK}/ghdl" tb_seq
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "tb_seq failed (${status}): ${output_text}${error_text}")
endif()
if(NOT output_text MATCHES "tb_seq PASS")
    message(FATAL_ERROR "tb_seq did not report PASS:\n${output_text}")
endif()
message(STATUS "  seq tracker ....... ok, gap, duplicate, heartbeat, resync, wrap")

# Streaming top over the same capture, with backpressure for real.
execute_process(
    COMMAND "${GHDL}" -r --std=08 "--workdir=${WORK}/ghdl" tb_top
            "-gHEX_PATH=${hex_path}" "-gOUT_PATH=${rtl_top_path}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the streaming RTL simulation failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  rtl streaming ..... ${output_text}")
if(NOT output_text MATCHES "rtl top seq missing[ ]+0")
    message(FATAL_ERROR "streaming sequence tracker reported a gap on a contiguous capture:\n${output_text}")
endif()
if(NOT output_text MATCHES "hw checksum ok")
    message(FATAL_ERROR "in-hardware checksum did not match the testbench fold:\n${output_text}")
endif()

execute_process(
    COMMAND "${HFT_BOOK_CHECK}" "${WORK}/cpp.dump" "${rtl_top_path}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "THE STREAMING BOOK DISAGREES WITH C++.\n${output_text}${error_text}")
endif()
message(STATUS "  streaming differential ...... the books agree on every order, level and field")
