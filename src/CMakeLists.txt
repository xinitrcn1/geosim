# --- core: dependency-free utilities (calendar, CSV, defs parser, scheduler) --
add_library(geosim_core STATIC
    core/calendar.cpp
    core/csv.cpp
    core/defs.cpp
    core/scheduler.cpp)
target_include_directories(geosim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(geosim_core PUBLIC cxx_std_20)
geosim_set_warnings(geosim_core)

# --- sim: simulation on the DCON container (headless, no rendering deps) -----
if(GEOSIM_USE_DCON)
    add_library(geosim_sim STATIC
        sim/backtest.cpp
        sim/ledger.cpp
        sim/params.cpp
        sim/report.cpp
        sim/save.cpp
        sim/scenario.cpp
        sim/simulation.cpp
        sim/systems/ai.cpp
        sim/systems/economy.cpp
        sim/systems/epidemic.cpp
        sim/systems/events.cpp
        sim/systems/indicators.cpp
        sim/systems/markets.cpp)
    target_link_libraries(geosim_sim PUBLIC geosim_core)
    geosim_dcon_generate(geosim_sim ${PROJECT_SOURCE_DIR}/schema/geosim_dcon.txt)
    geosim_set_warnings(geosim_sim)

    add_executable(geosim_headless headless/main.cpp)
    target_link_libraries(geosim_headless PRIVATE geosim_sim)
    geosim_set_warnings(geosim_headless)
endif()

# --- client: SDL3 + Vulkan + FreeType + HarfBuzz ----------------------------
if(GEOSIM_BUILD_CLIENT)
    add_executable(geosim client/main.cpp)
    target_link_libraries(geosim PRIVATE
        geosim_core
        SDL3::SDL3
        Vulkan::Vulkan
        Freetype::Freetype
        harfbuzz::harfbuzz)
    geosim_set_warnings(geosim)
endif()
