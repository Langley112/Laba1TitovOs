cmake_minimum_required(VERSION 3.10)
project(program_a LANGUAGES CXX)

add_executable(program_a program_a.cpp)
target_compile_features(program_a PRIVATE cxx_std_17)
set_target_properties(program_a PROPERTIES CXX_EXTENSIONS OFF)

add_executable(program_b program_b.cpp)
target_compile_features(program_b PRIVATE cxx_std_17)
set_target_properties(program_b PROPERTIES CXX_EXTENSIONS OFF)

add_executable(program_c program_c.cpp)
target_compile_features(program_c PRIVATE cxx_std_17)
set_target_properties(program_c PROPERTIES CXX_EXTENSIONS OFF)
