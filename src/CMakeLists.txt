# --- core: dependency-free utilities (calendar, CSV, defs parser, scheduler) --
add_library(geosim_core STATIC
    core/calendar.cpp
    core/csv.cpp
    core/defs.cpp
    core/scheduler.cpp)
target_include_directories(geosim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(geosim_core PUBLIC cxx_std_20)
geosim_target_defaults(geosim_core)

# --- sim: simulation on the DCON container (headless, no rendering deps) -----
if(GEOSIM_USE_DCON)
    add_library(geosim_sim STATIC
        sim/backtest.cpp
        sim/fork.cpp
        sim/ledger.cpp
        sim/params.cpp
        sim/report.cpp
        sim/save.cpp
        sim/scenario.cpp
        sim/scenario_ext.cpp
        sim/simulation.cpp
        sim/systems/ai.cpp
        sim/systems/assets.cpp
        sim/systems/fiscal.cpp
        sim/systems/institutions.cpp
        sim/systems/labour.cpp
        sim/systems/treaties.cpp
        sim/systems/bonds.cpp
        sim/systems/climate.cpp
        sim/systems/commodities.cpp
        sim/systems/credit.cpp
        sim/systems/economy.cpp
        sim/systems/epidemic.cpp
        sim/systems/events.cpp
        sim/systems/finance.cpp
        sim/systems/firms.cpp
        sim/systems/geopolitics.cpp
        sim/systems/logistics.cpp
        sim/systems/longcampaign.cpp
        sim/systems/longrun.cpp
        sim/systems/indicators.cpp
        sim/systems/internet.cpp
        sim/systems/markets.cpp
        sim/systems/politics.cpp
        sim/systems/policy.cpp
        sim/systems/society.cpp)
    target_link_libraries(geosim_sim PUBLIC geosim_core)
    geosim_dcon_generate(geosim_sim ${PROJECT_SOURCE_DIR}/schema/geosim_dcon.txt)
    geosim_target_defaults(geosim_sim)

    add_executable(geosim_headless headless/main.cpp)
    target_link_libraries(geosim_headless PRIVATE geosim_sim)
    geosim_target_defaults(geosim_headless)
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
    geosim_target_defaults(geosim)
endif()
