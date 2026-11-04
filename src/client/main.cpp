// Game client entry point (stub). Built only with -DGEOSIM_BUILD_CLIENT=ON.
// Architecture: docs/09-engine-architecture.md.

#define SDL_MAIN_USE_CALLBACKS 0
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

int main(int, char**) {
    if(!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("geosim", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if(!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    bool running = true;
    while(running) {
        SDL_Event ev;
        while(SDL_PollEvent(&ev)) {
            if(ev.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }
        SDL_Delay(16); // TODO: Vulkan renderer + sim snapshot consumer
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
