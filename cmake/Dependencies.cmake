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

set(snr_slang_find_options)

if(slang_DIR)
    list(APPEND snr_slang_find_options NO_DEFAULT_PATH)
endif()

find_package(slang CONFIG REQUIRED ${snr_slang_find_options})

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
