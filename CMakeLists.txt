cmake_minimum_required(VERSION 3.25)

project(snr VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

include(cmake/Dependencies.cmake)

file(GLOB_RECURSE SNR_SOURCES CONFIGURE_DEPENDS src/*.cpp)
# The CLI front end owns main(), so it belongs to the executable rather than the library.
list(FILTER SNR_SOURCES EXCLUDE REGEX "main\\.cpp$")
add_library(snr_core STATIC ${SNR_SOURCES})
target_include_directories(snr_core PUBLIC src)
target_include_directories(snr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(snr_core PRIVATE external/tinygltf)
target_include_directories(snr_core SYSTEM PRIVATE external/stb)
set(SNR_DEPENDENCIES
    slang-rhi
    glm::glm
    glfw
    snr_imgui
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
target_link_libraries(snr_core PUBLIC ${SNR_DEPENDENCIES})
target_compile_options(snr_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(snr src/main.cpp)
target_link_libraries(snr PRIVATE snr_core)
target_compile_definitions(snr PRIVATE
    SNR_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    SNR_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/external/gltf-sample-assets/Models"
)
target_compile_options(snr PRIVATE -Wall -Wextra -Wpedantic)
