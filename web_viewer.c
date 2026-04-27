#include "utils.h"

#include <emscripten/emscripten.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static TextProgram tp;
static char text[UTIL_MAX_TEXT_LEN];
static size_t text_len;
static int initialized;
static double last_render_ms;

static int ensure_initialized(void) {
  if (initialized) return 0;
#ifdef _OPENMP
  omp_set_num_threads(8);
#endif
  vm_init_coords();
  if (text_program_load(&tp, "glyph_table.txt") != 0) return -1;
  strncpy(text, DEFAULT_VIEWER_TEXT, sizeof(text) - 1);
  text[sizeof(text) - 1] = 0;
  text_len = strlen(text);
  if (text_program_build(&tp, text) != 0) return -1;
  initialized = 1;
  return 0;
}

static int rebuild_text(void) {
  if (ensure_initialized() != 0) return -1;
  return text_program_build(&tp, text);
}

EMSCRIPTEN_KEEPALIVE
int prospero_init(void) {
  return ensure_initialized();
}

EMSCRIPTEN_KEEPALIVE
const char *prospero_default_text(void) {
  return DEFAULT_VIEWER_TEXT;
}

EMSCRIPTEN_KEEPALIVE
const char *prospero_text(void) {
  return text;
}

EMSCRIPTEN_KEEPALIVE
int prospero_set_text(const char *input) {
  if (ensure_initialized() != 0) return -1;
  text_len = 0;
  text[0] = 0;
  for (const unsigned char *p = (const unsigned char *)input; p && *p; p++) {
    if (*p == '\n') {
      append_text_char(text, &text_len, '\n');
    } else if (*p >= 32 && *p < 127) {
      append_text_char(text, &text_len, closest_supported_char(&tp, *p));
    }
  }
  return text_program_build(&tp, text);
}

EMSCRIPTEN_KEEPALIVE
int prospero_append_text(const char *input) {
  if (ensure_initialized() != 0) return -1;
  append_text_input(&tp, text, &text_len, input);
  return rebuild_text();
}

EMSCRIPTEN_KEEPALIVE
int prospero_append_newline(void) {
  if (ensure_initialized() != 0) return -1;
  append_text_char(text, &text_len, '\n');
  return rebuild_text();
}

EMSCRIPTEN_KEEPALIVE
int prospero_backspace(void) {
  if (ensure_initialized() != 0) return -1;
  backspace_text(text, &text_len);
  return rebuild_text();
}

EMSCRIPTEN_KEEPALIVE
int prospero_render(void) {
  if (ensure_initialized() != 0) return -1;
  double t0 = emscripten_get_now();
#ifdef _OPENMP
#pragma omp parallel
  {
    Worker w;
    vm_worker_init(&w);
    vm_render_root_frame(&w, tp.root);
  }
#else
  Worker w;
  vm_worker_init(&w);
  vm_render_root_frame(&w, tp.root);
#endif
  last_render_ms = emscripten_get_now() - t0;
  return (int)(last_render_ms * 1000.0);
}

EMSCRIPTEN_KEEPALIVE
const uint8_t *prospero_image_data(void) {
  return vm_image_data();
}

EMSCRIPTEN_KEEPALIVE
int prospero_image_width(void) {
  return IMAGE_W;
}

EMSCRIPTEN_KEEPALIVE
int prospero_image_height(void) {
  return IMAGE_H;
}

EMSCRIPTEN_KEEPALIVE
int prospero_instruction_count(void) {
  return vm_instruction_count();
}

EMSCRIPTEN_KEEPALIVE
int prospero_root(void) {
  return tp.root;
}

EMSCRIPTEN_KEEPALIVE
int prospero_text_len(void) {
  return (int)text_len;
}

EMSCRIPTEN_KEEPALIVE
int prospero_max_threads(void) {
#ifdef _OPENMP
  return omp_get_max_threads();
#else
  return 1;
#endif
}

EMSCRIPTEN_KEEPALIVE
double prospero_last_render_ms(void) {
  return last_render_ms;
}
