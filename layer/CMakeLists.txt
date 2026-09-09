cmake_minimum_required(VERSION 3.20)
project(hexscale-layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(VkLayer_hexscale SHARED
    src/layer_entry.cpp
)

target_include_directories(VkLayer_hexscale PRIVATE
    ../daemon/include
)

target_compile_definitions(VkLayer_hexscale PRIVATE
    VK_USE_PLATFORM_WAYLAND_KHR
)

install(TARGETS VkLayer_hexscale DESTINATION lib)
install(FILES VkLayer_hexscale.json DESTINATION share/vulkan/explicit_layer.d)
