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
      include/atlaslob/domain/types.hpp
      include/atlaslob/domain/validation.hpp
)

target_compile_features(atlas_domain PUBLIC cxx_std_20)
set_target_properties(atlas_domain PROPERTIES CXX_EXTENSIONS OFF)
atlas_set_project_warnings(atlas_domain)
atlas_enable_sanitizers(atlas_domain)

add_executable(atlas_cli src/cli/main.cpp)
target_link_libraries(atlas_cli PRIVATE AtlasLOB::domain)
target_compile_features(atlas_cli PRIVATE cxx_std_20)
set_target_properties(atlas_cli PROPERTIES CXX_EXTENSIONS OFF)
atlas_set_project_warnings(atlas_cli)
atlas_enable_sanitizers(atlas_cli)

if(BUILD_TESTING)
  add_executable(atlas_domain_tests tests/unit/domain_validation_test.cpp)
  target_link_libraries(atlas_domain_tests PRIVATE AtlasLOB::domain)
  target_compile_features(atlas_domain_tests PRIVATE cxx_std_20)
  set_target_properties(atlas_domain_tests PROPERTIES CXX_EXTENSIONS OFF)
  atlas_set_project_warnings(atlas_domain_tests)
  atlas_enable_sanitizers(atlas_domain_tests)

  add_test(NAME domain.validation COMMAND atlas_domain_tests)
  set_tests_properties(domain.validation PROPERTIES LABELS "unit")

  add_test(NAME cli.validation_demo COMMAND atlas_cli validate-demo)
  set_tests_properties(
    cli.validation_demo
    PROPERTIES
      LABELS "integration"
      PASS_REGULAR_EXPRESSION
        "accepted order_id=1001 instrument_id=1 side=buy type=limit time_in_force=gtc price_ticks=10125 quantity=10"
  )

  add_test(NAME cli.invalid_usage COMMAND atlas_cli unknown-command)
  set_tests_properties(
    cli.invalid_usage
    PROPERTIES
      LABELS "integration"
      WILL_FAIL TRUE
  )
endif()

set(
  ATLAS_FORMAT_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/include/atlaslob/domain/commands.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/atlaslob/domain/types.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/atlaslob/domain/validation.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/domain/validation.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/domain_validation_test.cpp"
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
