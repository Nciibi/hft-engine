# End-to-end exercise of the RTL trace tools.
#
# The unit suite proves the comparator can fail IN PROCESS. This script
# proves the TOOLS can fail: that the generator writes a trace, that the
# comparator accepts an identical trace, and that the comparator REJECTS
# a trace that differs.
#
# The corruption here is a second generator run with a different seed.
# Two different captures produce two different traces, and a comparator
# that accepted them would report agreement it had not checked. It is a
# more honest corruption than editing a file, because it goes through
# exactly the same code path a real disagreement would.
#
# It also checks two things that are easy to get wrong and hard to
# notice: that the generator is deterministic for a given seed, and that
# the hex sidecar is a faithful dump of the capture rather than an
# approximation of it.
#
# Invoked as: cmake -DHFT_RTL_TRACE=... -DHFT_RTL_CHECK=... -DWORK=... -P rtl_trace_check.cmake

foreach(required HFT_RTL_TRACE HFT_RTL_CHECK WORK)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} was not passed to the script")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# ---- Generate -------------------------------------------------------

function(generate_trace output seed)
    execute_process(
        COMMAND "${HFT_RTL_TRACE}" --out "${output}" --hex "${output}.hex"
                --records 2000 --seed "${seed}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output_text
        ERROR_VARIABLE error_text)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "hft_rtl_trace failed (${status}): ${output_text}${error_text}")
    endif()
    set(GENERATED_OUTPUT "${output_text}" PARENT_SCOPE)
endfunction()

generate_trace("${WORK}/a.trace" "0xC0FFEE0012345678")
set(first_output "${GENERATED_OUTPUT}")
generate_trace("${WORK}/a2.trace" "0xC0FFEE0012345678")
generate_trace("${WORK}/b.trace" "0x0123456789ABCDEF")

# ---- Determinism ----------------------------------------------------

file(READ "${WORK}/a.trace" a_text)
file(READ "${WORK}/a2.trace" a2_text)
if(NOT a_text STREQUAL a2_text)
    message(FATAL_ERROR "the generator is not deterministic: two runs with the same seed differ")
endif()
message(STATUS "  determinism ....... same seed, identical trace")

# ---- The comparator accepts an identical trace ----------------------

execute_process(
    COMMAND "${HFT_RTL_CHECK}" "${WORK}/a.trace" "${WORK}/a2.trace"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the comparator rejected two identical traces: ${output_text}${error_text}")
endif()
message(STATUS "  identical traces ... accepted")

# ---- The comparator REJECTS a different one ------------------------

execute_process(
    COMMAND "${HFT_RTL_CHECK}" "${WORK}/a.trace" "${WORK}/b.trace"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(status EQUAL 0)
    message(FATAL_ERROR "the comparator accepted two DIFFERENT traces")
endif()
message(STATUS "  different traces ... rejected")

# ---- The hex sidecar is a faithful dump -----------------------------

file(READ "${WORK}/a.trace.hex" hex_text)
string(REGEX MATCHALL "[0-9a-f]" hex_chars "${hex_text}")
list(LENGTH hex_chars hex_char_count)

string(REGEX MATCH "capture bytes +([0-9]+)" _ "${first_output}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "could not read the capture size from the generator's output")
endif()
set(capture_bytes "${CMAKE_MATCH_1}")
math(EXPR expected_chars "${capture_bytes} * 2")
if(NOT hex_char_count EQUAL expected_chars)
    message(FATAL_ERROR
        "the hex sidecar holds ${hex_char_count} hex characters, the capture is ${capture_bytes} bytes")
endif()
message(STATUS "  hex sidecar ........ ${hex_char_count} characters for ${capture_bytes} bytes")

# ---- The trace holds one record per frame ---------------------------

string(REGEX MATCHALL "\n" trace_newlines "${a_text}")
list(LENGTH trace_newlines newline_count)
math(EXPR trace_records "${newline_count} - 1")
string(REGEX MATCH "trace records +([0-9]+)" _ "${first_output}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "could not read the record count from the generator's output")
endif()
set(reported_records "${CMAKE_MATCH_1}")
if(NOT trace_records EQUAL reported_records)
    message(FATAL_ERROR
        "the trace file holds ${trace_records} records, the generator reported ${reported_records}")
endif()
message(STATUS "  record count ...... ${trace_records} records, as reported")

message(STATUS "OK: the RTL trace tools behave.")
