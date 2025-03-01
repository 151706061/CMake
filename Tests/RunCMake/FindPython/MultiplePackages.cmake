enable_language(C)

include(CTest)

find_package (Python REQUIRED)

if (CMake_TEST_FindPython2)
  find_package (Python2 REQUIRED COMPONENTS Interpreter Development)

  if (NOT CMake_TEST_FindPython3 AND NOT Python2_EXECUTABLE STREQUAL Python_EXECUTABLE)
    message (FATAL_ERROR
      "Python interpreters do not match:\n"
      "  Python_EXECUTABLE='${Python_EXECUTABLE}'\n"
      "  Python2_EXECUTABLE='${Python3_EXECUTABLE}'\n"
    )
  endif()

  Python2_add_library (spam2 MODULE spam.c)
  target_compile_definitions (spam2 PRIVATE PYTHON2)

  add_test (NAME python2_spam2
            COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=$<TARGET_FILE_DIR:spam2>"
            "${Python2_INTERPRETER}" -c "import spam2; spam2.system(\"cd\")")

endif()

if (CMake_TEST_FindPython3)
  find_package (Python3 REQUIRED COMPONENTS Interpreter Development)

  if (NOT Python3_EXECUTABLE STREQUAL Python_EXECUTABLE)
    message (FATAL_ERROR
      "Python interpreters do not match:\n"
      "  Python_EXECUTABLE='${Python_EXECUTABLE}'\n"
      "  Python3_EXECUTABLE='${Python3_EXECUTABLE}'\n"
    )
  endif()

  Python3_add_library (spam3 MODULE spam.c)
  target_compile_definitions (spam3 PRIVATE PYTHON3)

  add_test (NAME python3_spam3
            COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=$<TARGET_FILE_DIR:spam3>"
            "${Python3_INTERPRETER}" -c "import spam3; spam3.system(\"cd\")")

endif()
