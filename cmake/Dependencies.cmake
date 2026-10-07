function(snr_require_submodule name file)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/external/${name}/${file}")
        message(
            FATAL_ERROR
            "Missing external/${name}/${file}. Run: git submodule update --init external/${name}"
        )
    endif()
endfunction()

snr_require_submodule(tinygltf tiny_gltf.h)
snr_require_submodule(glm glm/glm.hpp)
snr_require_submodule(stb stb_image.h)
snr_require_submodule(slang-rhi CMakeLists.txt)

if(NOT EXISTS "$ENV{VULKAN_SDK}/include/vulkan/vulkan.h")
    message(
        FATAL_ERROR
        "Vulkan SDK not found. Please install the Vulkan SDK and set the VULKAN_SDK environment variable."
    )
endif()

# The Linux Vulkan SDK ships Slang's CMake package (lib/cmake/slang), the Windows one only ships
# Slang's binaries, headers and import libraries, so recreate the package targets from that layout.
function(snr_slang_from_vulkan_sdk)
    set(sdk "$ENV{VULKAN_SDK}")

    if(WIN32)
        set(include_dir "${sdk}/Include/slang")
        set(bin_dir "${sdk}/Bin")
        set(lib_dir "${sdk}/Lib")
    endif()

    if(NOT WIN32 OR NOT EXISTS "${include_dir}/slang.h" OR NOT EXISTS "${bin_dir}/slangc.exe")
        message(
            FATAL_ERROR
            "Could not find the slang CMake package, and the Vulkan SDK at \"${sdk}\" does not "
            "contain the Slang compiler either (expected Include/slang/slang.h and "
            "Bin/slangc.exe).\n"
            "Install Slang separately and configure with -Dslang_DIR=<slang>/lib/cmake/slang."
        )
    endif()

    # Slang renamed its compiler library from slang to slang-compiler in v2025.21.
    if(EXISTS "${lib_dir}/slang-compiler.lib")
        set(library slang-compiler)
    else()
        set(library slang)
    endif()

    add_library(slang::slang SHARED IMPORTED)
    set_target_properties(
        slang::slang
        PROPERTIES
            IMPORTED_IMPLIB "${lib_dir}/${library}.lib"
            IMPORTED_LOCATION "${bin_dir}/${library}.dll"
            INTERFACE_COMPILE_DEFINITIONS SLANG_DYNAMIC
            INTERFACE_INCLUDE_DIRECTORIES "${include_dir}"
    )

    add_executable(slang::slangc IMPORTED)
    set_target_properties(slang::slangc PROPERTIES IMPORTED_LOCATION "${bin_dir}/slangc.exe")

    execute_process(
        COMMAND "${bin_dir}/slangc.exe" -v
        OUTPUT_VARIABLE snr_slang_version
        ERROR_VARIABLE snr_slang_version_stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
        TIMEOUT 10
    )
    # slangc reports its version on stderr.
    if(NOT snr_slang_version)
        set(snr_slang_version "${snr_slang_version_stderr}")
    endif()
    if(NOT snr_slang_version)
        set(snr_slang_version "unknown version")
    endif()

    set(slang_DIR "${sdk}" PARENT_SCOPE)
    set(slang_VERSION "${snr_slang_version} from Vulkan SDK" PARENT_SCOPE)
endfunction()

set(snr_slang_find_options)

if(slang_DIR)
    list(APPEND snr_slang_find_options NO_DEFAULT_PATH)
endif()

find_package(slang CONFIG QUIET ${snr_slang_find_options})

if(NOT slang_FOUND)
    snr_slang_from_vulkan_sdk()
endif()

if(NOT TARGET slang::slang OR NOT TARGET slang::slangc)
    message(
        FATAL_ERROR
        "The slang package must export slang::slang and slang::slangc targets"
    )
endif()

add_library(slang ALIAS slang::slang)
get_target_property(snr_slang_include_dir slang::slang INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "SNR Slang package: ${slang_VERSION} (${slang_DIR})")

set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(external/glm EXCLUDE_FROM_ALL)

set(SLANG_RHI_BUILD_FROM_SLANG_REPO ON)
set(SLANG_RHI_FETCH_SLANG OFF CACHE BOOL "" FORCE)
set(SLANG_RHI_SLANG_INCLUDE_DIR "${snr_slang_include_dir}" CACHE STRING "" FORCE)
set(SLANG_RHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SLANG_RHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SLANG_RHI_BUILD_TESTS_WITH_GLFW OFF CACHE BOOL "" FORCE)
set(SLANG_RHI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SLANG_RHI_INSTALL OFF CACHE BOOL "" FORCE)

foreach(backend CPU CUDA OPTIX WGPU METAL D3D11 D3D12 AGILITY_SDK NVAPI AFTERMATH)
    set(SLANG_RHI_ENABLE_${backend} OFF CACHE BOOL "" FORCE)
endforeach()

set(SLANG_RHI_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS "$ENV{VULKAN_SDK}" CACHE STRING "" FORCE)
add_subdirectory(external/slang-rhi EXCLUDE_FROM_ALL)
