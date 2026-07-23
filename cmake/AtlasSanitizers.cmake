function(atlas_enable_sanitizers target_name)
  if(ATLAS_ENABLE_TSAN AND (ATLAS_ENABLE_ASAN OR ATLAS_ENABLE_UBSAN))
    message(FATAL_ERROR "ThreadSanitizer must not be combined with ASan or UBSan")
  endif()

  if(NOT (ATLAS_ENABLE_ASAN OR ATLAS_ENABLE_UBSAN OR ATLAS_ENABLE_TSAN))
    return()
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "The selected sanitizers require GCC or Clang")
  endif()

  set(enabled_sanitizers)
  if(ATLAS_ENABLE_ASAN)
    list(APPEND enabled_sanitizers address)
  endif()
  if(ATLAS_ENABLE_UBSAN)
    list(APPEND enabled_sanitizers undefined)
  endif()
  if(ATLAS_ENABLE_TSAN)
    list(APPEND enabled_sanitizers thread)
  endif()

  list(JOIN enabled_sanitizers "," sanitizer_set)
  target_compile_options(
    ${target_name}
    PRIVATE "-fsanitize=${sanitizer_set}" -fno-omit-frame-pointer
  )
  target_link_options(${target_name} PRIVATE "-fsanitize=${sanitizer_set}")
endfunction()
