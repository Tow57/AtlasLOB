cmake_minimum_required(VERSION 3.25)

project(
  AtlasLOB
  VERSION 0.2.0
  DESCRIPTION "Deterministic C++20 limit order book and matching engine"
  LANGUAGES CXX
)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AtlasSanitizers)
include(AtlasWarnings)

option(ATLAS_WARNINGS_AS_ERRORS "Treat AtlasLOB warnings as errors" ON)
option(ATLAS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ATLAS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ATLAS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ATLAS_ENABLE_INVARIANTS "Enable expensive internal invariant checks" ON)
option(ATLAS_BUILD_FUZZERS "Build Clang libFuzzer decoder targets" OFF)
option(ATLAS_BUILD_PYTHON "Build the private CPython extension module" OFF)
option(ATLAS_BUILD_BENCHMARKS "Build opt-in performance measurement tools" OFF)

if(BUILD_TESTING OR ATLAS_BUILD_BENCHMARKS)
  include(AtlasDependencies)
endif()

if(ATLAS_BUILD_PYTHON)
  # The native extension links the project libraries into a shared module.
  # Ordinary C++ builds remain unaffected when the option is disabled.
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build AtlasLOB project libraries as shared" FORCE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

add_library(atlas_domain)
add_library(AtlasLOB::domain ALIAS atlas_domain)

target_sources(
  atlas_domain
  PRIVATE src/domain/validation.cpp
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
      include/atlaslob/domain/commands.hpp
      include/atlaslob/domain/event_batch.hpp
      include/atlaslob/domain/events.hpp
      include/atlaslob/domain/types.hpp
      include/atlaslob/domain/validation.hpp
)

target_compile_features(atlas_domain PUBLIC cxx_std_20)
set_target_properties(atlas_domain PROPERTIES CXX_EXTENSIONS OFF)
atlas_set_project_warnings(atlas_domain)
atlas_enable_sanitizers(atlas_domain)

add_library(
  atlas_internal_utility
  src/utility/sha256.cpp
  src/utility/sha256.hpp
)
target_include_directories(
  atlas_internal_utility
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/utility"
)
target_compile_features(atlas_internal_utility PUBLIC cxx_std_20)
set_target_properties(atlas_internal_utility PROPERTIES CXX_EXTENSIONS OFF)
atlas_set_project_warnings(atlas_internal_utility)
atlas_enable_sanitizers(atlas_internal_utility)

add_library(atlas_core)
add_library(AtlasLOB::core ALIAS atlas_core)

set(
  ATLAS_CORE_SOURCES
  src/core/active_order_index.cpp
  src/core/active_order_index.hpp
  src/core/book_side.cpp
  src/core/book_side.hpp
  src/core/canonical_digest.cpp
  src/core/canonical_digest.hpp
  src/core/checked_arithmetic.hpp
  src/core/command_admission.cpp
  src/core/command_admission.hpp
  src/core/command_executor.cpp
  src/core/command_executor.hpp
  src/core/command_sequencer.hpp
  src/core/event_batch_builder.cpp
  src/core/event_batch_builder.hpp
  src/core/execution_projection.cpp
  src/core/execution_projection.hpp
  src/core/execution_policy.hpp
  src/core/instrument_book.cpp
  src/core/instrument_book.hpp
  src/core/matching_engine.cpp
  src/core/multi_instrument_engine.cpp
  src/core/multi_instrument_engine_access.hpp
  src/core/match_plan.cpp
  src/core/match_plan.hpp
  src/core/order_node.hpp
  src/core/order_storage.cpp
  src/core/order_storage.hpp
  src/core/price_level.cpp
  src/core/price_level.hpp
  src/core/snapshot_sequence.hpp
  src/core/state_validation.cpp
  src/core/state_validation.hpp
  src/core/top_of_book.hpp
)

function(atlas_configure_core target_name)
  target_sources(
    ${target_name}
    PRIVATE ${ATLAS_CORE_SOURCES}
    PUBLIC
      FILE_SET HEADERS
      BASE_DIRS include
      FILES
        include/atlaslob/book_snapshot.hpp
        include/atlaslob/digest.hpp
        include/atlaslob/matching_engine.hpp
        include/atlaslob/multi_instrument_engine.hpp
  )
  target_link_libraries(
    ${target_name}
    PUBLIC AtlasLOB::domain
    PRIVATE atlas_internal_utility
  )
  target_include_directories(${target_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
  target_compile_features(${target_name} PUBLIC cxx_std_20)
  target_compile_definitions(
    ${target_name}
    PRIVATE ATLAS_ENABLE_INVARIANTS=$<BOOL:${ATLAS_ENABLE_INVARIANTS}>
  )
  set_target_properties(${target_name} PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(${target_name})
  atlas_enable_sanitizers(${target_name})
endfunction()

atlas_configure_core(atlas_core)

set(
  ATLAS_PERSISTENCE_PRIVATE_SOURCES
  src/persistence/binary_codec.cpp
  src/persistence/binary_codec.hpp
  src/persistence/command_log_codec.cpp
  src/persistence/command_log_codec.hpp
  src/persistence/configuration_digest.cpp
  src/persistence/crc32c.cpp
  src/persistence/crc32c.hpp
  src/persistence/inspection.cpp
  src/persistence/log_io.cpp
  src/persistence/log_io.hpp
  src/persistence/log_scanner.cpp
  src/persistence/log_scanner.hpp
  src/persistence/logged_engine.cpp
  src/persistence/logged_engine_internal.hpp
  src/persistence/replay.cpp
  src/persistence/replay_internal.hpp
  src/persistence/reports.cpp
  src/persistence/reports.hpp
  src/persistence/snapshot_codec.cpp
  src/persistence/snapshot_codec.hpp
  src/persistence/snapshot_recovery.cpp
  src/persistence/snapshot_store.cpp
  src/persistence/snapshot_store_internal.hpp
)

add_library(atlas_persistence)
add_library(AtlasLOB::persistence ALIAS atlas_persistence)
target_sources(
  atlas_persistence
  PRIVATE ${ATLAS_PERSISTENCE_PRIVATE_SOURCES}
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
      include/atlaslob/persistence/command_log.hpp
      include/atlaslob/persistence/configuration_digest.hpp
      include/atlaslob/persistence/inspection.hpp
      include/atlaslob/persistence/logged_engine.hpp
      include/atlaslob/persistence/replay.hpp
      include/atlaslob/persistence/snapshot.hpp
      include/atlaslob/persistence/snapshot_store.hpp
      include/atlaslob/persistence/types.hpp
)
target_link_libraries(
  atlas_persistence
  PUBLIC AtlasLOB::core
  PRIVATE atlas_internal_utility
)
target_include_directories(
  atlas_persistence
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/persistence"
)
target_compile_features(atlas_persistence PUBLIC cxx_std_20)
set_target_properties(atlas_persistence PROPERTIES CXX_EXTENSIONS OFF)
atlas_set_project_warnings(atlas_persistence)
atlas_enable_sanitizers(atlas_persistence)

if(ATLAS_BUILD_PYTHON)
  set(PYBIND11_FINDPYTHON ON)
  find_package(Python 3.11 REQUIRED COMPONENTS Interpreter Development.Module)
  find_package(pybind11 3.0.4 CONFIG REQUIRED)

  pybind11_add_module(
    atlas_native_engine
    MODULE
    src/python/native_engine_module.cpp
  )
  target_link_libraries(atlas_native_engine PRIVATE AtlasLOB::persistence)
  target_include_directories(
    atlas_native_engine
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/persistence"
  )
  target_compile_features(atlas_native_engine PRIVATE cxx_std_20)
  set_target_properties(
    atlas_native_engine
    PROPERTIES
      CXX_EXTENSIONS OFF
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
      OUTPUT_NAME "_native_engine"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ATLAS_NATIVE_ENGINE_VERSION_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AtlasNativeEngineExports.map")
    target_link_options(
      atlas_native_engine
      PRIVATE "LINKER:--version-script=${ATLAS_NATIVE_ENGINE_VERSION_SCRIPT}"
    )
    set_property(
      TARGET atlas_native_engine
      APPEND
      PROPERTY LINK_DEPENDS "${ATLAS_NATIVE_ENGINE_VERSION_SCRIPT}"
    )
  endif()
  if(MINGW)
    # A wheel must not depend on the build machine's MinGW runtime DLLs. Keep
    # the CPython DLL dynamic while embedding the GCC, C++, and winpthreads
    # runtimes in the extension itself.
    target_link_options(
      atlas_native_engine
      PRIVATE
        -static-libstdc++
        -static-libgcc
        -Wl,-Bstatic
        -Wl,--whole-archive
        -lwinpthread
        -Wl,--no-whole-archive
        -Wl,-Bdynamic
    )
  endif()
  atlas_set_project_warnings(atlas_native_engine)
  atlas_enable_sanitizers(atlas_native_engine)

  install(
    TARGETS atlas_native_engine
    LIBRARY
      DESTINATION atlaslob
      COMPONENT python
    RUNTIME
      DESTINATION atlaslob
      COMPONENT python
  )
endif()

if(ATLAS_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC)
    message(
      FATAL_ERROR
      "ATLAS_BUILD_FUZZERS requires a non-MSVC Clang toolchain"
    )
  endif()

  # Keep libFuzzer coverage instrumentation isolated from the ordinary
  # persistence library and tools while compiling the complete decoder
  # implementation into every fuzz executable.
  add_library(
    atlas_persistence_fuzz
    STATIC
    ${ATLAS_PERSISTENCE_PRIVATE_SOURCES}
  )
  target_link_libraries(
    atlas_persistence_fuzz
    PUBLIC AtlasLOB::core
    PRIVATE atlas_internal_utility
  )
  target_include_directories(
    atlas_persistence_fuzz
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/persistence"
  )
  target_compile_features(atlas_persistence_fuzz PUBLIC cxx_std_20)
  target_compile_options(atlas_persistence_fuzz PRIVATE -fsanitize=fuzzer-no-link)
  set_target_properties(atlas_persistence_fuzz PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(atlas_persistence_fuzz)
  atlas_enable_sanitizers(atlas_persistence_fuzz)

  foreach(decoder IN ITEMS log_header log_record snapshot)
    set(fuzzer_target "atlas_fuzz_${decoder}_decoder")
    add_executable(
      ${fuzzer_target}
      "tests/fuzz/${decoder}_decoder_fuzz.cpp"
    )
    target_link_libraries(${fuzzer_target} PRIVATE atlas_persistence_fuzz)
    target_include_directories(
      ${fuzzer_target}
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/persistence"
    )
    target_compile_features(${fuzzer_target} PRIVATE cxx_std_20)
    target_compile_options(${fuzzer_target} PRIVATE -fsanitize=fuzzer-no-link)
    target_link_options(${fuzzer_target} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${fuzzer_target} PROPERTIES CXX_EXTENSIONS OFF)
    atlas_set_project_warnings(${fuzzer_target})
    atlas_enable_sanitizers(${fuzzer_target})
  endforeach()
endif()

add_library(
  atlas_persistence_cli_support
  src/tools/platform_cli.cpp
  src/tools/platform_cli.hpp
  src/tools/persistence_cli.cpp
  src/tools/persistence_cli.hpp
)
target_link_libraries(
  atlas_persistence_cli_support
  PUBLIC AtlasLOB::persistence
)
target_include_directories(
  atlas_persistence_cli_support
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/tools"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/persistence"
)
target_compile_features(atlas_persistence_cli_support PUBLIC cxx_std_20)
if(WIN32)
  target_link_libraries(atlas_persistence_cli_support PRIVATE shell32)
endif()
set_target_properties(
  atlas_persistence_cli_support
  PROPERTIES CXX_EXTENSIONS OFF
)
atlas_set_project_warnings(atlas_persistence_cli_support)
atlas_enable_sanitizers(atlas_persistence_cli_support)

add_executable(atlas_inspect src/tools/atlas_inspect_main.cpp)
target_link_libraries(atlas_inspect PRIVATE atlas_persistence_cli_support)
target_compile_features(atlas_inspect PRIVATE cxx_std_20)
set_target_properties(atlas_inspect PROPERTIES CXX_EXTENSIONS OFF)
atlas_set_project_warnings(atlas_inspect)
atlas_enable_sanitizers(atlas_inspect)

add_executable(atlas_replay src/tools/atlas_replay_main.cpp)
target_link_libraries(atlas_replay PRIVATE atlas_persistence_cli_support)
target_compile_features(atlas_replay PRIVATE cxx_std_20)
set_target_properties(atlas_replay PROPERTIES CXX_EXTENSIONS OFF)
atlas_set_project_warnings(atlas_replay)
atlas_enable_sanitizers(atlas_replay)

add_library(
  atlas_cli_support
  src/cli/domain_fixture.cpp
  src/cli/domain_fixture.hpp
  src/cli/domain_fixture_formatter.cpp
  src/cli/domain_fixture_formatter.hpp
  src/cli/domain_fixture_parser.cpp
  src/cli/domain_fixture_parser.hpp
  src/cli/engine_fixture.cpp
  src/cli/engine_fixture.hpp
)
target_link_libraries(atlas_cli_support PUBLIC AtlasLOB::domain)
target_link_libraries(atlas_cli_support PRIVATE AtlasLOB::core)
target_include_directories(atlas_cli_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/cli")
target_compile_features(atlas_cli_support PUBLIC cxx_std_20)
set_target_properties(atlas_cli_support PROPERTIES CXX_EXTENSIONS OFF)
atlas_set_project_warnings(atlas_cli_support)
atlas_enable_sanitizers(atlas_cli_support)

add_executable(atlas_cli src/cli/main.cpp)
target_link_libraries(atlas_cli PRIVATE AtlasLOB::domain atlas_cli_support)
target_compile_features(atlas_cli PRIVATE cxx_std_20)
set_target_properties(atlas_cli PROPERTIES CXX_EXTENSIONS OFF)
atlas_set_project_warnings(atlas_cli)
atlas_enable_sanitizers(atlas_cli)

if(ATLAS_BUILD_BENCHMARKS)
  atlas_setup_benchmark_dependencies()

  add_library(
    atlas_benchmark_support
    STATIC
    src/benchmark/benchmark_log_materializer.cpp
    src/benchmark/benchmark_log_materializer.hpp
    src/benchmark/benchmark_observation.cpp
    src/benchmark/benchmark_observation.hpp
    src/benchmark/benchmark_replay.cpp
    src/benchmark/benchmark_replay.hpp
    src/benchmark/benchmark_runner.cpp
    src/benchmark/benchmark_runner.hpp
    src/benchmark/benchmark_workload.cpp
    src/benchmark/benchmark_workload.hpp
  )
  target_link_libraries(
    atlas_benchmark_support
    PUBLIC AtlasLOB::core
    PRIVATE AtlasLOB::persistence
    PRIVATE atlas_persistence_cli_support
    PRIVATE atlas_internal_utility
  )
  target_include_directories(
    atlas_benchmark_support
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark"
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/persistence"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/utility"
  )
  target_compile_features(atlas_benchmark_support PUBLIC cxx_std_20)
  set_target_properties(
    atlas_benchmark_support
    PROPERTIES CXX_EXTENSIONS OFF
  )
  atlas_set_project_warnings(atlas_benchmark_support)
  atlas_enable_sanitizers(atlas_benchmark_support)

  add_executable(
    atlas_bench_runner
    src/benchmark/atlas_bench_runner_main.cpp
  )
  target_link_libraries(atlas_bench_runner PRIVATE atlas_benchmark_support)
  target_compile_features(atlas_bench_runner PRIVATE cxx_std_20)
  set_target_properties(atlas_bench_runner PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(atlas_bench_runner)
  atlas_enable_sanitizers(atlas_bench_runner)

  add_executable(
    atlas_bench_alloc_runner
    src/benchmark/allocation_tracker.cpp
    src/benchmark/allocation_tracker.hpp
    src/benchmark/atlas_bench_alloc_runner_main.cpp
  )
  target_link_libraries(atlas_bench_alloc_runner PRIVATE atlas_benchmark_support)
  if(MINGW)
    # Keep every replaceable-new caller on the same C++ runtime side of the
    # allocation tracker. Dynamic MinGW libstdc++ may otherwise allocate in the
    # DLL and deallocate through this executable's interposed delete surface.
    target_link_options(
      atlas_bench_alloc_runner
      PRIVATE -static-libstdc++ -static-libgcc
    )
  endif()
  target_compile_features(atlas_bench_alloc_runner PRIVATE cxx_std_20)
  set_target_properties(
    atlas_bench_alloc_runner
    PROPERTIES CXX_EXTENSIONS OFF
  )
  atlas_set_project_warnings(atlas_bench_alloc_runner)
  atlas_enable_sanitizers(atlas_bench_alloc_runner)

  add_executable(
    atlas_bench_log_materializer
    src/benchmark/atlas_bench_log_materializer_main.cpp
  )
  target_link_libraries(atlas_bench_log_materializer PRIVATE atlas_benchmark_support)
  target_compile_features(atlas_bench_log_materializer PRIVATE cxx_std_20)
  set_target_properties(
    atlas_bench_log_materializer
    PROPERTIES CXX_EXTENSIONS OFF
  )
  atlas_set_project_warnings(atlas_bench_log_materializer)
  atlas_enable_sanitizers(atlas_bench_log_materializer)

  add_executable(atlas_microbench benchmarks/micro/atlas_microbench.cpp)
  target_link_libraries(
    atlas_microbench
    PRIVATE
      AtlasLOB::core
      benchmark::benchmark
  )
  target_compile_features(atlas_microbench PRIVATE cxx_std_20)
  set_target_properties(atlas_microbench PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(atlas_microbench)
  atlas_enable_sanitizers(atlas_microbench)

  add_custom_target(
    benchmark-smoke
    COMMAND
      "$<TARGET_FILE:atlas_microbench>"
      --benchmark_dry_run=true
      --benchmark_filter=PublicEngine
    DEPENDS
      atlas_bench_alloc_runner
      atlas_bench_log_materializer
      atlas_bench_runner
      atlas_microbench
    COMMENT "Running AtlasLOB microbenchmark dry-run smoke"
    USES_TERMINAL
    VERBATIM
  )

  if(BUILD_TESTING)
    add_test(
      NAME benchmark.microbench_dry_run
      COMMAND
        atlas_microbench
        --benchmark_dry_run=true
        --benchmark_filter=PublicEngine
    )
    set_tests_properties(
      benchmark.microbench_dry_run
      PROPERTIES LABELS "benchmark"
    )
  endif()
endif()

if(BUILD_TESTING)
  atlas_setup_test_dependencies()
  include(GoogleTest)

  if(ATLAS_BUILD_BENCHMARKS)
    add_executable(
      atlas_benchmark_tests
      tests/unit/benchmark_runner_test.cpp
    )
    target_link_libraries(
      atlas_benchmark_tests
      PRIVATE
        atlas_benchmark_support
        atlas_internal_utility
        GTest::gtest_main
    )
    target_compile_features(atlas_benchmark_tests PRIVATE cxx_std_20)
    set_target_properties(
      atlas_benchmark_tests
      PROPERTIES CXX_EXTENSIONS OFF
    )
    atlas_set_project_warnings(atlas_benchmark_tests)
    atlas_enable_sanitizers(atlas_benchmark_tests)

    gtest_discover_tests(
      atlas_benchmark_tests
      TEST_PREFIX "benchmark."
      PROPERTIES LABELS "benchmark"
    )

    add_executable(
      atlas_allocation_tracker_process_test
      src/benchmark/allocation_tracker.cpp
      src/benchmark/allocation_tracker.hpp
      tests/unit/allocation_tracker_process_test.cpp
    )
    target_include_directories(
      atlas_allocation_tracker_process_test
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark"
    )
    target_compile_features(atlas_allocation_tracker_process_test PRIVATE cxx_std_20)
    set_target_properties(
      atlas_allocation_tracker_process_test
      PROPERTIES CXX_EXTENSIONS OFF
    )
    if(MINGW)
      target_link_options(
        atlas_allocation_tracker_process_test
        PRIVATE -static-libstdc++ -static-libgcc
      )
    endif()
    atlas_set_project_warnings(atlas_allocation_tracker_process_test)
    atlas_enable_sanitizers(atlas_allocation_tracker_process_test)
    add_test(
      NAME benchmark.allocation_tracker_process
      COMMAND atlas_allocation_tracker_process_test
    )
    set_tests_properties(
      benchmark.allocation_tracker_process
      PROPERTIES LABELS "benchmark"
    )
    add_dependencies(
      benchmark-smoke
      atlas_allocation_tracker_process_test
      atlas_benchmark_tests
    )
  endif()

  add_executable(atlas_diff_native tests/differential/native_driver.cpp)
  target_link_libraries(atlas_diff_native PRIVATE AtlasLOB::core)
  target_include_directories(
    atlas_diff_native
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential"
  )
  target_compile_features(atlas_diff_native PRIVATE cxx_std_20)
  set_target_properties(atlas_diff_native PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(atlas_diff_native)
  atlas_enable_sanitizers(atlas_diff_native)

  add_executable(
    atlas_diff_multi_native
    tests/differential/multi_native_driver.cpp
  )
  target_link_libraries(atlas_diff_multi_native PRIVATE AtlasLOB::core)
  target_include_directories(
    atlas_diff_multi_native
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential"
  )
  target_compile_features(atlas_diff_multi_native PRIVATE cxx_std_20)
  set_target_properties(
    atlas_diff_multi_native
    PROPERTIES CXX_EXTENSIONS OFF
  )
  atlas_set_project_warnings(atlas_diff_multi_native)
  atlas_enable_sanitizers(atlas_diff_multi_native)

  add_executable(
    atlas_native_diff_tests
    tests/differential/native_driver.cpp
    tests/integration/native_diff_driver_test.cpp
  )
  target_link_libraries(atlas_native_diff_tests PRIVATE AtlasLOB::core GTest::gtest_main)
  target_include_directories(
    atlas_native_diff_tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential"
  )
  target_compile_definitions(atlas_native_diff_tests PRIVATE ATLAS_DIFF_NATIVE_NO_MAIN=1)
  target_compile_features(atlas_native_diff_tests PRIVATE cxx_std_20)
  set_target_properties(atlas_native_diff_tests PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(atlas_native_diff_tests)
  atlas_enable_sanitizers(atlas_native_diff_tests)

  gtest_discover_tests(
    atlas_native_diff_tests
    TEST_PREFIX "differential."
    PROPERTIES LABELS "integration\\;differential"
  )

  add_executable(
    atlas_multi_native_diff_tests
    tests/differential/multi_native_driver.cpp
    tests/integration/multi_native_diff_driver_test.cpp
  )
  target_link_libraries(
    atlas_multi_native_diff_tests
    PRIVATE AtlasLOB::core GTest::gtest_main
  )
  target_include_directories(
    atlas_multi_native_diff_tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential"
  )
  target_compile_definitions(
    atlas_multi_native_diff_tests
    PRIVATE ATLAS_DIFF_MULTI_NATIVE_NO_MAIN=1
  )
  target_compile_features(atlas_multi_native_diff_tests PRIVATE cxx_std_20)
  set_target_properties(
    atlas_multi_native_diff_tests
    PROPERTIES CXX_EXTENSIONS OFF
  )
  atlas_set_project_warnings(atlas_multi_native_diff_tests)
  atlas_enable_sanitizers(atlas_multi_native_diff_tests)

  gtest_discover_tests(
    atlas_multi_native_diff_tests
    TEST_PREFIX "differential."
    PROPERTIES LABELS "integration\\;differential"
  )

  set(ATLAS_NATIVE_CLI_TEST_INPUT "${CMAKE_CURRENT_BINARY_DIR}/native_cli_test.input")
  file(
    GENERATE
    OUTPUT "${ATLAS_NATIVE_CLI_TEST_INPUT}"
    CONTENT "ATLAS_DIFF_V1 7 1000 1 16 0\n"
  )
  foreach(
    test_case
    IN ITEMS
      exact
      compact
      equals_exact
      equals_compact
      split_exact
      split_compact
      invalid_usage
  )
    add_test(
      NAME differential.native_cli_${test_case}
      COMMAND
        "${CMAKE_COMMAND}"
        "-DNATIVE_EXECUTABLE=$<TARGET_FILE:atlas_diff_native>"
        "-DINPUT_FILE=${ATLAS_NATIVE_CLI_TEST_INPUT}"
        "-DTEST_CASE=${test_case}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/native_cli_test.cmake"
    )
    set_tests_properties(
      differential.native_cli_${test_case}
      PROPERTIES LABELS "integration;differential"
    )
  endforeach()

  add_library(atlas_core_testable)
  atlas_configure_core(atlas_core_testable)
  target_compile_definitions(atlas_core_testable PUBLIC ATLAS_ENABLE_TEST_ACCESS=1)

  file(
    GLOB ATLAS_DOMAIN_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/domain_*_test.cpp"
  )
  add_executable(atlas_domain_tests ${ATLAS_DOMAIN_TEST_SOURCES})
  target_link_libraries(atlas_domain_tests PRIVATE AtlasLOB::domain GTest::gtest_main)
  target_compile_features(atlas_domain_tests PRIVATE cxx_std_20)
  set_target_properties(atlas_domain_tests PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(atlas_domain_tests)
  atlas_enable_sanitizers(atlas_domain_tests)

  gtest_discover_tests(
    atlas_domain_tests
    TEST_PREFIX "domain."
    PROPERTIES LABELS "domain"
  )

  file(
    GLOB ATLAS_FIXTURE_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/*fixture*_test.cpp"
  )
  add_executable(atlas_fixture_tests ${ATLAS_FIXTURE_TEST_SOURCES})
  target_link_libraries(
    atlas_fixture_tests
    PRIVATE AtlasLOB::core atlas_cli_support GTest::gtest_main
  )
  target_compile_features(atlas_fixture_tests PRIVATE cxx_std_20)
  set_target_properties(atlas_fixture_tests PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(atlas_fixture_tests)
  atlas_enable_sanitizers(atlas_fixture_tests)

  gtest_discover_tests(
    atlas_fixture_tests
    TEST_PREFIX "integration."
    PROPERTIES LABELS "integration"
  )

  file(
    GLOB ATLAS_CORE_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/core_*_test.cpp"
  )
  add_executable(atlas_core_tests ${ATLAS_CORE_TEST_SOURCES})
  target_link_libraries(atlas_core_tests PRIVATE atlas_core_testable GTest::gtest_main)
  target_include_directories(
    atlas_core_tests
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  )
  target_compile_features(atlas_core_tests PRIVATE cxx_std_20)
  set_target_properties(atlas_core_tests PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(atlas_core_tests)
  atlas_enable_sanitizers(atlas_core_tests)

  gtest_discover_tests(
    atlas_core_tests
    TEST_PREFIX "core."
    PROPERTIES LABELS "core"
  )

  file(
    GLOB ATLAS_PERSISTENCE_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/persistence_*_test.cpp"
  )
  add_executable(
    atlas_persistence_tests
    ${ATLAS_PERSISTENCE_TEST_SOURCES}
  )
  target_link_libraries(
    atlas_persistence_tests
    PRIVATE
      AtlasLOB::persistence
      GTest::gtest_main
      atlas_persistence_cli_support
  )
  target_include_directories(
    atlas_persistence_tests
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/persistence"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/tools"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/utility"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  )
  target_compile_features(atlas_persistence_tests PRIVATE cxx_std_20)
  set_target_properties(
    atlas_persistence_tests
    PROPERTIES CXX_EXTENSIONS OFF
  )
  atlas_set_project_warnings(atlas_persistence_tests)
  atlas_enable_sanitizers(atlas_persistence_tests)

  gtest_discover_tests(
    atlas_persistence_tests
    TEST_PREFIX "persistence."
    PROPERTIES LABELS "persistence"
  )

  find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME persistence.cli_executable_boundary
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/persistence_cli_boundary_test.py"
        --inspect "$<TARGET_FILE:atlas_inspect>"
        --replay "$<TARGET_FILE:atlas_replay>"
    )
    set_tests_properties(
      persistence.cli_executable_boundary
      PROPERTIES LABELS "persistence;integration"
    )
  endif()

  file(
    GLOB ATLAS_STRESS_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/stress/*_stress_test.cpp"
  )
  add_executable(atlas_stress_tests ${ATLAS_STRESS_TEST_SOURCES})
  target_link_libraries(atlas_stress_tests PRIVATE AtlasLOB::core GTest::gtest_main)
  target_include_directories(
    atlas_stress_tests
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
  )
  target_compile_features(atlas_stress_tests PRIVATE cxx_std_20)
  set_target_properties(atlas_stress_tests PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(atlas_stress_tests)
  atlas_enable_sanitizers(atlas_stress_tests)

  gtest_discover_tests(
    atlas_stress_tests
    TEST_PREFIX "stress."
    PROPERTIES LABELS "stress"
  )

  add_test(NAME cli.validation_demo COMMAND atlas_cli validate-demo)
  set_tests_properties(
    cli.validation_demo
    PROPERTIES
      LABELS "integration"
      PASS_REGULAR_EXPRESSION
        "accepted order_id=1001 instrument_id=1 side=buy type=limit time_in_force=gtc price_ticks=10125 quantity=10"
  )

  add_test(
    NAME cli.domain_fixture_valid
    COMMAND
      atlas_cli domain-fixture
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/domain-valid.commands"
  )
  set_tests_properties(
    cli.domain_fixture_valid
    PROPERTIES
      LABELS "integration"
      PASS_REGULAR_EXPRESSION
        "line=6 REPLACE client_id=11 old_order_id=1001 new_order_id=1003 instrument_id=7 new_price_ticks=10260 new_quantity=20 validation=valid reason=none"
  )

  add_test(
    NAME cli.domain_fixture_invalid
    COMMAND
      atlas_cli domain-fixture
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/fixtures/domain_invalid.commands"
  )
  set_tests_properties(
    cli.domain_fixture_invalid
    PROPERTIES
      LABELS "integration"
      WILL_FAIL TRUE
  )

  add_test(
    NAME cli.domain_fixture_parse_error
    COMMAND
      atlas_cli domain-fixture
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/fixtures/parse_error.commands"
  )
  set_tests_properties(
    cli.domain_fixture_parse_error
    PROPERTIES
      LABELS "integration"
      WILL_FAIL TRUE
  )

  add_test(
    NAME cli.engine_fixture_valid
    COMMAND
      atlas_cli engine-fixture 7
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/engine-demo.commands"
  )
  set_tests_properties(
    cli.engine_fixture_valid
    PROPERTIES
      LABELS "integration"
      PASS_REGULAR_EXPRESSION
        "summary commands=5 committed=5 rejected=0 parse_errors=0 engine_errors=0 last_sequence=5 state_digest=[0-9a-f]+"
  )

  add_test(NAME cli.invalid_usage COMMAND atlas_cli unknown-command)
  set_tests_properties(
    cli.invalid_usage
    PROPERTIES
      LABELS "integration"
      WILL_FAIL TRUE
  )
endif()

file(
  GLOB_RECURSE ATLAS_FORMAT_FILES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp"
)

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
  add_custom_target(
    format
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${ATLAS_FORMAT_FILES}
    COMMENT "Formatting AtlasLOB C++ sources"
    VERBATIM
  )
  add_custom_target(
    format-check
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${ATLAS_FORMAT_FILES}
    COMMENT "Checking AtlasLOB C++ formatting"
    VERBATIM
  )
else()
  add_custom_target(
    format-check
    COMMAND "${CMAKE_COMMAND}" -E echo "clang-format was not found"
    COMMAND "${CMAKE_COMMAND}" -E false
    VERBATIM
  )
endif()
