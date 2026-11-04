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
        sim/systems/strategy.cpp
        sim/systems/diplomacy.cpp
        sim/systems/organisations.cpp
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
endif()

# --- client: SDL3 + Vulkan 1.3 + FreeType (docs/08-ui.md) ----------------------
if(GEOSIM_BUILD_CLIENT)
    if(NOT TARGET geosim_sim)
        message(FATAL_ERROR "the client links geosim_sim: configure with -DGEOSIM_USE_DCON=ON")
    endif()

    # GLSL -> SPIR-V at build time; each shader becomes a C header holding a
    # `uint32_t <name>[]` array (glslangValidator --vn), embedded in the binary.
    set(geosim_shader_dir ${CMAKE_CURRENT_SOURCE_DIR}/client/shaders)
    set(geosim_shader_out ${CMAKE_CURRENT_BINARY_DIR}/client_shaders)
    file(MAKE_DIRECTORY ${geosim_shader_out})
    set(geosim_shader_headers)
    foreach(shader quad.vert ui.frag map.frag fullscreen.vert blur.frag)
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
    endif()

    add_executable(geosim
        client/main.cpp
        client/app.cpp
        client/audio/music.cpp
        client/sim_runner.cpp
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
        Freetype::Freetype)
    if(GEOSIM_HAVE_HARFBUZZ)
        target_link_libraries(geosim PRIVATE harfbuzz::harfbuzz)
        target_compile_definitions(geosim PRIVATE GEOSIM_HAS_HARFBUZZ=1)
    endif()
    geosim_target_defaults(geosim)
    # Vulkan structs are initialised as `VkFooCreateInfo info{VK_STRUCTURE_TYPE_...}`
    # (the rest value-initialised), which -Wextra would flag on every struct.
    target_compile_options(geosim PRIVATE -Wno-missing-field-initializers)
endif()
