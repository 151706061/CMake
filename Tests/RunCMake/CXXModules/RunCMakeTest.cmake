include(RunCMake)

run_cmake(Inspect)
include("${RunCMake_BINARY_DIR}/Inspect-build/info.cmake")

# Test negative cases where C++20 modules do not work.
run_cmake(NoCXX)
if ("cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
  # This test requires that the compiler be told to compile in an older-than-20
  # standard. If the compiler forces a standard to be used, skip it.
  if (NOT forced_cxx_standard)
    run_cmake(NoCXX20)
    if(CMAKE_CXX_STANDARD_DEFAULT AND CMAKE_CXX20_STANDARD_COMPILE_OPTION)
      run_cmake_with_options(ImplicitCXX20 -DCMAKE_CXX20_STANDARD_COMPILE_OPTION=${CMAKE_CXX20_STANDARD_COMPILE_OPTION})
    endif()
  endif ()

  run_cmake(NoScanningSourceFileProperty)
  run_cmake(NoScanningTargetProperty)
  run_cmake(NoScanningVariable)
  run_cmake(CMP0155-OLD)
  run_cmake(CMP0155-NEW)
  run_cmake(CMP0155-NEW-with-rule)
endif ()

if (RunCMake_GENERATOR MATCHES "Ninja")
  execute_process(
    COMMAND "${CMAKE_MAKE_PROGRAM}" --version
    RESULT_VARIABLE res
    OUTPUT_VARIABLE ninja_version
    ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)

  if (res)
    message(WARNING
      "Failed to determine `ninja` version: ${err}")
    set(ninja_version "0")
  endif ()
endif ()

set(generator_supports_cxx_modules 0)
if (RunCMake_GENERATOR MATCHES "Ninja" AND
    ninja_version VERSION_GREATER_EQUAL "1.11" AND
    "cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
  set(generator_supports_cxx_modules 1)
endif ()

if (RunCMake_GENERATOR MATCHES "Visual Studio" AND
    CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "19.34")
  set(generator_supports_cxx_modules 1)
endif ()

# Test behavior when the generator does not support C++20 modules.
if (NOT generator_supports_cxx_modules)
  if ("cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    run_cmake(NoDyndepSupport)
  endif ()

  # Bail; the remaining tests require the generator to successfully generate
  # with C++20 modules in the source list.
  return ()
endif ()

set(fileset_types
  Modules)
set(scopes
  Interface
  Private
  Public)
set(target_types
  Interface
  Static
  )
foreach (fileset_type IN LISTS fileset_types)
  foreach (scope IN LISTS scopes)
    foreach (target_type IN LISTS target_types)
      run_cmake("FileSet${fileset_type}${scope}On${target_type}")
    endforeach ()
  endforeach ()
  run_cmake("FileSet${fileset_type}InterfaceImported")

  # Test the error messages when a non-C++ source file is found in the source
  # list.
  run_cmake("NotCompiledSource${fileset_type}")
  run_cmake("NotCXXSource${fileset_type}")
endforeach ()

if ("cxx_std_23" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
  run_cmake(CXXImportStdConfig)
  run_cmake(CXXImportStdHeadTarget)
  run_cmake(CXXImportStdLinkLanguage)
  run_cmake(CXXImportStdInvalidGenex)
endif ()

if ("cxx_std_23" IN_LIST CMAKE_CXX_COMPILE_FEATURES AND
    NOT have_cxx23_import_std)
  run_cmake(NoCXX23TargetUnset)
  run_cmake(NoCXX23TargetNotRequired)
  run_cmake(NoCXX23TargetRequired)
endif ()

if ("cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES AND
    NOT have_cxx26_import_std)
  run_cmake(NoCXX26TargetUnset)
  run_cmake(NoCXX26TargetNotRequired)
  run_cmake(NoCXX26TargetRequired)
endif ()

run_cmake(InstallBMI)
run_cmake(InstallBMIGenericArgs)
run_cmake(InstallBMIIgnore)

run_cmake(ExportBuildCxxModules)
run_cmake(ExportBuildCxxModulesTargets)
run_cmake(ExportInstallCxxModules)

# Generator-specific tests.
if (RunCMake_GENERATOR MATCHES "Ninja")
  run_cmake(NinjaDependInfoFileSet)
  run_cmake(NinjaDependInfoExport)
  run_cmake(NinjaDependInfoExportFilesystemSafe)
  run_cmake(NinjaDependInfoBMIInstall)
  run_cmake(NinjaForceResponseFile) # issue#25367
  run_cmake(NinjaDependInfoCompileDatabase)
elseif (RunCMake_GENERATOR MATCHES "Visual Studio")
  run_cmake(VisualStudioNoSyntheticTargets)
else ()
  message(FATAL_ERROR
    "Please add 'DependInfo' tests for the '${RunCMake_GENERATOR}' generator.")
endif ()

# Actual compilation tests.
if (NOT CMake_TEST_MODULE_COMPILATION)
  return ()
endif ()

function (run_cxx_module_test directory)
  set(test_name "${directory}")
  if (NOT ARGN STREQUAL "")
    list(POP_FRONT ARGN test_name)
  endif ()

  set(RunCMake_TEST_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/examples/${directory}")
  set(RunCMake_TEST_BINARY_DIR "${RunCMake_BINARY_DIR}/examples/${test_name}-build")

  if (RunCMake_GENERATOR_IS_MULTI_CONFIG)
    set(RunCMake_TEST_OPTIONS -DCMAKE_CONFIGURATION_TYPES=Debug)
  else ()
    set(RunCMake_TEST_OPTIONS -DCMAKE_BUILD_TYPE=Debug)
  endif ()

  if (RunCMake_CXXModules_INSTALL)
    set(prefix "${RunCMake_BINARY_DIR}/examples/${test_name}-install")
    file(REMOVE_RECURSE "${prefix}")
    list(APPEND RunCMake_TEST_OPTIONS
      "-DCMAKE_INSTALL_PREFIX=${prefix}")
  endif ()

  list(APPEND RunCMake_TEST_OPTIONS
    "-DCMake_TEST_MODULE_COMPILATION_RULES=${CMake_TEST_MODULE_COMPILATION_RULES}"
    ${ARGN})
  run_cmake("examples/${test_name}")
  set(RunCMake_TEST_NO_CLEAN 1)
  if (RunCMake_CXXModules_TARGET)
    run_cmake_command("examples/${test_name}-build" "${CMAKE_COMMAND}" --build . --config Debug --target "${RunCMake_CXXModules_TARGET}")
  else ()
    run_cmake_command("examples/${test_name}-build" "${CMAKE_COMMAND}" --build . --config Debug)
    foreach (RunCMake_CXXModules_TARGET IN LISTS RunCMake_CXXModules_TARGETS)
      set(RunCMake_CXXModules_CONFIG "Debug")
      set(RunCMake_CXXModules_NAME_SUFFIX "")
      if (RunCMake_CXXModules_TARGET MATCHES "(.*)@(.*)")
        set(RunCMake_CXXModules_TARGET "${CMAKE_MATCH_1}")
        set(RunCMake_CXXModules_CONFIG "${CMAKE_MATCH_2}")
        set(RunCMake_CXXModules_NAME_SUFFIX "-${RunCMake_CXXModules_CONFIG}")
      endif ()
      run_cmake_command("examples/${test_name}-target-${RunCMake_CXXModules_TARGET}${RunCMake_CXXModules_NAME_SUFFIX}" "${CMAKE_COMMAND}" --build . --target "${RunCMake_CXXModules_TARGET}" --config "${RunCMake_CXXModules_CONFIG}")
    endforeach ()
  endif ()
  if (RunCMake_CXXModules_INSTALL)
    run_cmake_command("examples/${test_name}-install" "${CMAKE_COMMAND}" --build . --target install --config Debug)
  endif ()
  if (NOT RunCMake_CXXModules_NO_TEST)
    run_cmake_command("examples/${test_name}-test" "${CMAKE_CTEST_COMMAND}" -C Debug --output-on-failure)
  endif ()
  if (RunCMake_CXXModules_REBUILD)
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.125) # handle 1s resolution
    include("${RunCMake_TEST_SOURCE_DIR}/pre-rebuild.cmake")
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.125) # handle 1s resolution
    run_cmake_command("examples/${test_name}-rebuild" "${CMAKE_COMMAND}" --build . --config Debug)
  endif ()
endfunction ()

function (run_cxx_module_test_target directory target)
  set(RunCMake_CXXModules_TARGET "${target}")
  set(RunCMake_CXXModules_NO_TEST 1)
  run_cxx_module_test("${directory}" ${ARGN})
endfunction ()

function (run_cxx_module_test_rebuild directory)
  set(RunCMake_CXXModules_INSTALL 0)
  set(RunCMake_CXXModules_NO_TEST 1)
  set(RunCMake_CXXModules_REBUILD 1)
  run_cxx_module_test("${directory}" ${ARGN})
endfunction ()

# Module compilation features:
# Compiler-based:
# - `named`: basic support for named modules is available
# - `shared`: shared libraries are supported
# - `partitions`: module partitions are supported
# - `internal_partitions`: internal module partitions are supported
# - `bmionly`: the compiler supports BMI-only builds
# - `import_std23`: the compiler supports `import std` for C++23
#
# Generator-based:
# - `compile_commands`: the generator supports `compile_commands.json`
# - `collation`: the generator supports module collation features
# - `export_bmi`: the generator supports exporting BMIs
# - `ninja`: a `ninja` binary is available to perform `Ninja`-only testing
#   (assumed if the generator matches `Ninja`).
string(REPLACE "," ";" CMake_TEST_MODULE_COMPILATION "${CMake_TEST_MODULE_COMPILATION}")
if (RunCMake_GENERATOR MATCHES "Ninja")
  list(APPEND CMake_TEST_MODULE_COMPILATION
    "ninja")
endif ()

if (RunCMake_GENERATOR MATCHES "Ninja")
  if (RunCMake_GENERATOR_IS_MULTI_CONFIG)
    set(ninja_cmp0154_target "CMakeFiles/ninja_cmp0154.dir/Debug/unrelated.cxx${CMAKE_CXX_OUTPUT_EXTENSION}")
  else ()
    set(ninja_cmp0154_target "CMakeFiles/ninja_cmp0154.dir/unrelated.cxx${CMAKE_CXX_OUTPUT_EXTENSION}")
  endif ()
  run_cxx_module_test_target(ninja-cmp0154 "${ninja_cmp0154_target}")
endif ()

run_cxx_module_test(scan-with-pch)

# Tests which use named modules.
if ("named" IN_LIST CMake_TEST_MODULE_COMPILATION)
  run_cxx_module_test(simple)
  run_cxx_module_test(file-sets-with-dot)
  run_cxx_module_test(vs-without-flags)
  run_cxx_module_test(library library-static -DBUILD_SHARED_LIBS=OFF)
  run_cxx_module_test(unity-build)
  run_cxx_module_test(object-library)
  run_cxx_module_test(generated)
  run_cxx_module_test(deep-chain)
  run_cxx_module_test(non-trivial-collation-order)
  run_cxx_module_test(non-trivial-collation-order-randomized)
  run_cxx_module_test(duplicate)
  set(RunCMake_CXXModules_NO_TEST 1)
  run_cxx_module_test(import-from-object)
  run_cxx_module_test(circular)
  run_cxx_module_test(try-compile)
  run_cxx_module_test(try-run)
  unset(RunCMake_CXXModules_NO_TEST)
  run_cxx_module_test(same-src-name)
  run_cxx_module_test(scan_properties)
  run_cxx_module_test(target-objects)

  if ("cxx_std_23" IN_LIST CMAKE_CXX_COMPILE_FEATURES AND
      "import_std23" IN_LIST CMake_TEST_MODULE_COMPILATION)
    run_cxx_module_test(import-std)
    set(RunCMake_CXXModules_NO_TEST 1)
    run_cxx_module_test(import-std-no-std-property)
    unset(RunCMake_CXXModules_NO_TEST)
    run_cxx_module_test(import-std-export-no-std-build)
    set(RunCMake_CXXModules_INSTALL 1)
    run_cxx_module_test(import-std-export-no-std-install)
    unset(RunCMake_CXXModules_INSTALL)

    if ("collation" IN_LIST CMake_TEST_MODULE_COMPILATION)
      run_cxx_module_test(import-std-not-in-export-build)
      run_cxx_module_test(import-std-transitive import-std-transitive-not-in-export-build "-DCMAKE_PREFIX_PATH=${RunCMake_BINARY_DIR}/examples/import-std-not-in-export-build-build")

      set(RunCMake_CXXModules_INSTALL 1)
      run_cxx_module_test(import-std-not-in-export-install)
      unset(RunCMake_CXXModules_INSTALL)
      run_cxx_module_test(import-std-transitive import-std-transitive-not-in-export-install "-DCMAKE_PREFIX_PATH=${RunCMake_BINARY_DIR}/examples/import-std-not-in-export-install-install")

      run_cxx_module_test(import-std-transitive import-std-transitive-export-no-std-build "-DCMAKE_PREFIX_PATH=${RunCMake_BINARY_DIR}/examples/import-std-export-no-std-build-build" -DEXPORT_NO_STD=1)
      run_cxx_module_test(import-std-transitive import-std-transitive-export-no-std-install "-DCMAKE_PREFIX_PATH=${RunCMake_BINARY_DIR}/examples/import-std-export-no-std-install-install" -DEXPORT_NO_STD=1)
    endif ()
  endif ()
endif ()

# Tests which require compile commands support.
if ("compile_commands" IN_LIST CMake_TEST_MODULE_COMPILATION)
  run_cxx_module_test(export-compile-commands)
endif ()

macro (setup_export_build_database_targets)
  set(RunCMake_CXXModules_TARGETS
    cmake_build_database-CXX
    cmake_build_database)

  if (RunCMake_GENERATOR_IS_MULTI_CONFIG)
    list(INSERT RunCMake_CXXModules_TARGETS 0
      cmake_build_database-CXX-Debug
      cmake_build_database-Debug
      # Other config targets.
      cmake_build_database-CXX-Release@Release
      cmake_build_database-Release@Release)
  endif ()
endmacro ()

# Tests which require build database support.
if ("build_database" IN_LIST CMake_TEST_MODULE_COMPILATION)
  setup_export_build_database_targets()
  set(RunCMake_CXXModules_NO_TEST 1)

  run_cxx_module_test(export-build-database)

  unset(RunCMake_CXXModules_NO_TEST)
  unset(RunCMake_CXXModules_TARGETS)
endif ()

# Tests which require collation work.
if ("collation" IN_LIST CMake_TEST_MODULE_COMPILATION)
  run_cxx_module_test(duplicate-sources)
  run_cxx_module_test(public-req-private)
  set(RunCMake_CXXModules_NO_TEST 1)
  run_cxx_module_test(req-private-other-target)
  unset(RunCMake_CXXModules_NO_TEST)
  run_cxx_module_test_rebuild(depchain-modmap)
  run_cxx_module_test_rebuild(depchain-modules-json-file)
  if (RunCMake_GENERATOR MATCHES "Ninja")
    run_cxx_module_test_rebuild(depchain-collation-restat)
  endif ()
endif ()

# Tests which use named modules in shared libraries.
if ("shared" IN_LIST CMake_TEST_MODULE_COMPILATION)
  run_cxx_module_test(library library-shared -DBUILD_SHARED_LIBS=ON)
endif ()

# Tests which use partitions.
if ("partitions" IN_LIST CMake_TEST_MODULE_COMPILATION)
  run_cxx_module_test(partitions)
endif ()

# Tests which use internal partitions.
if ("internal_partitions" IN_LIST CMake_TEST_MODULE_COMPILATION)
  run_cxx_module_test(internal-partitions)
endif ()

function (run_cxx_module_import_test type name)
  set(RunCMake_CXXModules_INSTALL 0)

  if ("EXPORT_BUILD_DATABASE" IN_LIST ARGN)
    list(REMOVE_ITEM ARGN EXPORT_BUILD_DATABASE)
    list(APPEND ARGN -DCMAKE_EXPORT_BUILD_DATABASE=1)
  endif ()

  run_cxx_module_test(import-modules "import-modules-${name}" "-DCMAKE_PREFIX_PATH=${RunCMake_BINARY_DIR}/examples/${name}-${type}" ${ARGN})
endfunction ()

# Tests which install BMIs
if ("export_bmi" IN_LIST CMake_TEST_MODULE_COMPILATION)
  run_cxx_module_test(export-interface-no-properties-build)
  run_cxx_module_test(export-interface-build)
  run_cxx_module_test(export-include-directories-build)
  run_cxx_module_test(export-include-directories-old-cmake-build)
  run_cxx_module_test(export-usage-build)
  run_cxx_module_test(export-bmi-and-interface-build)
  run_cxx_module_test(export-command-sepdir-build)
  run_cxx_module_test(export-transitive-targets-build)
  run_cxx_module_test(export-transitive-modules1-build)
  run_cxx_module_test(export-transitive-modules-build export-transitive-modules-build "-DCMAKE_PREFIX_PATH=${RunCMake_BINARY_DIR}/examples/export-transitive-modules1-build-build" )
  run_cxx_module_test(export-with-headers-build)

  if ("collation" IN_LIST CMake_TEST_MODULE_COMPILATION AND
      "bmionly" IN_LIST CMake_TEST_MODULE_COMPILATION)
    run_cxx_module_import_test(build export-interface-build)
    run_cxx_module_import_test(build export-interface-no-properties-build -DNO_PROPERTIES=1)
    run_cxx_module_import_test(build export-include-directories-build -DINCLUDE_PROPERTIES=1)
    run_cxx_module_import_test(build export-bmi-and-interface-build -DWITH_BMIS=1)
    run_cxx_module_import_test(build export-command-sepdir-build -DEXPORT_COMMAND_SEPDIR=1)
    run_cxx_module_import_test(build export-transitive-targets-build -DTRANSITIVE_TARGETS=1)
    run_cxx_module_import_test(build export-transitive-modules-build -DTRANSITIVE_MODULES=1)
    run_cxx_module_import_test(build export-with-headers-build -DWITH_HEADERS=1)

    if ("build_database" IN_LIST CMake_TEST_MODULE_COMPILATION)
      setup_export_build_database_targets()

      run_cxx_module_import_test(build export-build-database -DBUILD_DATABASE=1 EXPORT_BUILD_DATABASE)

      unset(RunCMake_CXXModules_TARGETS)
    endif ()
  endif ()
endif ()

# All of the following tests perform installation.
set(RunCMake_CXXModules_INSTALL 1)

# Tests which install BMIs
if ("install_bmi" IN_LIST CMake_TEST_MODULE_COMPILATION)
  run_cxx_module_test(install-bmi)
  run_cxx_module_test(install-bmi-and-interfaces)

  if ("export_bmi" IN_LIST CMake_TEST_MODULE_COMPILATION)
    run_cxx_module_test(export-interface-no-properties-install)
    run_cxx_module_test(export-interface-install)
    run_cxx_module_test(export-include-directories-install)
    run_cxx_module_test(export-include-directories-old-cmake-install)
    run_cxx_module_test(export-usage-install)
    run_cxx_module_test(export-bmi-and-interface-install)
    run_cxx_module_test(export-command-sepdir-install)
    run_cxx_module_test(export-transitive-targets-install)
    run_cxx_module_test(export-transitive-modules1-install)
    run_cxx_module_test(export-transitive-modules-install export-transitive-modules-install "-DCMAKE_PREFIX_PATH=${RunCMake_BINARY_DIR}/examples/export-transitive-modules1-install-install" )
    run_cxx_module_test(export-with-headers-install)

    if ("collation" IN_LIST CMake_TEST_MODULE_COMPILATION AND
        "bmionly" IN_LIST CMake_TEST_MODULE_COMPILATION)
      run_cxx_module_import_test(install export-interface-install)
      run_cxx_module_import_test(install export-interface-no-properties-install -DNO_PROPERTIES=1)
      run_cxx_module_import_test(install export-include-directories-install -DINCLUDE_PROPERTIES=1)
      run_cxx_module_import_test(install export-bmi-and-interface-install -DWITH_BMIS=1)
      run_cxx_module_import_test(install export-command-sepdir-install -DEXPORT_COMMAND_SEPDIR=1)
      run_cxx_module_import_test(install export-transitive-targets-install -DTRANSITIVE_TARGETS=1)
      run_cxx_module_import_test(install export-transitive-modules-install -DTRANSITIVE_MODULES=1)
      run_cxx_module_import_test(install export-with-headers-install -DWITH_HEADERS=1)
    endif ()
  endif ()
endif ()

# All remaining tests require a working `Ninja` generator to set up a test case
# for the current generator.
if (NOT "ninja" IN_LIST CMake_TEST_MODULE_COMPILATION)
  return ()
endif ()
# All remaining tests require `bmionly` in order to consume from the `ninja`
# build.
if (NOT "bmionly" IN_LIST CMake_TEST_MODULE_COMPILATION)
  return ()
endif ()

function (run_cxx_module_test_ninja directory)
  set(RunCMake_GENERATOR "Ninja")
  set(RunCMake_CXXModules_NO_TEST 1)
  set(RunCMake_CXXModules_INSTALL 1)
  # `Ninja` is not a multi-config generator.
  set(RunCMake_GENERATOR_IS_MULTI_CONFIG 0)
  run_cxx_module_test("${directory}" "${directory}-ninja" ${ARGN})
endfunction ()

# Installation happens within `run_cxx_module_test_ninja`.
set(RunCMake_CXXModules_INSTALL 0)

set(test_set modules-from-ninja)
run_cxx_module_test_ninja("export-${test_set}")
run_cxx_module_test(import-modules "import-${test_set}" "-DCMAKE_PREFIX_PATH=${RunCMake_BINARY_DIR}/examples/export-${test_set}-ninja-install" -DFROM_NINJA=1)
