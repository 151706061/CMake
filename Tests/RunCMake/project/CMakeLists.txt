if("x${RunCMake_TEST}" STREQUAL "xNoMinimumRequired")
  # No cmake_minimum_required(VERSION)
else()
  cmake_minimum_required(VERSION 3.10)
  # CMP0180 needs to be set before the project() call for these tests
  if("x${RunCMake_TEST}" STREQUAL "xCMP0180-NEW")
    cmake_policy(SET CMP0180 NEW)
  elseif("x${RunCMake_TEST}" STREQUAL "xCMP0180-OLD")
    cmake_policy(SET CMP0180 OLD)
  endif()
endif()
project(${RunCMake_TEST} NONE)
include(${RunCMake_TEST}.cmake)
