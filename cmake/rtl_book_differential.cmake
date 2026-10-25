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
# The chain is:
#
#   1. The capture is generated, once, and written out twice: as the
#      binary the C++ side reads and as the hex sidecar the RTL side
#      reads. Both are the same bytes.
#   2. The C++ sparse book replays it and dumps the result. That dump is
#      the reference for every other run.
#   3. The RTL is analysed. GHDL will not re-analyse a changed file on
#      its own, so this is not optional.
#   4. The RTL book runs with the dense grid disabled. This must still
#      match the sparse reference exactly: the dense feature must not
#      change the default path.
#   5. The C++ and RTL books both run with a full-band dense grid, and
#      both must match the sparse reference.
#   6. Both run with a deliberately narrow band, so some prices use the
#      dense grid and some use the sparse fallback. The merged dump must
#      still be in priority order and still match.
#   7. The dense and fallback cycle counts must be lower than the sparse
#      count. This is the reason the grid exists; a correctness-only
#      check would pass for a grid that never gets used.
#
# Invoked as:
#   cmake -DHFT_RTL_TRACE=... -DHFT_BOOK_DUMP=... -DHFT_BOOK_CHECK=...
#         -DGHDL=... -DRTL_DIR=... -DWORK=... -DRECORDS=...
#         -DDENSE_FLOOR=... -DDENSE_SLOTS=...
#         -DFALLBACK_FLOOR=... -DFALLBACK_SLOTS=...
#         -P rtl_book_differential.cmake

foreach(required HFT_RTL_TRACE HFT_BOOK_DUMP HFT_BOOK_CHECK GHDL RTL_DIR WORK RECORDS)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} was not passed to the script")
    endif()
endforeach()

if(NOT DEFINED DENSE_FLOOR)
    set(DENSE_FLOOR 990000)
endif()
if(NOT DEFINED DENSE_SLOTS)
    set(DENSE_SLOTS 32768)
endif()
if(NOT DEFINED FALLBACK_FLOOR)
    set(FALLBACK_FLOOR 998000)
endif()
if(NOT DEFINED FALLBACK_SLOTS)
    set(FALLBACK_SLOTS 2048)
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(MAKE_DIRECTORY "${WORK}/ghdl")

# GHDL is fussy about backslashes in a generic's value, and CMake on
# Windows produces them. Forward slashes work everywhere.
string(REPLACE "\\" "/" hex_path "${WORK}/capture.hex")
string(REPLACE "\\" "/" rtl_sparse_path "${WORK}/rtl_sparse.dump")
string(REPLACE "\\" "/" rtl_dense_path "${WORK}/rtl_dense.dump")
string(REPLACE "\\" "/" rtl_fallback_path "${WORK}/rtl_fallback.dump")

function(check_books label left right)
    execute_process(
        COMMAND "${HFT_BOOK_CHECK}" "${left}" "${right}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output_text
        ERROR_VARIABLE error_text)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR
            "THE BOOKS DISAGREE (${label}).\n${output_text}${error_text}")
    endif()
    message(STATUS "  ${label} ...... the books agree on every order, level and field")
endfunction()

function(read_cycles label output_text)
    string(REGEX MATCH "rtl book cycles[ ]+([0-9]+)" _ "${output_text}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "could not read the cycle count from ${label}")
    endif()
    set(${label}_cycles "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

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

# ---- 2. The C++ sparse reference --------------------------------------

execute_process(
    COMMAND "${HFT_BOOK_DUMP}" "${WORK}/capture.itch" --out "${WORK}/cpp.dump"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_book_dump failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  c++ sparse ........ ${output_text}")

# ---- 3. Analyse the RTL ------------------------------------------------

set(rtl_sources
    "${RTL_DIR}/hft_itch_pkg.vhd"
    "${RTL_DIR}/hft_trace_fmt.vhd"
    "${RTL_DIR}/hft_book_pkg.vhd"
    "${RTL_DIR}/hft_hex_reader.vhd"
    "${RTL_DIR}/hft_moldudp64.vhd"
    "${RTL_DIR}/hft_itch_decode.vhd"
    "${RTL_DIR}/hft_book.vhd"
    "${RTL_DIR}/hft_book_hw.vhd"
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

# ---- 4. The sparse RTL book -------------------------------------------

execute_process(
    COMMAND "${GHDL}" -r --std=08 "--workdir=${WORK}/ghdl" tb_book
            "-gHEX_PATH=${hex_path}" "-gOUT_PATH=${rtl_sparse_path}"
            -gDENSE=false
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the sparse RTL simulation failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  rtl sparse ........ ${output_text}")
read_cycles(sparse "${output_text}")
check_books("sparse differential" "${WORK}/cpp.dump" "${rtl_sparse_path}")

# ---- 5. The dense grid over the whole price range ----------------------

execute_process(
    COMMAND "${HFT_BOOK_DUMP}" "${WORK}/capture.itch" --out "${WORK}/cpp_dense.dump"
            --dense --floor "${DENSE_FLOOR}" --tick 1
            --ticks-per-side "${DENSE_SLOTS}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_book_dump --dense failed (${status}): ${output_text}${error_text}")
endif()
check_books("c++ dense differential" "${WORK}/cpp.dump" "${WORK}/cpp_dense.dump")

execute_process(
    COMMAND "${GHDL}" -r --std=08 "--workdir=${WORK}/ghdl" tb_book
            "-gHEX_PATH=${hex_path}" "-gOUT_PATH=${rtl_dense_path}"
            -gDENSE=true -gPRICE_FLOOR=${DENSE_FLOOR} -gDENSE_SLOTS=${DENSE_SLOTS}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the dense RTL simulation failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  rtl dense ......... ${output_text}")
read_cycles(dense "${output_text}")
check_books("dense differential" "${WORK}/cpp.dump" "${rtl_dense_path}")

# ---- 6. The dense grid with the sparse fallback ------------------------

execute_process(
    COMMAND "${HFT_BOOK_DUMP}" "${WORK}/capture.itch" --out "${WORK}/cpp_fallback.dump"
            --dense --floor "${FALLBACK_FLOOR}" --tick 1
            --ticks-per-side "${FALLBACK_SLOTS}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "hft_book_dump --dense fallback failed (${status}): ${output_text}${error_text}")
endif()
check_books("c++ fallback differential" "${WORK}/cpp.dump" "${WORK}/cpp_fallback.dump")

execute_process(
    COMMAND "${GHDL}" -r --std=08 "--workdir=${WORK}/ghdl" tb_book
            "-gHEX_PATH=${hex_path}" "-gOUT_PATH=${rtl_fallback_path}"
            -gDENSE=true -gPRICE_FLOOR=${FALLBACK_FLOOR} -gDENSE_SLOTS=${FALLBACK_SLOTS}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output_text
    ERROR_VARIABLE error_text)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the fallback RTL simulation failed (${status}): ${output_text}${error_text}")
endif()
message(STATUS "  rtl fallback ...... ${output_text}")
read_cycles(fallback "${output_text}")
check_books("fallback differential" "${WORK}/cpp.dump" "${rtl_fallback_path}")

# ---- 7. The grid has to be faster, not merely correct ------------------

if(NOT dense_cycles LESS sparse_cycles)
    message(FATAL_ERROR
        "the dense book took ${dense_cycles} cycles, the sparse book ${sparse_cycles}")
endif()
if(NOT fallback_cycles LESS sparse_cycles)
    message(FATAL_ERROR
        "the fallback book took ${fallback_cycles} cycles, the sparse book ${sparse_cycles}")
endif()
message(STATUS "  cycles ............ sparse ${sparse_cycles}, dense ${dense_cycles}, fallback ${fallback_cycles}")
