cmake_minimum_required(VERSION 3.20)
project(hexscale-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hexscale-cli
    src/main.cpp
    ../daemon/src/qnn_backend.cpp
)

target_include_directories(hexscale-cli PRIVATE
    ../daemon/include
)

target_link_libraries(hexscale-cli PRIVATE
    ${CMAKE_DL_LIBS}
)

install(TARGETS hexscale-cli DESTINATION bin)
