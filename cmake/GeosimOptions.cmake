# Build options and optional dependency discovery.
#
# The default configuration builds the headless simulation (DCON + oneTBB are
# fetched at configure time if not installed). GEOSIM_USE_DCON=OFF builds only
# the dependency-free core library and its tests.

option(GEOSIM_BUILD_TESTS  "Build ctest unit tests"                              ON)
option(GEOSIM_BUILD_CLIENT "Build the SDL3/Vulkan/FreeType/HarfBuzz game client" OFF)
option(GEOSIM_USE_DCON     "Fetch DataContainer (+oneTBB) and build the simulation" ON)

# Supported compilers: GCC and Clang. MSVC is not supported.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "geosim supports GCC and Clang only (got ${CMAKE_CXX_COMPILER_ID})")
endif()

# Shared warning flags, applied per target via geosim_set_warnings().
function(geosim_set_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endfunction()

if(GEOSIM_BUILD_CLIENT)
    find_package(SDL3 CONFIG REQUIRED)
    find_package(Vulkan REQUIRED)
    find_package(Freetype REQUIRED)
    # HarfBuzz ships a CMake config on most distributions; fall back to pkg-config.
    find_package(harfbuzz CONFIG QUIET)
    if(NOT harfbuzz_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
        add_library(harfbuzz::harfbuzz ALIAS PkgConfig::HARFBUZZ)
    endif()
endif()

if(GEOSIM_USE_DCON)
    include(Dcon)
endif()
