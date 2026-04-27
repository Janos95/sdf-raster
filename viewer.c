#include "vm.h"

#include <SDL2/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum {
  TITLE_WARMUP_FRAMES = 32,
  TARGET_FPS = 60
};

static inline double now_sec(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

static void expand_rgb(uint8_t *dst, const uint8_t *src) {
  for (int y = 0; y < IMAGE_H; y++) {
    const uint8_t *srow = src + (IMAGE_H - 1 - y) * IMAGE_W;
    uint8_t *drow = dst + 3 * y * IMAGE_W;
    for (int x = 0; x < IMAGE_W; x++) {
      uint8_t v = srow[x];
      drow[3 * x + 0] = v;
      drow[3 * x + 1] = v;
      drow[3 * x + 2] = v;
    }
  }
}

static int present_frame(SDL_Window *window, SDL_Renderer *renderer, SDL_Texture *texture, uint8_t *rgb, double frame_ms) {
  expand_rgb(rgb, vm_image_data());
  if (SDL_UpdateTexture(texture, NULL, rgb, IMAGE_W * 3) != 0) {
    fprintf(stderr, "SDL_UpdateTexture: %s\n", SDL_GetError());
    return -1;
  }
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, NULL, NULL);
  SDL_RenderPresent(renderer);

  char title[128];
  snprintf(title, sizeof(title), "Prospero %.3f ms", frame_ms);
  SDL_SetWindowTitle(window, title);
  return 0;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "prospero.vm";
  vm_init_coords();
  if (vm_load_file(path) != 0) return 1;

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window *window = SDL_CreateWindow("Prospero", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, IMAGE_W, IMAGE_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) {
    fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_RenderSetLogicalSize(renderer, IMAGE_W, IMAGE_H);

  SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, IMAGE_W, IMAGE_H);
  if (!texture) {
    fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  uint8_t *rgb = malloc((size_t)IMAGE_W * IMAGE_H * 3);
  if (!rgb) {
    fprintf(stderr, "out of memory\n");
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  int running = 1;
  int avg_ready = 0, title_frames = 0;
  double render_start = 0, frame_ms = 0, avg_ms = 0;
#pragma omp parallel shared(running, render_start, frame_ms, avg_ms, avg_ready, title_frames)
  {
    Worker w;
    vm_worker_init(&w);
    for (;;) {
#pragma omp master
      {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
          if (e.type == SDL_QUIT) running = 0;
          else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = 0;
        }
      }
#pragma omp barrier
      if (!running) break;

#pragma omp master
      render_start = now_sec();
#pragma omp barrier
      vm_render_frame(&w);

#pragma omp master
      {
        frame_ms = (now_sec() - render_start) * 1000;
        if (++title_frames > TITLE_WARMUP_FRAMES) {
          avg_ms = avg_ready ? 0.98 * avg_ms + 0.02 * frame_ms : frame_ms;
          avg_ready = 1;
        }
        if (present_frame(window, renderer, texture, rgb, avg_ready ? avg_ms : frame_ms) != 0) running = 0;
        double frame_end = now_sec();
        double target_dt = 1.0 / TARGET_FPS;
        double elapsed = frame_end - render_start;
        if (elapsed < target_dt) SDL_Delay((Uint32)((target_dt - elapsed) * 1000));
      }
#pragma omp barrier
    }
  }

  free(rgb);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
