cmake_minimum_required(VERSION 3.25)

project(
  AtlasLOB
  VERSION 0.1.0
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
      include/atlaslob/domain/events.hpp
      include/atlaslob/domain/types.hpp
      include/atlaslob/domain/validation.hpp
)

target_compile_features(atlas_domain PUBLIC cxx_std_20)
set_target_properties(atlas_domain PROPERTIES CXX_EXTENSIONS OFF)
atlas_set_project_warnings(atlas_domain)
atlas_enable_sanitizers(atlas_domain)

add_library(atlas_core)
add_library(AtlasLOB::core ALIAS atlas_core)

set(
  ATLAS_CORE_SOURCES
  src/core/active_order_index.cpp
  src/core/active_order_index.hpp
  src/core/book_side.cpp
  src/core/book_side.hpp
  src/core/checked_arithmetic.hpp
  src/core/instrument_book.cpp
  src/core/instrument_book.hpp
  src/core/order_node.hpp
  src/core/order_storage.cpp
  src/core/order_storage.hpp
  src/core/price_level.cpp
  src/core/price_level.hpp
)

function(atlas_configure_core target_name)
  target_sources(${target_name} PRIVATE ${ATLAS_CORE_SOURCES})
  target_link_libraries(${target_name} PUBLIC AtlasLOB::domain)
  target_include_directories(${target_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
  target_compile_features(${target_name} PUBLIC cxx_std_20)
  target_compile_definitions(
    ${target_name}
    PUBLIC ATLAS_ENABLE_INVARIANTS=$<BOOL:${ATLAS_ENABLE_INVARIANTS}>
  )
  set_target_properties(${target_name} PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(${target_name})
  atlas_enable_sanitizers(${target_name})
endfunction()

atlas_configure_core(atlas_core)

add_library(
  atlas_cli_support
  src/cli/domain_fixture.cpp
  src/cli/domain_fixture.hpp
  src/cli/domain_fixture_formatter.cpp
  src/cli/domain_fixture_formatter.hpp
  src/cli/domain_fixture_parser.cpp
  src/cli/domain_fixture_parser.hpp
)
target_link_libraries(atlas_cli_support PUBLIC AtlasLOB::domain)
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

if(BUILD_TESTING)
  include(AtlasDependencies)
  atlas_setup_test_dependencies()
  include(GoogleTest)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/domain_fixture*_test.cpp"
  )
  add_executable(atlas_fixture_tests ${ATLAS_FIXTURE_TEST_SOURCES})
  target_link_libraries(
    atlas_fixture_tests
    PRIVATE atlas_cli_support GTest::gtest_main
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
