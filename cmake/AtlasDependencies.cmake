include(FetchContent)

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
