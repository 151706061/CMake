
cmake_minimum_required(VERSION 3.10)

project(CompileFeatures)

set(ext_C c)
set(ext_CXX cpp)
set(std_C 90 99 11 17 23)
set(std_CXX 98 11 14 17 20 23 26)

if(CMake_TEST_CUDA)
  enable_language(CUDA)
  set(ext_CUDA cu)
  set(std_CUDA 03 11 14 17 20 23 26)
endif()

if(CMake_TEST_HIP)
  enable_language(HIP)
  set(ext_HIP hip)
  set(std_HIP 98 11 14 17 20 23 26)
endif()

foreach(lang C CXX CUDA HIP)
  foreach(std IN LISTS std_${lang})
    string(TOLOWER "${lang}_std_${std}" feature)
    if("${std}" IN_LIST CMake_TEST_${lang}_STANDARDS
        OR CMAKE_${lang}${std}_STANDARD_COMPILE_OPTION)
      add_library(test_${feature} OBJECT ${feature}.${ext_${lang}})
      target_compile_features(test_${feature} PRIVATE ${feature})
    endif()
  endforeach()
endforeach()

if(("23" IN_LIST CMake_TEST_CXX_STANDARDS OR CMAKE_CXX23_STANDARD_COMPILE_OPTION)
  AND ("11" IN_LIST CMake_TEST_C_STANDARDS OR CMAKE_C11_STANDARD_COMPILE_OPTION))
  add_library(test_cxx_std_23_with_c_std_11 OBJECT cxx_std_23.cpp c_std_11.c)
  target_compile_features(test_cxx_std_23_with_c_std_11 PRIVATE cxx_std_23 c_std_11)
endif()

if(("23" IN_LIST CMake_TEST_C_STANDARDS OR CMAKE_C23_STANDARD_COMPILE_OPTION)
  AND ("17" IN_LIST CMake_TEST_CXX_STANDARDS OR CMAKE_CXX17_STANDARD_COMPILE_OPTION)
  # FIXME: "clang-cl -stc:clatest" does not enable C23.
  AND NOT CMAKE_GENERATOR MATCHES "Visual Studio"
  )
  add_library(test_c_std_23_with_cxx_std_17 OBJECT c_std_23.c cxx_std_17.cpp)
  target_compile_features(test_c_std_23_with_cxx_std_17 PRIVATE c_std_23 cxx_std_17)
endif()

macro(run_test feature lang)
  if (${feature} IN_LIST CMAKE_${lang}_COMPILE_FEATURES)
    add_library(test_${feature} OBJECT ${feature}.${ext_${lang}})
    set_property(TARGET test_${feature}
      PROPERTY COMPILE_FEATURES "${feature}"
    )
  else()
    list(APPEND ${lang}_non_features ${feature})
  endif()
endmacro()

if(NOT CMAKE_C_COMPILER_ID MATCHES "^(Cray|PGI|NVHPC|XL|XLClang|IBMClang|IntelLLVM|Fujitsu|FujitsuClang|OrangeC)$")
  get_property(c_features GLOBAL PROPERTY CMAKE_C_KNOWN_FEATURES)
  list(FILTER c_features EXCLUDE REGEX "^c_std_[0-9][0-9]")
  foreach(feature ${c_features})
    run_test(${feature} C)
  endforeach()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Cray|PGI|NVHPC|XL|XLClang|IBMClang|IntelLLVM|Fujitsu|FujitsuClang|OrangeC)$")
  get_property(cxx_features GLOBAL PROPERTY CMAKE_CXX_KNOWN_FEATURES)
  list(FILTER cxx_features EXCLUDE REGEX "^cxx_std_[0-9][0-9]")
  foreach(feature ${cxx_features})
    run_test(${feature} CXX)
  endforeach()
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 5.1)
  # AppleClang prior to 5.1 does not set any preprocessor define to distinguish
  # c++1y from c++11, so CMake does not support c++1y features before AppleClang 5.1.
  list(REMOVE_ITEM CXX_non_features
    cxx_attribute_deprecated
    cxx_binary_literals
  )
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.2)
  # AppleClang prior to 4.1 reports false for __has_feature(cxx_local_type_template_args)
  # and __has_feature(cxx_unrestricted_unions) but it happens to pass these tests.
  list(REMOVE_ITEM CXX_non_features
    cxx_local_type_template_args
    cxx_unrestricted_unions
  )
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL SunPro)
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 5.15)
    # SunPro 5.14 accepts -std=c++14 and compiles two features but does
    # not define __cplusplus to a value different than with -std=c++11.
    list(REMOVE_ITEM CXX_non_features
      cxx_aggregate_default_initializers
      cxx_digit_separators
    )
  endif()

  # FIXME: Do any of these work correctly on SunPro 5.13 or above?
  list(REMOVE_ITEM CXX_non_features
    cxx_attribute_deprecated
    cxx_contextual_conversions
    cxx_extended_friend_declarations
    cxx_long_long_type
    cxx_sizeof_member
    cxx_variadic_macros
  )
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.5)
  # The cxx_raw_string_literals feature happens to work in some distributions
  # of GNU 4.4, but it is first documented as available with GNU 4.5.
  list(REMOVE_ITEM CXX_non_features
    cxx_raw_string_literals
  )
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.6)
  # The cxx_constexpr feature happens to work (for *this* testcase) with
  # GNU 4.5, but it is first documented as available with GNU 4.6.
  list(REMOVE_ITEM CXX_non_features
    cxx_constexpr
  )
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.8)
  # The cxx_alignof feature happens to work (for *this* testcase) with
  # GNU 4.7, but it is first documented as available with GNU 4.8.
  list(REMOVE_ITEM CXX_non_features
    cxx_alignof
  )
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.9)
  # GNU prior to 4.9 does not set any preprocessor define to distinguish
  # c++1y from c++11, so CMake does not support c++1y features before GNU 4.9.
  list(REMOVE_ITEM CXX_non_features
    # GNU 4.8 knows cxx_attributes, but doesn't know [[deprecated]]
    # and warns that it is unknown and ignored.
    cxx_attribute_deprecated
    cxx_binary_literals
    cxx_lambda_init_captures
    cxx_return_type_deduction
  )
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18.0)
    list(REMOVE_ITEM CXX_non_features
      # The cxx_contextual_conversions feature happens to work
      # (for *this* testcase) with VS 2010 and VS 2012, but
      # they do not document support until VS 2013.
      cxx_contextual_conversions
      )
  elseif (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.0)
    list(REMOVE_ITEM CXX_non_features
      # The cxx_deleted_functions and cxx_nonstatic_member_init
      # features happen to work (for *this* testcase) with VS 2013,
      # but they do not document support until VS 2015.
      cxx_deleted_functions
      cxx_nonstatic_member_init
      )
  endif()
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
  if (CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"
      AND CMAKE_CXX_SIMULATE_VERSION VERSION_LESS 19.10)
    list(REMOVE_ITEM CXX_non_features
      cxx_relaxed_constexpr
      )
  endif()
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16.0)
    if (CMAKE_CXX_COMIPLER_VERSION VERSION_EQUAL 15.0)
      list(REMOVE_ITEM CXX_non_features
        # The cxx_contextual_conversions feature happens to work
        # (for *this* testcase) with Intel 13/14/15, but they do not
        # document support until 16.
        cxx_contextual_conversions
        )

    elseif (NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14.0)
      list(REMOVE_ITEM CXX_non_features
        cxx_alignof

        # not supposed to work until 15
        cxx_attribute_deprecated

        # The cxx_contextual_conversions feature happens to work
        # (for *this* testcase) with Intel 13/14/15, but they do not
        # document support until 16.
        cxx_contextual_conversions
        )

    elseif (NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.1)
      list(REMOVE_ITEM CXX_non_features
        # These features happen to work but aren't documented to
        # do so until 14.0
        cxx_constexpr
        cxx_enum_forward_declarations
        cxx_sizeof_member
        cxx_strong_enums
        cxx_unicode_literals

        # not supposed to work until 15
        cxx_attribute_deprecated
        cxx_nonstatic_member_init

        # The cxx_contextual_conversions feature happens to work
        # (for *this* testcase) with Intel 13/14/15, but they do not
        # document support until 16.
        cxx_contextual_conversions

        # This is an undocumented feature; it does not work in future versions
        cxx_aggregate_default_initializers
        )
    endif()
  endif()
endif()

if (CMAKE_C_COMPILER_ID STREQUAL "Intel")
  if (CMAKE_C_COMPILER_VERSION VERSION_LESS 15.0.2)
    # This works on some pre-15.0.2 versions and not others.
    list(REMOVE_ITEM C_non_features
      c_static_assert
      )
  endif()
endif()

if (CMAKE_C_COMPILE_FEATURES)
  set(C_expected_features ${CMAKE_C_COMPILE_FEATURES})
  list(FILTER C_expected_features EXCLUDE REGEX "^c_std_[0-9][0-9]")
endif()
if (CMAKE_CXX_COMPILE_FEATURES)
  set(CXX_expected_features ${CMAKE_CXX_COMPILE_FEATURES})
  list(FILTER CXX_expected_features EXCLUDE REGEX "^cxx_std_[0-9][0-9]")
endif ()
set(C_ext c)
set(C_standard_flag 11)
set(CXX_ext cpp)
set(CXX_standard_flag 14)
foreach(lang CXX C)
  if (${lang}_expected_features)
    foreach(feature ${${lang}_non_features})
      message("Testing feature : ${feature}")
      try_compile(${feature}_works
        "${CMAKE_CURRENT_BINARY_DIR}/${feature}_test"
        "${CMAKE_CURRENT_SOURCE_DIR}/feature_test.${${lang}_ext}"
        COMPILE_DEFINITIONS "-DTEST=${feature}.${${lang}_ext}"
        CMAKE_FLAGS "-DCMAKE_${lang}_STANDARD=${${lang}_standard_flag}"
          "-DINCLUDE_DIRECTORIES=${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE OUTPUT
      )
      if (${feature}_works)
        message(SEND_ERROR
          "Feature ${feature} expected not to work for ${lang} ${CMAKE_${lang}_COMPILER_ID}-${CMAKE_${lang}_COMPILER_VERSION}.
  Update the supported features or blacklist it.\n${OUTPUT}")
      else()
        message("Testing feature : ${feature} -- Fails, as expected.")
      endif()
    endforeach()
  endif()
endforeach()

if (C_expected_features)
  if (CMAKE_C_STANDARD_DEFAULT)
    string(FIND "${CMAKE_C_FLAGS}" "-std=" std_flag_idx)
    if (std_flag_idx EQUAL -1)
      add_executable(default_dialect_C default_dialect.c)
      target_compile_definitions(default_dialect_C PRIVATE
        DEFAULT_C23=$<EQUAL:${CMAKE_C_STANDARD_DEFAULT},23>
        DEFAULT_C17=$<EQUAL:${CMAKE_C_STANDARD_DEFAULT},17>
        DEFAULT_C11=$<EQUAL:${CMAKE_C_STANDARD_DEFAULT},11>
        DEFAULT_C99=$<EQUAL:${CMAKE_C_STANDARD_DEFAULT},99>
        DEFAULT_C90=$<EQUAL:${CMAKE_C_STANDARD_DEFAULT},90>
      )
    endif()
  endif()

  add_executable(CompileFeaturesGenex_C genex_test.c)
  set_property(TARGET CompileFeaturesGenex_C PROPERTY C_STANDARD 11)

  foreach(f
      c_restrict
      c_static_assert
      c_function_prototypes
      )
    if(${f} IN_LIST C_expected_features)
      set(expect_${f} 1)
    else()
      set(expect_${f} 0)
    endif()
    string(TOUPPER "${f}" F)
    target_compile_definitions(CompileFeaturesGenex_C PRIVATE
      EXPECT_${F}=${expect_${f}}
      HAVE_${F}=$<COMPILE_FEATURES:${f}>
      )
  endforeach()
endif()

if (CMAKE_CXX_COMPILE_FEATURES)
  if (CMAKE_CXX_STANDARD_DEFAULT)
    string(FIND "${CMAKE_CXX_FLAGS}" "-std=" std_flag_idx)
    if (std_flag_idx EQUAL -1)
      add_executable(default_dialect default_dialect.cpp)
      target_compile_definitions(default_dialect PRIVATE
        DEFAULT_CXX26=$<EQUAL:${CMAKE_CXX_STANDARD_DEFAULT},26>
        DEFAULT_CXX23=$<EQUAL:${CMAKE_CXX_STANDARD_DEFAULT},23>
        DEFAULT_CXX20=$<EQUAL:${CMAKE_CXX_STANDARD_DEFAULT},20>
        DEFAULT_CXX17=$<EQUAL:${CMAKE_CXX_STANDARD_DEFAULT},17>
        DEFAULT_CXX14=$<EQUAL:${CMAKE_CXX_STANDARD_DEFAULT},14>
        DEFAULT_CXX11=$<EQUAL:${CMAKE_CXX_STANDARD_DEFAULT},11>
        DEFAULT_CXX98=$<EQUAL:${CMAKE_CXX_STANDARD_DEFAULT},98>
      )
    endif()
  endif()
endif ()

if (CMake_TEST_CUDA
    AND CMAKE_CUDA_COMPILE_FEATURES
    AND CMAKE_CUDA_STANDARD_DEFAULT
    AND NOT CMAKE_CUDA_FLAGS MATCHES "-std=")
  add_executable(default_dialect_cuda default_dialect.cu)
  target_compile_definitions(default_dialect_cuda PRIVATE
    DEFAULT_CXX26=$<EQUAL:${CMAKE_CUDA_STANDARD_DEFAULT},26>
    DEFAULT_CXX23=$<EQUAL:${CMAKE_CUDA_STANDARD_DEFAULT},23>
    DEFAULT_CXX20=$<EQUAL:${CMAKE_CUDA_STANDARD_DEFAULT},20>
    DEFAULT_CXX17=$<EQUAL:${CMAKE_CUDA_STANDARD_DEFAULT},17>
    DEFAULT_CXX14=$<EQUAL:${CMAKE_CUDA_STANDARD_DEFAULT},14>
    DEFAULT_CXX11=$<EQUAL:${CMAKE_CUDA_STANDARD_DEFAULT},11>
    DEFAULT_CXX98=$<EQUAL:${CMAKE_CUDA_STANDARD_DEFAULT},03>
  )
endif ()

# always add a target "CompileFeatures"
if ((NOT CXX_expected_features) OR
    (NOT cxx_auto_type IN_LIST CXX_expected_features))
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dummy.cpp"
    "int main(int,char**) { return 0; }\n"
  )
  add_executable(CompileFeatures "${CMAKE_CURRENT_BINARY_DIR}/dummy.cpp")
else()
  # these tests only work if at least cxx_auto_type is available
  add_executable(CompileFeatures main.cpp)
  set_property(TARGET CompileFeatures
    PROPERTY COMPILE_FEATURES "cxx_auto_type"
  )
  set_property(TARGET CompileFeatures
    PROPERTY CXX_STANDARD_REQUIRED TRUE
  )

  add_executable(GenexCompileFeatures main.cpp)
  set_property(TARGET GenexCompileFeatures
    PROPERTY COMPILE_FEATURES "$<1:cxx_auto_type>;$<0:not_a_feature>"
  )

  add_library(iface INTERFACE)
  set_property(TARGET iface
    PROPERTY INTERFACE_COMPILE_FEATURES "cxx_auto_type"
  )
  add_executable(IfaceCompileFeatures main.cpp)
  target_link_libraries(IfaceCompileFeatures iface)

  foreach(f
      cxx_final
      cxx_override
      cxx_auto_type
      cxx_inheriting_constructors
      )
    if(${f} IN_LIST CXX_expected_features)
      set(expect_${f} 1)
    else()
      set(expect_${f} 0)
    endif()
  endforeach()

  if(expect_cxx_final AND expect_cxx_override)
    set(expect_override_control 1)
  else()
    set(expect_override_control 0)
  endif()
  if(expect_cxx_inheriting_constructors AND expect_cxx_final)
    set(expect_inheriting_constructors_and_final 1)
  else()
    set(expect_inheriting_constructors_and_final 0)
  endif()

  set(genex_test_defs
    HAVE_OVERRIDE_CONTROL=$<COMPILE_FEATURES:cxx_final,cxx_override>
    HAVE_AUTO_TYPE=$<COMPILE_FEATURES:cxx_auto_type>
    HAVE_INHERITING_CONSTRUCTORS=$<COMPILE_FEATURES:cxx_inheriting_constructors>
    HAVE_FINAL=$<COMPILE_FEATURES:cxx_final>
    HAVE_INHERITING_CONSTRUCTORS_AND_FINAL=$<COMPILE_FEATURES:cxx_inheriting_constructors,cxx_final>
    EXPECT_OVERRIDE_CONTROL=${expect_override_control}
    EXPECT_INHERITING_CONSTRUCTORS=${expect_cxx_inheriting_constructors}
    EXPECT_FINAL=${expect_cxx_final}
    EXPECT_INHERITING_CONSTRUCTORS_AND_FINAL=${expect_inheriting_constructors_and_final}
    )
  if (CMAKE_CXX_STANDARD_DEFAULT)
    list(APPEND genex_test_defs
      TEST_CXX_STD
      HAVE_CXX_STD_11=$<COMPILE_FEATURES:cxx_std_11>
      HAVE_CXX_STD_14=$<COMPILE_FEATURES:cxx_std_14>
      HAVE_CXX_STD_17=$<COMPILE_FEATURES:cxx_std_17>
      HAVE_CXX_STD_20=$<COMPILE_FEATURES:cxx_std_20>
      HAVE_CXX_STD_23=$<COMPILE_FEATURES:cxx_std_23>
      HAVE_CXX_STD_26=$<COMPILE_FEATURES:cxx_std_26>
    )
  endif()

  add_executable(CompileFeaturesGenex genex_test.cpp)
  set_property(TARGET CompileFeaturesGenex PROPERTY CXX_STANDARD 11)
  target_compile_definitions(CompileFeaturesGenex PRIVATE ${genex_test_defs})

  add_executable(CompileFeaturesGenex2 genex_test.cpp)
  target_compile_features(CompileFeaturesGenex2 PRIVATE cxx_std_11)
  target_compile_definitions(CompileFeaturesGenex2 PRIVATE ${genex_test_defs} ALLOW_LATER_STANDARDS=1)

  add_library(std_11_iface INTERFACE)
  target_compile_features(std_11_iface INTERFACE cxx_std_11)
  add_executable(CompileFeaturesGenex3 genex_test.cpp)
  target_link_libraries(CompileFeaturesGenex3 PRIVATE std_11_iface)
  target_compile_definitions(CompileFeaturesGenex3 PRIVATE ${genex_test_defs} ALLOW_LATER_STANDARDS=1)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
    AND (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.30
      # The MSVC 14.29.30133 toolset supports C++20,
      # but MSBuild puts the flags in the wrong order.
      OR (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.29.30129 AND NOT CMAKE_GENERATOR MATCHES "Visual Studio")
      )
    )
  add_library(msvc_permissive msvc_permissive.cxx)
  target_compile_features(msvc_permissive PRIVATE cxx_std_20)
  # The `-std:c++20` flag implies `-permissive-`.  Test passing `-permissive` afterward.
  target_compile_options(msvc_permissive PRIVATE -permissive)
endif()
