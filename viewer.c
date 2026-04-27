#include "utils.h"

#include <SDL2/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    const uint8_t *srow = src + y * IMAGE_W;
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

static void usage(const char *argv0) {
  fprintf(stderr, "usage: %s [--text TEXT] [--source-vm prospero.vm] [--glyphs glyph_pipeline/glyph_table.txt]\n", argv0);
}

int main(int argc, char **argv) {
  const char *source_vm = "prospero.vm";
  const char *glyph_table = "glyph_pipeline/glyph_table.txt";
  const char *initial_text = DEFAULT_VIEWER_TEXT;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--text") && i + 1 < argc) initial_text = argv[++i];
    else if (!strcmp(argv[i], "--source-vm") && i + 1 < argc) source_vm = argv[++i];
    else if (!strcmp(argv[i], "--glyphs") && i + 1 < argc) glyph_table = argv[++i];
    else {
      usage(argv[0]);
      return 1;
    }
  }

  TextProgram tp;
  char text[UTIL_MAX_TEXT_LEN];
  strncpy(text, initial_text, sizeof(text) - 1);
  text[sizeof(text) - 1] = 0;
  size_t text_len = strlen(text);
  vm_init_coords();
  if (text_program_load(&tp, source_vm, glyph_table) != 0) return 1;
  if (text_program_build(&tp, text) != 0) return 1;
  fprintf(stderr, "viewer bytecode: instructions=%d root=%d\n", vm_instruction_count(), tp.root);

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
  SDL_StartTextInput();

  int running = 1;
  int avg_ready = 0, title_frames = 0, needs_render = 1;
  double render_start = 0, frame_ms = 0, avg_ms = 0;
#pragma omp parallel shared(running, render_start, frame_ms, avg_ms, avg_ready, title_frames, needs_render, text_len, text, tp)
  {
    Worker w;
    vm_worker_init(&w);
    for (;;) {
#pragma omp master
      {
        int changed = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
          if (e.type == SDL_QUIT) running = 0;
          else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = 0;
          else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_BACKSPACE) changed |= backspace_text(text, &text_len);
          else if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER)) changed |= append_text_char(text, &text_len, '\n');
          else if (e.type == SDL_TEXTINPUT) changed |= append_text_input(&tp, text, &text_len, e.text.text);
        }
        if (changed) {
          if (text_program_build(&tp, text) == 0) {
            needs_render = 1;
            avg_ready = 0;
            title_frames = 0;
            fprintf(stderr, "viewer bytecode: instructions=%d root=%d text_len=%zu\n", vm_instruction_count(), tp.root, text_len);
          } else {
            running = 0;
          }
        }
      }
#pragma omp barrier
      if (!running) break;

#pragma omp master
      {
        if (needs_render) render_start = now_sec();
      }
#pragma omp barrier
      if (needs_render) vm_render_root_frame(&w, tp.root);

#pragma omp master
      {
        if (needs_render) {
          frame_ms = (now_sec() - render_start) * 1000;
          if (++title_frames > TITLE_WARMUP_FRAMES) {
            avg_ms = avg_ready ? 0.98 * avg_ms + 0.02 * frame_ms : frame_ms;
            avg_ready = 1;
          }
          if (present_frame(window, renderer, texture, rgb, avg_ready ? avg_ms : frame_ms) != 0) running = 0;
          needs_render = 0;
        }
        SDL_Delay((Uint32)(1000 / TARGET_FPS));
      }
#pragma omp barrier
    }
  }

  SDL_StopTextInput();
  free(rgb);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
