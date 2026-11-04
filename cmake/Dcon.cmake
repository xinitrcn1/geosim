# DataContainer (DCON) integration.
#
# Fetches schombert/DataContainer, builds its generator, and provides
#   geosim_dcon_generate(<target> <schema.txt>)
# which runs the generator at build time and adds the generated headers
# (<name>.hpp, <name>_ids.hpp) plus DCON's CommonIncludes to <target>.
#
# The generator writes its output next to its input, so the schema is copied
# into the build tree first and nothing generated ever lands in the source tree.

include(FetchContent)

set(GEOSIM_DCON_GIT_TAG "master" CACHE STRING
    "DataContainer git tag/commit (pin a commit hash before relying on it)")

FetchContent_Declare(dcon
    GIT_REPOSITORY https://github.com/schombert/DataContainer.git
    GIT_TAG        ${GEOSIM_DCON_GIT_TAG}
    GIT_SHALLOW    TRUE
    # Upstream has no CMake build (Visual Studio solution only). Pointing at a
    # directory without CMakeLists.txt makes MakeAvailable just populate it.
    SOURCE_SUBDIR  _no_cmake_)
FetchContent_MakeAvailable(dcon)

add_executable(dcon_generator
    ${dcon_SOURCE_DIR}/DataContainerGenerator/DataContainerGenerator.cpp
    ${dcon_SOURCE_DIR}/DataContainerGenerator/code_fragments.cpp
    ${dcon_SOURCE_DIR}/DataContainerGenerator/object_member_fragments.cpp
    ${dcon_SOURCE_DIR}/DataContainerGenerator/parsing.cpp
    ${dcon_SOURCE_DIR}/DataContainerGenerator/query_fragments.cpp
    ${dcon_SOURCE_DIR}/DataContainerGenerator/serialize_fragments.cpp)
target_compile_features(dcon_generator PRIVATE cxx_std_20)

# oneTBB: the worker pool behind DCON's execute_parallel (ve.hpp maps
# `concurrency::` to oneapi::tbb when PREFER_ONE_TBB is defined). We use DCON's
# threading as the engine's parallelism; there is no separate job system
# (docs/09-engine-architecture.md#threading). A system install is preferred,
# otherwise oneTBB is built from source.
set(GEOSIM_TBB_GIT_TAG "v2022.0.0" CACHE STRING "oneTBB tag used when no system TBB is found")
find_package(TBB CONFIG QUIET)
if(NOT TBB_FOUND)
    message(STATUS "geosim: system oneTBB not found, fetching ${GEOSIM_TBB_GIT_TAG}")
    set(TBB_TEST OFF CACHE BOOL "" FORCE)
    set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TBB_STRICT OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(onetbb
        GIT_REPOSITORY https://github.com/uxlfoundation/oneTBB.git
        GIT_TAG        ${GEOSIM_TBB_GIT_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(onetbb)
endif()

# CommonIncludes runtime (common_types.cpp + ve SIMD headers).
#  - PREFER_ONE_TBB: use oneTBB instead of <ppl.h> (an MSVC-only header).
add_library(dcon_common STATIC ${dcon_SOURCE_DIR}/CommonIncludes/common_types.cpp)
target_include_directories(dcon_common SYSTEM PUBLIC ${dcon_SOURCE_DIR}/CommonIncludes)
target_compile_features(dcon_common PUBLIC cxx_std_20)
target_compile_definitions(dcon_common PUBLIC PREFER_ONE_TBB)
target_link_libraries(dcon_common PUBLIC TBB::tbb)
# ve picks its SIMD backend from the target ISA, so one must be enabled.
# AVX2 + FMA is the baseline (docs/00-vision.md#target-platform).
# -ffp-contract=on allows FMA fusion within an expression only (never across
# statements, unlike `fast`). That is deterministic for a given compiler and
# flag set (docs/02-simulation-core.md#determinism).
target_compile_options(dcon_common PUBLIC
    -mavx2 -mfma -ffp-contract=on)
# Same exception / RTTI model as the engine (common_types.cpp is linked into it).
target_compile_options(dcon_common PRIVATE ${GEOSIM_NO_EXCEPTIONS_RTTI})

function(geosim_dcon_generate target schema)
    get_filename_component(schema_abs "${schema}" ABSOLUTE)
    get_filename_component(schema_name "${schema}" NAME_WE)
    set(out_dir "${CMAKE_BINARY_DIR}/generated")
    set(schema_copy "${out_dir}/${schema_name}.txt")
    set(out_hpp     "${out_dir}/${schema_name}.hpp")
    set(out_ids     "${out_dir}/${schema_name}_ids.hpp")

    add_custom_command(
        OUTPUT  ${out_hpp} ${out_ids}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
        COMMAND ${CMAKE_COMMAND} -E copy ${schema_abs} ${schema_copy}
        COMMAND $<TARGET_FILE:dcon_generator> ${schema_copy}
        DEPENDS ${schema_abs} dcon_generator
        COMMENT "DCON: generating ${schema_name}.hpp"
        VERBATIM)

    add_custom_target(${target}_dcon DEPENDS ${out_hpp} ${out_ids})
    add_dependencies(${target} ${target}_dcon)
    target_include_directories(${target} SYSTEM PUBLIC ${out_dir})
    target_link_libraries(${target} PUBLIC dcon_common)
endfunction()
