cmake_minimum_required(VERSION 3.20)
project(hexscale VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(daemon)
add_subdirectory(cli)
add_subdirectory(layer)
