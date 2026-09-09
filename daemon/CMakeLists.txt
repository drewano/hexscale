cmake_minimum_required(VERSION 3.20)
project(hexscaled LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hexscaled
    src/main.cpp
    src/fastrpc_session.cpp
    src/qnn_backend.cpp
    src/ipc_server.cpp
)

target_include_directories(hexscaled PRIVATE
    include
)

find_package(Threads REQUIRED)
target_link_libraries(hexscaled PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

install(TARGETS hexscaled DESTINATION bin)
