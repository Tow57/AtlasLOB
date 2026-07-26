include(FetchContent)

function(atlas_setup_benchmark_dependencies)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Do not build Google Benchmark tests" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "Do not install Google Benchmark" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "Do not build Google Benchmark GTest tests" FORCE)
  set(BENCHMARK_ENABLE_LTO OFF CACHE BOOL "Do not enable dependency LTO" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "Do not apply dependency warnings as errors" FORCE)
  set(BENCHMARK_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "Do not download transitive benchmark dependencies" FORCE)

  FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG 192ef10025eb2c4cdd392bc502f0c852196baa48
    GIT_PROGRESS TRUE
  )
  FetchContent_MakeAvailable(googlebenchmark)
endfunction()

function(atlas_setup_test_dependencies)
  set(INSTALL_GTEST OFF CACHE BOOL "Do not install AtlasLOB test dependencies" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "AtlasLOB does not use GoogleMock" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "Use the shared CRT with GoogleTest on Windows" FORCE)

  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
    GIT_PROGRESS TRUE
  )
  FetchContent_MakeAvailable(googletest)
endfunction()
