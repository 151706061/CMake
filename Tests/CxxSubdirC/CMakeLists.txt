cmake_minimum_required(VERSION 3.10)
project(CxxSubdirC CXX)
add_subdirectory(Cdir)
add_executable(CxxSubdirC main.cxx $<TARGET_OBJECTS:Cobj>)
