#pragma once

#include "imgui.hpp"
#include <SDL.h>
#include "imgui_impl_sdl.h"

inline void imgui_back_init() {
  SDL_Window* window = SDL_CreateWindow("test",  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_SHOWN);
  SDL_Event* event = new SDL_Event();
}
