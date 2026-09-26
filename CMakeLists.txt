cmake_minimum_required(VERSION 3.25)

project(snr VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

include(cmake/Dependencies.cmake)

# file(GLOB_RECURSE SNR_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(snr STATIC
    src/scene/scene.cpp
    src/render/renderer.cpp
    src/gpu/device.cpp
    src/gpu/shader_library.cpp
    src/scene/gpu_scene.cpp
    src/render/settings.cpp
    src/render/film.cpp
    src/render/session.cpp
    src/io/image_output.cpp
)
target_include_directories(snr PUBLIC src)
target_include_directories(snr PRIVATE external/cgltf)
target_include_directories(snr SYSTEM PRIVATE external/stb)
target_link_libraries(snr PUBLIC
    slang-rhi
    glm::glm
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
target_compile_options(snr PRIVATE -Wall -Wextra -Wpedantic)

add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE snr)
target_compile_definitions(main PRIVATE
    SNR_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    SNR_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/external/gltf-sample-assets/Models"
)
target_compile_options(main PRIVATE -Wall -Wextra -Wpedantic)
