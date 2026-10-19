cmake_minimum_required(VERSION 3.20)
project(hft_engine
    VERSION 0.1.0
    DESCRIPTION "ITCH 5.0 order book and market making engine"
    LANGUAGES CXX)

# ---- Language level -------------------------------------------------
# C++20 for designated initialisers, <compare>, and concepts. Nothing
# in this project needs C++23, and requiring it would needlessly shrink
# the set of compilers a reviewer can build with.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Warning policy -------------------------------------------------
# Warnings are errors. A latency project that compiles with a wall of
# warnings is a latency project with uninitialised reads in it.
add_library(hft_warnings INTERFACE)
if(MSVC)
    target_compile_options(hft_warnings INTERFACE
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /utf-8
        /EHsc
        # MSan is unavailable on Windows; the Linux profile enables it.
        /fp:precise)
else()
    target_compile_options(hft_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wold-style-cast
        -Wcast-align
        -Wdouble-promotion
        -Wnull-dereference)
endif()

# ---- Release flags --------------------------------------------------
# The fast path is sensitive to these two, and they must be opt-in via
# CMAKE_BUILD_TYPE=Release rather than applied unconditionally: a Debug
# build that silently keeps /O2 is worse than no build at all.
if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG /Oi")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

option(HFT_NATIVE_ARCH "Compile for the host CPU (-march=native)" OFF)
if(HFT_NATIVE_ARCH)
    # Off by default. Benchmark numbers are only comparable when the
    # ISA is pinned, and a benchmark host that is not the deployment
    # host should not quietly get host-specific instructions.
    if(NOT MSVC)
        add_compile_options(-march=native)
    endif()
endif()

# ---- Core library ---------------------------------------------------
# Zero external dependencies. Every target links only hft_core.
add_library(hft_core STATIC
    src/types.cpp
    src/itch/decode.cpp
    src/lob/order_book.cpp
    src/feed/generator.cpp
    src/util/affinity.cpp)

target_include_directories(hft_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(hft_core PUBLIC hft_warnings)

# ---- Benchmarks and tools -------------------------------------------
# Optimised even in a Debug configure: a benchmark built -O0 measures
# the compiler's failure to optimise, not the engine.
add_executable(hft_bench src/bench/bench.cpp)
target_link_libraries(hft_bench PRIVATE hft_core)

# Market maker: quoting model plus adverse-selection measurement over a
# replayed capture.
add_executable(hft_market_maker src/bench/market_maker.cpp)
target_link_libraries(hft_market_maker PRIVATE hft_core)

# SPSC ring against a mutex + condition_variable baseline, on two pinned
# physical cores.
add_executable(hft_ring_bench src/bench/ring_bench.cpp)
target_link_libraries(hft_ring_bench PRIVATE hft_core)

# Per-stage latency at a shallow and a deep book: shallow is 10 price
# levels a side, deep is 1000. Reported separately because book-update
# cost depends on ladder depth -- on the sparse fallback ladder it is
# proportional to it, which is what the dense ladder removes.
add_executable(hft_stage_bench src/bench/stage_bench.cpp)
target_link_libraries(hft_stage_bench PRIVATE hft_core)

# Market data pipeline: decode + apply on one thread versus two threads
# separated by the ring, across a sweep of batch sizes. Fingerprints both
# paths and reports a mismatch rather than a speedup.
add_executable(hft_pipeline_bench src/bench/pipeline_bench.cpp)
target_link_libraries(hft_pipeline_bench PRIVATE hft_core)

# Price ladder scaling: quantifies the O(depth) walk in the sparse level
# list, and the before/after of the dense ladder against it. Reports
# ratios rather than absolute latency, because the ratio survives a
# coarse clock and an absolute figure does not.
add_executable(hft_ladder_bench src/bench/ladder_bench.cpp)
target_link_libraries(hft_ladder_bench PRIVATE hft_core)

# Clock characterisation: what the resolution floor of this host is, and
# therefore which stage figures are measurable at all. Reports no engine
# numbers; it establishes whether the other tools could measure them.
add_executable(hft_tsc_bench src/bench/tsc_bench.cpp)
target_link_libraries(hft_tsc_bench PRIVATE hft_core)

# Replay: deterministic reconstruction of a book from a capture, with a
# state checksum and SOUP sequence validation.
add_executable(hft_replay src/replay/replay.cpp)
target_link_libraries(hft_replay PRIVATE hft_core)

add_executable(hft_test tests/test_units.cpp)
target_link_libraries(hft_test PRIVATE hft_core)

# Pre-trade risk and the OMS. Header-only, so no extra sources.
add_executable(hft_risk_oms tests/test_risk_oms.cpp)
target_link_libraries(hft_risk_oms PRIVATE hft_core)

# The differential test links the reference model, which is
# header-only, so it needs no extra sources.
add_executable(hft_differential tests/differential.cpp)
target_link_libraries(hft_differential PRIVATE hft_core)

# Concurrency: the SPSC ring buffer. Header-only, so no extra sources.
add_executable(hft_concurrent tests/test_concurrent.cpp)
target_link_libraries(hft_concurrent PRIVATE hft_core)

# ---- RTL differential harness ---------------------------------------
# The trace format is the contract between the C++ decoder and the RTL
# model. The format is header-only, so these three targets need no
# extra sources.
#
# hft_rtl_trace   a capture, read frame by frame, written as a trace
# hft_rtl_check   two traces, compared
# hft_rtl_test    the format, the comparator, and the bridge, in process
add_executable(hft_rtl_trace src/rtl/rtl_trace.cpp)
target_link_libraries(hft_rtl_trace PRIVATE hft_core)

add_executable(hft_rtl_check src/rtl/rtl_check.cpp)
target_link_libraries(hft_rtl_check PRIVATE hft_core)

# The book dump and its comparator: the order-book differential, the
# same idea as the trace tools but one layer up.
add_executable(hft_book_dump src/rtl/book_dump.cpp)
target_link_libraries(hft_book_dump PRIVATE hft_core)

add_executable(hft_book_check src/rtl/book_check.cpp)
target_link_libraries(hft_book_check PRIVATE hft_core)

# The capture inspector: what is on the wire, and does the field table
# agree with it? This is the tool that found the Order Executed length
# bug, and it is the check that would have found it the day it was
# written.
add_executable(hft_capture_inspect src/rtl/capture_inspect.cpp)
target_link_libraries(hft_capture_inspect PRIVATE hft_core)

# The raw-ITCH to MoldUDP64 converter: the exchange sends a raw SOUP
# stream, and this repository's readers take the transport envelope.
add_executable(hft_itch_convert src/rtl/itch_convert.cpp)
target_link_libraries(hft_itch_convert PRIVATE hft_core)

add_executable(hft_rtl_test tests/test_rtl_trace.cpp)
target_link_libraries(hft_rtl_test PRIVATE hft_core)

# ---- CTest ----------------------------------------------------------
enable_testing()

add_test(NAME units COMMAND hft_test)

# Risk and OMS. The random invariant test lives inside this binary and
# runs 60,000 randomised operations, so it needs no separate entry.
add_test(NAME risk_oms COMMAND hft_risk_oms)

# Quoting model, adverse-selection metrics, and the book invariant
# (the generated book must never cross). Header-only.
add_executable(hft_strategy tests/test_strategy.cpp)
target_link_libraries(hft_strategy PRIVATE hft_core)
add_test(NAME strategy COMMAND hft_strategy)

# The SPSC ring: single-threaded property checks plus two-threaded
# transfer and payload-integrity checks. The threaded cases are swept
# over several ring depths inside the binary, because a concurrency bug
# that only appears at one depth is one a single depth would miss.
add_test(NAME concurrent COMMAND hft_concurrent)

add_test(NAME differential_default COMMAND hft_differential)

# Several seeds, because a differential test that only passes on one
# seed is a test that has not been run.
foreach(seed
        0xA5A51234DEAD0001
        0x0000000000000001
        0xFFFFFFFFFFFFFFFF
        0x0123456789ABCDEF
        0xDEADBEEFCAFEBABE)
    add_test(NAME differential_${seed} COMMAND hft_differential ${seed} 50000)
endforeach()

# The same differential against the DENSE price ladder.
#
# The dense ladder is a different implementation of level lookup and of
# price ordering -- an array index and an occupancy bitmap where the sparse
# book has a hash map and a linked list -- so the sparse suite says nothing
# about it. This is the only thing that does: 250,000 operations against
# an independent naive model with full state comparison after every one,
# across five seeds.
foreach(seed
        0xA5A51234DEAD0001
        0x0000000000000001
        0xFFFFFFFFFFFFFFFF
        0x0123456789ABCDEF
        0xDEADBEEFCAFEBABE)
    add_test(NAME differential_dense_${seed}
             COMMAND hft_differential ${seed} 50000 dense)
endforeach()
add_test(NAME differential_dense_default COMMAND hft_differential dense)

# Determinism: the same capture replayed twice must produce the same
# book checksum. This is the one test that would catch a change
# introducing iteration-order dependence, pointer-value dependence, or
# uninitialised reads in state, and it is cheap.
add_test(NAME determinism COMMAND hft_replay 50000)
set_tests_properties(determinism PROPERTIES
    PASS_REGULAR_EXPRESSION "sequence gaps +0")

# A capture containing Order Replace records must replay cleanly by
# skipping them. This asserts the skip path end to end, including that
# nothing is silently dropped.
add_test(NAME replay_clean COMMAND hft_replay 100000)
set_tests_properties(replay_clean PROPERTIES
    PASS_REGULAR_EXPRESSION "malformed +0")

# Symbol sharding: symbol routing and the order-reference index that
# makes it possible. Header-only.
add_executable(hft_shards tests/test_shards.cpp)
target_link_libraries(hft_shards PRIVATE hft_core)
add_test(NAME shards COMMAND hft_shards)

# The RTL trace: the format, the comparator, and the bridge from the
# engine's decoder to a trace record.
#
# This suite exists because a comparison that cannot fail is not a
# comparison. Every test in it builds a trace, and several of them build
# a CORRUPTED copy and assert that the comparator notices -- because the
# whole purpose of the harness is to catch a disagreement between two
# implementations, and a comparator that silently reported agreement
# would make every future differential result meaningless.
add_test(NAME rtl_trace COMMAND hft_rtl_test)

# The tools end to end: generate, compare, and -- the part that matters
# -- REJECT a trace that differs.
#
# The unit suite proves the comparator can fail in process. This one
# proves the shipped binaries can, which is a different claim: a tool
# whose exit status was always zero would make every future differential
# result meaningless, and nothing in the unit suite would notice.
add_test(NAME rtl_trace_tools
         COMMAND "${CMAKE_COMMAND}"
                 -DHFT_RTL_TRACE=$<TARGET_FILE:hft_rtl_trace>
                 -DHFT_RTL_CHECK=$<TARGET_FILE:hft_rtl_check>
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/rtl_trace_work
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rtl_trace_check.cmake")

# ---- The differential -------------------------------------------------
#
# The C++ decoder and the RTL model, over the same capture, compared
# field by field.
#
# This is the check the rest of this repository does not have. Every
# other check here compares the engine against something INSIDE the
# repository -- the generator, a naive reference model, another build of
# the same code -- and the Add Order misdecode survived all of them,
# because the generator wrote the same wrong layout the decoder read.
#
# The RTL model is written from the published specification and shares
# no code with the C++ side: its own field tables, its own framing, its
# own decode rules. When the two traces agree, two independent
# implementations are saying the same thing about the same bytes.
#
# GHDL is not a dependency. Where it is absent the test is skipped
# rather than failed, because a missing simulator is a missing
# simulator and not a wrong answer -- the same rule the doc check
# follows. Set HFT_REQUIRE_GHDL=ON to turn the skip into a failure,
# which is what CI does on the job that installs it.
#
# GHDL is looked for on PATH, in the two standard Windows install
# locations, and wherever the GHDL environment variable points. The
# cache variable overrides all of them:
#   cmake -B build -DHFT_GHDL=/usr/bin/ghdl
set(HFT_GHDL "" CACHE FILEPATH "The GHDL executable, for the RTL differential test")
if(HFT_GHDL)
    set(GHDL "${HFT_GHDL}")
else()
    find_program(GHDL ghdl
        HINTS
            "$ENV{GHDL}/bin"
            "C:/ghdl/bin"
            "D:/ghdl/bin"
        DOC "The GHDL simulator, for the RTL differential test")
endif()
if(GHDL)
    add_test(NAME rtl_decode_differential
             COMMAND "${CMAKE_COMMAND}"
                     -DHFT_RTL_TRACE=$<TARGET_FILE:hft_rtl_trace>
                     -DHFT_RTL_CHECK=$<TARGET_FILE:hft_rtl_check>
                     -DGHDL=${GHDL}
                     -DRTL_DIR=${CMAKE_CURRENT_SOURCE_DIR}/rtl
                     -DWORK=${CMAKE_CURRENT_BINARY_DIR}/rtl_diff_work
                     -DRECORDS=2000
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rtl_differential.cmake")
    set_tests_properties(rtl_decode_differential PROPERTIES TIMEOUT 900)

    # The same differential over a capture that stops in the middle of a
    # message. Both implementations have to agree about what a short
    # frame is and about how much of it was missing, and the truncated
    # record is the one case where a decoder that guesses rather than
    # reports would look fine.
    add_test(NAME rtl_decode_differential_truncated
             COMMAND "${CMAKE_COMMAND}"
                     -DHFT_RTL_TRACE=$<TARGET_FILE:hft_rtl_trace>
                     -DHFT_RTL_CHECK=$<TARGET_FILE:hft_rtl_check>
                     -DGHDL=${GHDL}
                     -DRTL_DIR=${CMAKE_CURRENT_SOURCE_DIR}/rtl
                     -DWORK=${CMAKE_CURRENT_BINARY_DIR}/rtl_diff_trunc_work
                     -DRECORDS=2000
                     -DTRUNCATE_AT=4096
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rtl_differential.cmake")
    set_tests_properties(rtl_decode_differential_truncated PROPERTIES TIMEOUT 900)

    # The order-book differential, one layer up: the C++ book and the
    # RTL book, over the same capture, compared field by field.
    #
    # The decode differential proved that two implementations agree on
    # what the WIRE says. This proves that two implementations agree on
    # what the BOOK does with it -- the price-time priority, the queue
    # order within a price, the level aggregates, and the order count
    # at every level.
    #
    # The comparison is a dump and not a checksum, because a checksum
    # says only "these differ" and a dump says which order and which
    # field.
    add_test(NAME rtl_book_differential
             COMMAND "${CMAKE_COMMAND}"
                     -DHFT_RTL_TRACE=$<TARGET_FILE:hft_rtl_trace>
                     -DHFT_BOOK_DUMP=$<TARGET_FILE:hft_book_dump>
                     -DHFT_BOOK_CHECK=$<TARGET_FILE:hft_book_check>
                     -DGHDL=${GHDL}
                     -DRTL_DIR=${CMAKE_CURRENT_SOURCE_DIR}/rtl
                     -DWORK=${CMAKE_CURRENT_BINARY_DIR}/rtl_book_work
                     -DRECORDS=1000
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rtl_book_differential.cmake")
    set_tests_properties(rtl_book_differential PROPERTIES TIMEOUT 1800)

    # The real-data differential: the same two comparisons, over REAL
    # Nasdaq market data rather than a generated capture.
    #
    # The generated captures are a limitation with a name: a generator
    # and a decoder that share a mistake agree with each other and are
    # both wrong. The Order Executed field table is the proof -- it
    # carried a Printable byte the message does not have, and every
    # test in this repository agreed with it. The first real message
    # from the exchange disagreed with both.
    #
    # The data is fetched, not committed: the exchange's files are
    # several gigabytes and are the exchange's property. When the
    # network is not there the test SKIPS rather than fails, because a
    # missing network is not a wrong answer.
    if(WIN32)
        set(FETCH_SCRIPT powershell.exe -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch-itch.ps1")
    else()
        set(FETCH_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch-itch.sh")
    endif()
    add_test(NAME rtl_real_data_differential
             COMMAND "${CMAKE_COMMAND}"
                     -DHFT_RTL_TRACE=$<TARGET_FILE:hft_rtl_trace>
                     -DHFT_RTL_CHECK=$<TARGET_FILE:hft_rtl_check>
                     -DHFT_BOOK_DUMP=$<TARGET_FILE:hft_book_dump>
                     -DHFT_BOOK_CHECK=$<TARGET_FILE:hft_book_check>
                     -DHFT_CAPTURE_INSPECT=$<TARGET_FILE:hft_capture_inspect>
                     -DHFT_ITCH_CONVERT=$<TARGET_FILE:hft_itch_convert>
                     -DGHDL=${GHDL}
                     -DRTL_DIR=${CMAKE_CURRENT_SOURCE_DIR}/rtl
                     -DWORK=${CMAKE_CURRENT_BINARY_DIR}/rtl_real_work
                     -DRAW=${CMAKE_CURRENT_BINARY_DIR}/rtl_real_work/raw.itch
                     "-DFETCH=${FETCH_SCRIPT}"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/real_data_differential.cmake")
    set_tests_properties(rtl_real_data_differential PROPERTIES TIMEOUT 3600)

    message(STATUS "  ghdl .............. ${GHDL}")
else()
    message(STATUS "  ghdl .............. not found, the RTL differential is skipped")
endif()

# The threaded pipeline must build the same book as the single-threaded
# one from the same feed. That is a correctness property, not a speed
# measurement, so it belongs in CI: a pipeline that drops or reorders a
# message looks exactly like a win in every other column.
#
# A small record count is deliberate. The checksum comparison is
# deterministic and needs no volume, and the run-to-run drift at small
# sizes would make the speedup column meaningless anyway -- the tool says
# so in its own output.
add_test(NAME pipeline_threaded_equivalence
         COMMAND hft_pipeline_bench 50000)
set_tests_properties(pipeline_threaded_equivalence PROPERTIES
    PASS_REGULAR_EXPRESSION "All four threaded runs produced a book identical")

# Clock characterisation. Not an engine measurement -- it establishes the
# resolution floor every other latency figure in this repository depends
# on, and fails when the TSC cannot be calibrated. Asserted in CI because a
# benchmark host whose clock cannot be calibrated cannot produce a
# publishable latency table, and that should stop the run rather than be
# discovered afterwards.
add_test(NAME tsc_calibration COMMAND hft_tsc_bench)
set_tests_properties(tsc_calibration PROPERTIES
    PASS_REGULAR_EXPRESSION "invariant TSC      YES")

# Ring benchmark is a measurement, not a test, so only its correctness
# claim is asserted here: every message transferred, and every echoed
# token returned intact. The tool prints a single verdict line so this is
# one regex rather than a pair of "must not contain" checks that CTest
# cannot express.
add_test(NAME ring_transfer_integrity COMMAND hft_ring_bench 200000)
set_tests_properties(ring_transfer_integrity PROPERTIES
    PASS_REGULAR_EXPRESSION "transfer integrity: PASS")

# Documentation consistency. Not a code test: this checks that every
# relative link target exists and every anchor has a matching heading.
#
# It is in CI because this repository's documentation accreted
# corrections instead of replacing them, and three sections came to
# contradict each other while every link between them still resolved. A
# broken anchor is the same failure in a smaller costume -- the reader is
# sent confidently to a place that no longer says what the link claims.
if(WIN32)
    find_program(DOC_CHECK powershell.exe)
    if(DOC_CHECK)
        add_test(NAME docs COMMAND "${DOC_CHECK}" -NoProfile -ExecutionPolicy Bypass
                 -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-docs.ps1")
    else()
        message(STATUS "  doc check ......... powershell not found, skipped")
    endif()
else()
    find_program(DOC_CHECK check-docs.sh PATHS "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
    if(DOC_CHECK)
        add_test(NAME docs COMMAND "${DOC_CHECK}")
    else()
        message(STATUS "  doc check ......... bash not found, skipped")
    endif()
endif()

# Claim check: the counts the README publishes against the counts the
# binaries actually print.
#
# Separate from the doc check above, and for a different reason. The doc
# check asks whether a link resolves; it cannot notice that "783 checks"
# disagrees with a binary printing 1,412, because a number typed by hand
# into prose has nothing to disagree with it. That is how this repository
# shipped a check count wrong by more than a factor of two while every
# test passed and the docs check was green.
#
# CMAKE_BINARY_DIR is passed explicitly because CTest runs tests with the
# build directory as the working directory, and a relative path from there
# would resolve against the wrong tree.
if(WIN32)
    find_program(CLAIM_CHECK powershell.exe)
    if(CLAIM_CHECK)
        add_test(NAME claims COMMAND "${CLAIM_CHECK}" -NoProfile -ExecutionPolicy Bypass
                 -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-claims.ps1"
                 -BuildDir "${CMAKE_BINARY_DIR}")
    else()
        message(STATUS "  claim check ....... powershell not found, skipped")
    endif()
else()
    find_program(CLAIM_CHECK check-claims.sh PATHS "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
    if(CLAIM_CHECK)
        add_test(NAME claims COMMAND "${CLAIM_CHECK}" "${CMAKE_BINARY_DIR}")
    else()
        message(STATUS "  claim check ....... bash not found, skipped")
    endif()
endif()

# ---- Reporting -----------------------------------------------------
message(STATUS "hft_engine ${PROJECT_VERSION}")
message(STATUS "  build type ....... ${CMAKE_BUILD_TYPE}")
message(STATUS "  native arch ...... ${HFT_NATIVE_ARCH}")
message(STATUS "  C++ standard ..... ${CMAKE_CXX_STANDARD}")
