#include <SDL3/SDL.h>

#include <stdio.h>  // printf, fprintf
#include <stdlib.h> // abort

namespace Platform {

bool init() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }
    return 0;
}

void shutdown() { SDL_Quit(); }

}