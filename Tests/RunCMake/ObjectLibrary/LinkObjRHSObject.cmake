enable_language(C)

add_library(AnObjLib OBJECT a.c)
target_compile_definitions(AnObjLib INTERFACE REQUIRED)

add_library(AnotherObjLib OBJECT b.c)
target_link_libraries(AnotherObjLib PRIVATE AnObjLib)

add_executable(exe exe.c)
target_link_libraries(exe AnotherObjLib)
