cmake_minimum_required(VERSION 3.16)

project(luce_host_tests LANGUAGES CXX)

add_executable(luce_host_tests test_main.cpp)
target_compile_features(luce_host_tests PRIVATE cxx_std_17)
target_include_directories(luce_host_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../include)
target_compile_options(luce_host_tests PRIVATE -Wall -Wextra -Werror)
