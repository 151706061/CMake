# This test will verify if CheckSymbolExists only report symbols available
# for linking that really are. You can find some documentation on this in
# bug 11333 where we found out that gcc would optimize out the actual
# reference to the symbol, so symbols that are in fact _not_ available in the
# given libraries (but seen in header) were reported as present.
#
# If you change this test do not forget to change the CheckCXXSymbolExists
# test, too.

cmake_minimum_required(VERSION 3.10)
project(CheckSymbolExists C)

set(CMAKE_REQUIRED_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}")

include(CheckSymbolExists)

foreach(_config_type Release RelWithDebInfo MinSizeRel Debug)
  set(CMAKE_TRY_COMPILE_CONFIGURATION ${_config_type})
  unset(CSE_RESULT_${_config_type} CACHE)
  message(STATUS "Testing configuration ${_config_type}")

  check_symbol_exists(non_existent_function_for_symbol_test "cm_cse.h" CSE_RESULT_${_config_type})

  if (CSE_RESULT_${_config_type})
    message(SEND_ERROR "CheckSymbolExists reported a nonexistent symbol as existing in configuration ${_config_type}")
  endif ()
endforeach()

set(CMAKE_TRY_COMPILE_CONFIGURATION ${CMAKE_BUILD_TYPE})
unset(CSE_RESULT_ERRNO CACHE)

check_symbol_exists(errno "errno.h" CSE_RESULT_ERRNO)

if (NOT CSE_RESULT_ERRNO)
  message(SEND_ERROR "CheckSymbolExists did not find errno in <errno.h>")
else ()
  message(STATUS "errno found as expected")
endif ()

if (CMAKE_C_COMPILER_ID MATCHES "^(GNU|LCC)$")
  string(APPEND CMAKE_C_FLAGS " -O3")
  unset(CSE_RESULT_O3 CACHE)
  message(STATUS "Testing with optimization -O3")

  check_symbol_exists(non_existent_function_for_symbol_test "cm_cse.h" CSE_RESULT_O3)

  if (CSE_RESULT_O3)
    message(SEND_ERROR "CheckSymbolExists reported a nonexistent symbol as existing with optimization -O3")
  endif ()

  string(APPEND CMAKE_C_FLAGS " -pedantic-errors")
  unset(CS_RESULT_PEDANTIC_ERRORS CACHE)
  message(STATUS "Testing with -pedantic-errors")

  check_symbol_exists(fopen "stdio.h" CSE_RESULT_PEDANTIC_ERRORS)

  if(NOT CSE_RESULT_PEDANTIC_ERRORS)
    message(SEND_ERROR "CheckSymbolExists reported an existing symbol as nonexisting with -pedantic-errors")
  endif()

endif ()
