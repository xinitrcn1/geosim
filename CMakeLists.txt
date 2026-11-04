cmake_minimum_required(VERSION 3.25)

project(geosim
    VERSION 0.0.1
    DESCRIPTION "Socioeconomic world simulation"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GeosimOptions)

enable_testing()

add_subdirectory(src)

if(GEOSIM_BUILD_TESTS)
    add_subdirectory(tests)
endif()
