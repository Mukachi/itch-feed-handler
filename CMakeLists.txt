cmake_minimum_required(VERSION 3.28)
project(itch-feed-handler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(itchfh INTERFACE)
target_include_directories(itchfh INTERFACE include)
target_compile_options(itchfh INTERFACE -Wall -Wextra -Wpedantic -Werror)
# x86-64-v3 (AVX2, BMI1/2, LZCNT, MOVBE), as the gateway: a named level,
# recorded in every run's flags.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(ITCHFH_ARCH_FLAGS -march=x86-64-v3)
  target_compile_options(itchfh INTERFACE ${ITCHFH_ARCH_FLAGS})
endif()

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()

# lockfree-ipc: its rings and measurement harness (note 001), fetched at a
# pinned commit into build/_deps. Headers only: SOURCE_SUBDIR names no
# directory, so its own tests, benchmarks and baselines are not built here.
include(FetchContent)
set(ITCHFH_LFIPC_COMMIT c733131c95198f08cc9032b7ae105cfaf6f7cd88)
FetchContent_Declare(lockfree_ipc
  GIT_REPOSITORY https://github.com/Mukachi/lockfree-ipc.git
  GIT_TAG ${ITCHFH_LFIPC_COMMIT}
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(lockfree_ipc)

# Build provenance for this repository, written by lockfree-ipc's script into
# lfipc/build_info.hpp, where its environment capture reads it (lockfree-ipc
# note 004).
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
add_custom_target(build_info ALL
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/lfipc/build_info.hpp
          -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
          "-DFLAGS=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type_upper}} -Wall -Wextra -Wpedantic -Werror ${ITCHFH_ARCH_FLAGS}"
          -P ${lockfree_ipc_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/lfipc/build_info.hpp
  COMMENT "Recording build provenance")

add_library(lfipc INTERFACE)
target_include_directories(lfipc SYSTEM INTERFACE ${lockfree_ipc_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(lfipc INTERFACE ITCHFH_LFIPC_COMMIT="${ITCHFH_LFIPC_COMMIT}")
add_dependencies(lfipc build_info)

# Tests: plain executables under CTest. Each one added with its component.
set(ITCHFH_TESTS reference_book_test book_test reference_decoder_test handler_test decoder_test)
foreach(t ${ITCHFH_TESTS})
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE itchfh lfipc Threads::Threads)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# S4 of note 002: applying an event allocates nothing and makes no system call,
# checked in the compiled code (not under sanitizers, which add calls).
add_executable(hot_path_subject tests/hot_path_subject.cpp)
target_link_libraries(hot_path_subject PRIVATE itchfh)
if(CMAKE_OBJDUMP AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize")
  add_test(NAME check_hot_path
           COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/check_hot_path.py
                   ${CMAKE_OBJDUMP} $<TARGET_FILE:hot_path_subject>)
endif()

# Over the sample day (label `data`: needs data/, so CI leaves it out).
add_executable(sample_day_test tests/sample_day_test.cpp)
target_link_libraries(sample_day_test PRIVATE itchfh)
target_compile_definitions(sample_day_test PRIVATE ITCHFH_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
add_test(NAME sample_day_test COMMAND sample_day_test)
set_tests_properties(sample_day_test PROPERTIES LABELS data TIMEOUT 7200)
add_executable(handler_day_test tests/handler_day_test.cpp)
target_link_libraries(handler_day_test PRIVATE itchfh)
target_compile_definitions(handler_day_test PRIVATE ITCHFH_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
add_test(NAME handler_day_test COMMAND handler_day_test)
set_tests_properties(handler_day_test PROPERTIES LABELS data TIMEOUT 14400)

# The benchmark (note 002, S5): writes into results/ only in bench mode.
add_executable(book_bench bench/book_bench.cpp)
target_link_libraries(book_bench PRIVATE itchfh lfipc Threads::Threads)
target_compile_definitions(book_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results"
                                              ITCHFH_DATA_DIR="${CMAKE_SOURCE_DIR}/data")

add_executable(handler_bench bench/handler_bench.cpp)
target_link_libraries(handler_bench PRIVATE itchfh lfipc Threads::Threads)
target_compile_definitions(handler_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results"
                                                 ITCHFH_DATA_DIR="${CMAKE_SOURCE_DIR}/data")

# The extraction tool of note 002, D7 (not part of the handler).
add_executable(extract_events tools/extract_events.cpp)
target_link_libraries(extract_events PRIVATE itchfh)

# One number everywhere: a stale RESULTS.md or README figure fails the suite.
add_test(NAME report_check COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/run.py report --check
                                  --lockfree-ipc ${lockfree_ipc_SOURCE_DIR})
