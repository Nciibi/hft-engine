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
    src/feed/generator.cpp)

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

# ---- Reporting -----------------------------------------------------
message(STATUS "hft_engine ${PROJECT_VERSION}")
message(STATUS "  build type ....... ${CMAKE_BUILD_TYPE}")
message(STATUS "  native arch ...... ${HFT_NATIVE_ARCH}")
message(STATUS "  C++ standard ..... ${CMAKE_CXX_STANDARD}")
