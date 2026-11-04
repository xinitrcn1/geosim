# Build options and optional dependency discovery.
#
# The build always includes the headless simulation: DataContainer (DCON) and
# oneTBB are required (fetched at configure time if not installed) - the schema
# generator needs DCON anyway.

option(GEOSIM_BUILD_TESTS  "Build ctest unit tests"                              ON)
option(GEOSIM_BUILD_CLIENT "Build the SDL3/Vulkan/FreeType game client (HarfBuzz optional)" OFF)

# Supported compilers: GCC and Clang. MSVC is not supported.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "geosim supports GCC and Clang only (got ${CMAKE_CXX_COMPILER_ID})")
endif()

# Shared per-target settings, applied to every geosim target via
# geosim_target_defaults():
#  - warnings;
#  - -fno-exceptions -fno-rtti: the engine reports errors through
#    core::diagnostics and never uses dynamic_cast/typeid
#    (docs/09-engine-architecture.md#error-handling). DCON aborts on a full
#    container unless DCON_USE_EXCEPTIONS is defined (we never define it), and
#    oneTBB's headers disable their exception paths when __cpp_exceptions is off.
set(GEOSIM_NO_EXCEPTIONS_RTTI -fno-exceptions -fno-rtti)
function(geosim_target_defaults target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion ${GEOSIM_NO_EXCEPTIONS_RTTI})
endfunction()

if(GEOSIM_BUILD_CLIENT)
    find_package(SDL3 CONFIG REQUIRED)
    find_package(Vulkan REQUIRED COMPONENTS glslangValidator)
    find_package(Freetype REQUIRED)
    # HarfBuzz is optional for now: the client does "shaping-lite" (cmap +
    # advances + a GSUB ligature table for emoji flags, docs/08-ui.md#text).
    # When found it is linked and GEOSIM_HAS_HARFBUZZ=1 is defined so a full
    # shaper can be dropped in later.
    find_package(harfbuzz CONFIG QUIET)
    if(NOT harfbuzz_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(HARFBUZZ QUIET IMPORTED_TARGET harfbuzz)
            if(HARFBUZZ_FOUND)
                add_library(harfbuzz::harfbuzz ALIAS PkgConfig::HARFBUZZ)
            endif()
        endif()
    endif()
    if(TARGET harfbuzz::harfbuzz)
        set(GEOSIM_HAVE_HARFBUZZ ON)
    else()
        set(GEOSIM_HAVE_HARFBUZZ OFF)
        message(STATUS "geosim client: HarfBuzz not found, using the built-in shaping-lite path")
    endif()
endif()

include(Dcon)
