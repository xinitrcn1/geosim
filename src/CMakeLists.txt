# --- core: dependency-free utilities (calendar, CSV, defs parser, scheduler) --
add_library(geosim_core STATIC
    core/calendar.cpp
    core/csv.cpp
    core/defs.cpp
    core/scheduler.cpp)
target_include_directories(geosim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(geosim_core PUBLIC cxx_std_20)
geosim_target_defaults(geosim_core)

# --- sim: simulation on the DCON container (headless, no rendering deps; DCON is required) -----
add_library(geosim_sim STATIC
    sim/backtest.cpp
    sim/fork.cpp
    sim/ledger.cpp
    sim/params.cpp
    sim/report.cpp
    sim/save.cpp
    sim/world_io.cpp
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
    sim/systems/strategy.cpp
    sim/systems/diplomacy.cpp
    sim/systems/organisations.cpp
    sim/systems/geo_orgs.cpp
    sim/systems/megaevents.cpp
    sim/systems/disasters.cpp
    sim/systems/actions.cpp
    sim/commands.cpp
    sim/systems/reforms.cpp
    sim/systems/politics_ext.cpp
    sim/systems/agreements.cpp
    sim/systems/society.cpp)
target_link_libraries(geosim_sim PUBLIC geosim_core)
geosim_dcon_generate(geosim_sim ${PROJECT_SOURCE_DIR}/schema/geosim_dcon.txt)
geosim_target_defaults(geosim_sim)

add_executable(geosim_headless headless/main.cpp)
target_link_libraries(geosim_headless PRIVATE geosim_sim)
geosim_target_defaults(geosim_headless)

# --- client: SDL3 + Vulkan 1.3 + FreeType (docs/08-ui.md) ----------------------
if(GEOSIM_BUILD_CLIENT)

    # GLSL -> SPIR-V at build time; each shader becomes a C header holding a
    # `uint32_t <name>[]` array (glslangValidator --vn), embedded in the binary.
    set(geosim_shader_dir ${CMAKE_CURRENT_SOURCE_DIR}/client/shaders)
    set(geosim_shader_out ${CMAKE_CURRENT_BINARY_DIR}/client_shaders)
    file(MAKE_DIRECTORY ${geosim_shader_out})
    set(geosim_shader_headers)
    foreach(shader quad.vert ui.frag map.frag world_map.frag map_ids.vert map_ids.frag map_resolve.frag fullscreen.vert blur.frag)
        string(REPLACE "." "_" var "spv_${shader}")
        set(out ${geosim_shader_out}/${shader}.h)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} --quiet --target-env vulkan1.3
                    --vn ${var} -o ${out} ${geosim_shader_dir}/${shader}
            DEPENDS ${geosim_shader_dir}/${shader} ${geosim_shader_dir}/common.glsl
            COMMENT "glslang ${shader}"
            VERBATIM)
        list(APPEND geosim_shader_headers ${out})
    endforeach()
    add_custom_target(geosim_client_shaders DEPENDS ${geosim_shader_headers})

    # Procedural music (docs/14-audio.md): composer + synth + offline renderer,
    # no SDL. DSP code is always optimised: at -O0 the synth would not keep up
    # with real time in Debug builds.
    add_library(geosim_audio STATIC
        client/audio/composer.cpp
        client/audio/jukebox.cpp
        client/audio/render.cpp
        client/audio/synth.cpp)
    target_link_libraries(geosim_audio PUBLIC geosim_core)
    geosim_target_defaults(geosim_audio)
    target_compile_options(geosim_audio PRIVATE -O2)
    if(GEOSIM_BUILD_TESTS)
        add_executable(test_music ${PROJECT_SOURCE_DIR}/tests/test_music.cpp)
        target_link_libraries(test_music PRIVATE geosim_audio)
        geosim_target_defaults(test_music)
        add_test(NAME test_music COMMAND test_music)
        # Map projection (header-only, no Vulkan).
        add_executable(test_map_view ${PROJECT_SOURCE_DIR}/tests/test_map_view.cpp)
        target_include_directories(test_map_view PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/client)
        geosim_target_defaults(test_map_view)
        add_test(NAME test_map_view COMMAND test_map_view)
    endif()

    add_executable(geosim
        client/main.cpp
        client/app.cpp
        client/audio/music.cpp
        client/map_data.cpp
        client/map_layer.cpp
        client/map_renderer.cpp
        client/hazard_feed.cpp
        client/sim_hazards.cpp
        client/sim_runner.cpp
        client/sim_tabs.cpp
        client/action_tabs.cpp
        client/text.cpp
        client/ui_context.cpp
        client/ui_layout.cpp
        client/ui_renderer.cpp
        client/vk_context.cpp)
    add_dependencies(geosim geosim_client_shaders)
    find_package(Threads REQUIRED) # the simulation runs on its own thread
    target_include_directories(geosim PRIVATE ${geosim_shader_out})
    target_link_libraries(geosim PRIVATE
        geosim_sim
        geosim_audio
        Threads::Threads
        SDL3::SDL3
        Vulkan::Vulkan
        Freetype::Freetype
        ZLIB::ZLIB)
    if(GEOSIM_HAVE_HARFBUZZ)
        target_link_libraries(geosim PRIVATE harfbuzz::harfbuzz)
        target_compile_definitions(geosim PRIVATE GEOSIM_HAS_HARFBUZZ=1)
    endif()
    geosim_target_defaults(geosim)
    # Vulkan structs are initialised as `VkFooCreateInfo info{VK_STRUCTURE_TYPE_...}`
    # (the rest value-initialised), which -Wextra would flag on every struct.
    target_compile_options(geosim PRIVATE -Wno-missing-field-initializers)
    # Decoding and colouring the world map (~45 M texels) is slow at -O0.
    set_source_files_properties(client/map_data.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()

# DCON's whole-container serialisation is a very large generated function set: no
# debug info for it (it is never stepped through) cuts its compile time by ~15%.
set_source_files_properties(sim/world_io.cpp PROPERTIES COMPILE_OPTIONS "-g0")
# Every simulation file includes the generated container (game_state.hpp): parse it
# once as a precompiled header instead of in each translation unit.
target_precompile_headers(geosim_sim PRIVATE sim/game_state.hpp)
