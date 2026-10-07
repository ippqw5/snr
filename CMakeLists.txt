cmake_minimum_required(VERSION 3.25)

project(snr VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

include(cmake/Dependencies.cmake)

file(GLOB_RECURSE SNR_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(snr STATIC ${SNR_SOURCES})
target_include_directories(snr PUBLIC src)
# GltfScene exposes tinygltf::Model in its interface, so the dependency is public.
target_include_directories(snr PUBLIC external/tinygltf)
target_include_directories(snr SYSTEM PRIVATE external/stb)
target_link_libraries(snr PUBLIC
    slang-rhi
    glm::glm
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
target_compile_options(snr PRIVATE -Wall -Wextra -Wpedantic)

add_executable(pathtracer main.cpp)
target_link_libraries(pathtracer PRIVATE snr)
target_compile_definitions(pathtracer PRIVATE
    SNR_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    SNR_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/external/gltf-sample-assets/Models"
)
target_compile_options(pathtracer PRIVATE -Wall -Wextra -Wpedantic)
