cmake_minimum_required(VERSION 3.16)
project(Cyrk)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "-Wfatal-errors -Wall -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG  "-D_GLIBCXX_DEBUG")

include_directories(.)

add_executable(Cyrk
        demo.cpp
        machine.hpp
        system.cpp
        system.hpp)