#include "vm.h"

#include <SDL2/SDL.h>

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  TITLE_WARMUP_FRAMES = 32,
  TARGET_FPS = 60,
  MAX_SOURCE_IDS = MAX_INS,
  MAX_GLYPHS = 256,
  MAX_GLYPH_TERMS = 64,
  MAX_TEXT_LEN = 4096
};

typedef struct {
  char key[32];
  int idx;
} IdMap;

typedef struct {
  Instr in;
  char id[32];
} SourceInstr;

typedef struct {
  SourceInstr ins[MAX_INS];
  IdMap ids[MAX_SOURCE_IDS];
  int nins, nids;
} SourceProgram;

typedef struct {
  int present;
  int letter_index;
  int x0, y0, x1, y1;
  float sx0, sy0, sx1, sy1;
  int nterms;
  char term_id[MAX_GLYPH_TERMS][32];
  int term_idx[MAX_GLYPH_TERMS];
} Glyph;

typedef struct {
  SourceProgram src;
  Glyph glyph[MAX_GLYPHS];
  int out_x, out_y, root;
} TextProgram;

typedef struct {
  int source_idx;
  int out_idx;
} MemoEntry;

static int emit_failed;

static const char *DEFAULT_VIEWER_TEXT =
    "But this rough magic I here abjure, and when I have required Some heavenly music, "
    "which even now I do, To work mine end upon their senses that This airy charm is for, "
    "I'll break my staff, bury it certain fathoms in the earth, And deeper than did ever "
    "plummet sound I'll drown my book.";

static inline double now_sec(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

static int parse_op_local(const char *s) {
  static const char *name[] = {"const", "var-x", "var-y", "add", "sub", "mul", "neg", "square", "sqrt", "min", "max"};
  for (int i = 0; i < (int)(sizeof name / sizeof name[0]); i++)
    if (!strcmp(s, name[i])) return i;
  return -1;
}

static int source_idof(const SourceProgram *p, const char *s) {
  for (int i = 0; i < p->nids; i++)
    if (!strcmp(p->ids[i].key, s)) return p->ids[i].idx;
  return -1;
}

static void parse_source_arg(const SourceProgram *p, const char *s, int32_t *ref, float *value) {
  if (s[0] == '_') *ref = source_idof(p, s);
  else *value = strtof(s, NULL);
}

static int load_source_vm(const char *path, SourceProgram *p) {
  memset(p, 0, sizeof(*p));
  FILE *f = fopen(path, "r");
  if (!f) {
    perror(path);
    return -1;
  }
  char line[256], id[32], op[32], a[64], b[64];
  while (fgets(line, sizeof(line), f)) {
    a[0] = b[0] = 0;
    int n = sscanf(line, "%31s %31s %63s %63s", id, op, a, b);
    if (n < 2 || line[0] == '#') continue;
    int opcode = parse_op_local(op);
    if (opcode < 0) {
      fprintf(stderr, "%s: unknown op %s\n", path, op);
      fclose(f);
      return -1;
    }
    if (p->nins >= MAX_INS || p->nids >= MAX_SOURCE_IDS) {
      fprintf(stderr, "%s: too many instructions\n", path);
      fclose(f);
      return -1;
    }
    int idx = p->nins++;
    strcpy(p->ins[idx].id, id);
    p->ins[idx].in = (Instr){opcode, -1, -1, 0, 0};
    strcpy(p->ids[p->nids].key, id);
    p->ids[p->nids++].idx = idx;
    if (n >= 3) parse_source_arg(p, a, &p->ins[idx].in.a, &p->ins[idx].in.fa);
    if (n >= 4) parse_source_arg(p, b, &p->ins[idx].in.b, &p->ins[idx].in.fb);
  }
  fclose(f);
  return 0;
}

static int load_glyph_table(const char *path, const SourceProgram *src, Glyph glyph[MAX_GLYPHS]) {
  memset(glyph, 0, sizeof(Glyph) * MAX_GLYPHS);
  FILE *f = fopen(path, "r");
  if (!f) {
    perror(path);
    return -1;
  }
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char *tok = strtok(line, " \t\r\n");
    if (!tok || strcmp(tok, "glyph")) continue;
    char *parts[80];
    int n = 0;
    while (n < (int)(sizeof parts / sizeof parts[0]) && (parts[n] = strtok(NULL, " \t\r\n"))) n++;
    if (n < 12) {
      fprintf(stderr, "%s: malformed glyph row\n", path);
      fclose(f);
      return -1;
    }
    int code = atoi(parts[0]);
    if (code < 0 || code >= MAX_GLYPHS) continue;
    Glyph *g = &glyph[code];
    g->present = 1;
    g->letter_index = atoi(parts[1]);
    g->x0 = atoi(parts[2]);
    g->y0 = atoi(parts[3]);
    g->x1 = atoi(parts[4]);
    g->y1 = atoi(parts[5]);
    g->sx0 = strtof(parts[6], NULL);
    g->sy0 = strtof(parts[7], NULL);
    g->sx1 = strtof(parts[8], NULL);
    g->sy1 = strtof(parts[9], NULL);
    g->nterms = atoi(parts[10]);
    if (g->nterms < 0 || g->nterms > MAX_GLYPH_TERMS || n < 11 + g->nterms) {
      fprintf(stderr, "%s: bad term count for glyph %d\n", path, code);
      fclose(f);
      return -1;
    }
    for (int i = 0; i < g->nterms; i++) {
      strncpy(g->term_id[i], parts[11 + i], sizeof(g->term_id[i]) - 1);
      g->term_id[i][sizeof(g->term_id[i]) - 1] = 0;
      g->term_idx[i] = source_idof(src, g->term_id[i]);
      if (g->term_idx[i] < 0) {
        fprintf(stderr, "%s: unknown source term %s\n", path, g->term_id[i]);
        fclose(f);
        return -1;
      }
    }
  }
  fclose(f);
  return 0;
}

static int emit_instr(Op op, int a, int b, float fa, float fb) {
  int idx = vm_add_instr((Instr){op, a, b, fa, fb});
  if (idx < 0) {
    emit_failed = 1;
    fprintf(stderr, "generated bytecode exceeds MAX_INS=%d\n", MAX_INS);
  }
  return idx;
}

static int emit_const(float v) { return emit_instr(OP_CONST, -1, -1, v, 0); }
static int emit2(Op op, int a, int b) { return emit_instr(op, a, b, 0, 0); }

static int emit_union(int a, int b) {
  if (a < 0) return b;
  if (b < 0) return a;
  return emit2(OP_MIN, a, b);
}

static float px_to_render(float px, int size) {
  return -1.0f + 2.0f * px / (float)(size - 1);
}

static int is_descender(int ch) { return ch == 'g' || ch == 'j' || ch == 'p' || ch == 'q' || ch == 'y'; }

static void glyph_metrics(const Glyph *g, int ch, float scale, float *w, float *h, float *baseline) {
  *w = (float)(g->x1 - g->x0 + 1) * scale;
  *h = (float)(g->y1 - g->y0 + 1) * scale;
  if (*w < 1) *w = 1;
  if (*h < 1) *h = 1;
  if (is_descender(ch)) *baseline = fminf(*h, 35.0f * scale);
  else if (ch == '\'') *baseline = 58.0f * scale;
  else *baseline = *h;
}

static float text_width(const char *begin, const char *end, const Glyph glyph[MAX_GLYPHS], float scale, float tracking, float space_width) {
  float width = 0;
  for (const char *p = begin; p < end; p++) {
    unsigned char ch = (unsigned char)*p;
    if (ch == ' ') width += space_width;
    else if (glyph[ch].present) {
      float w, h, b;
      glyph_metrics(&glyph[ch], ch, scale, &w, &h, &b);
      width += w + tracking;
    }
  }
  return width;
}

static int transformed_axis(int out_var, float src0, float src1, float dst0, float dst1) {
  float dst_len = dst1 - dst0;
  if (dst_len > -1e-9f && dst_len < 1e-9f) return emit_const(src0);
  float scale = (src1 - src0) / dst_len;
  int sub = emit2(OP_SUB, out_var, emit_const(dst0));
  int mul = emit2(OP_MUL, sub, emit_const(scale));
  return emit2(OP_ADD, emit_const(src0), mul);
}

static int memo_get(const MemoEntry *memo, int nmemo, int source_idx) {
  for (int i = 0; i < nmemo; i++)
    if (memo[i].source_idx == source_idx) return memo[i].out_idx;
  return -1;
}

static void memo_put(MemoEntry *memo, int *nmemo, int source_idx, int out_idx) {
  if (*nmemo >= MAX_LOCAL) return;
  memo[*nmemo] = (MemoEntry){source_idx, out_idx};
  (*nmemo)++;
}

static int clone_expr(const SourceProgram *src, int source_idx, int x_expr, int y_expr, MemoEntry *memo, int *nmemo) {
  int cached = memo_get(memo, *nmemo, source_idx);
  if (cached >= 0) return cached;
  Instr in = src->ins[source_idx].in;
  if (in.op == OP_VARX) {
    memo_put(memo, nmemo, source_idx, x_expr);
    return x_expr;
  }
  if (in.op == OP_VARY) {
    memo_put(memo, nmemo, source_idx, y_expr);
    return y_expr;
  }
  int a = -1, b = -1;
  if (in.a >= 0) a = clone_expr(src, in.a, x_expr, y_expr, memo, nmemo);
  if (in.b >= 0) b = clone_expr(src, in.b, x_expr, y_expr, memo, nmemo);
  int out = emit_instr(in.op, a, b, in.fa, in.fb);
  memo_put(memo, nmemo, source_idx, out);
  return out;
}

static int emit_glyph(const TextProgram *tp, const Glyph *g, float x, float y, float w, float h) {
  float dx0 = px_to_render(x, IMAGE_W);
  float dx1 = px_to_render(x + w - 1, IMAGE_W);
  float dy0 = px_to_render(y, IMAGE_H);
  float dy1 = px_to_render(y + h - 1, IMAGE_H);
  int src_x = transformed_axis(tp->out_x, g->sx0, g->sx1, dx0, dx1);
  int src_y = transformed_axis(tp->out_y, g->sy1, g->sy0, dy0, dy1);
  MemoEntry memo[MAX_LOCAL];
  int nmemo = 0, root = -1;
  for (int i = 0; i < g->nterms; i++) {
    int term = clone_expr(&tp->src, g->term_idx[i], src_x, src_y, memo, &nmemo);
    root = emit_union(root, term);
  }
  return root;
}

static int build_text_program(TextProgram *tp, const char *text) {
  const float scale = 0.76f, margin_x = 36.0f, margin_y = 54.0f, max_width = 955.0f;
  const float tracking = 4.0f, space_width = 18.0f, line_gap = 9.0f;
  float max_ascent = 0, max_descent = 0;
  for (int ch = 0; ch < MAX_GLYPHS; ch++) {
    if (!tp->glyph[ch].present) continue;
    float w, h, baseline;
    glyph_metrics(&tp->glyph[ch], ch, scale, &w, &h, &baseline);
    if (baseline > max_ascent) max_ascent = baseline;
    if (h - baseline > max_descent) max_descent = h - baseline;
  }
  float line_h = max_ascent + max_descent + line_gap;
  vm_reset_program();
  emit_failed = 0;
  tp->out_x = emit_instr(OP_VARX, -1, -1, 0, 0);
  tp->out_y = emit_instr(OP_VARY, -1, -1, 0, 0);
  float x = margin_x, y = margin_y;
  int root = -1;
  const char *p = text;
  while (*p) {
    while (*p == ' ') p++;
    if (*p == '\n') {
      x = margin_x;
      y += line_h;
      p++;
      continue;
    }
    const char *word = p;
    while (*p && *p != ' ' && *p != '\n') p++;
    if (word == p) continue;
    float word_w = text_width(word, p, tp->glyph, scale, tracking, space_width);
    if (x > margin_x && x + word_w > margin_x + max_width) {
      x = margin_x;
      y += line_h;
    }
    for (const char *q = word; q < p; q++) {
      unsigned char ch = (unsigned char)*q;
      Glyph *g = &tp->glyph[ch];
      if (!g->present) {
        fprintf(stderr, "missing glyph expression for '%c' (%u)\n", isprint(ch) ? ch : '?', ch);
        return -1;
      }
      float gw, gh, baseline;
      glyph_metrics(g, ch, scale, &gw, &gh, &baseline);
      float top = y + max_ascent - baseline;
      int term = emit_glyph(tp, g, x, top, gw, gh);
      if (term < 0) return -1;
      root = emit_union(root, term);
      x += gw + tracking;
    }
    x += space_width;
  }
  if (root < 0) root = emit_const(1.0f);
  tp->root = root;
  return emit_failed || root < 0 ? -1 : 0;
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

static int text_char_supported(const TextProgram *tp, unsigned char ch) {
  return ch == ' ' || ch == '\n' || tp->glyph[ch].present;
}

static unsigned char closest_supported_char(const TextProgram *tp, unsigned char ch) {
  if (text_char_supported(tp, ch)) return ch;
  int best = -1, best_dist = 1000;
  for (int c = 32; c < 127; c++) {
    if (!text_char_supported(tp, (unsigned char)c)) continue;
    int dist = abs((int)c - (int)ch);
    if (dist < best_dist) {
      best = c;
      best_dist = dist;
    }
  }
  return best >= 0 ? (unsigned char)best : ' ';
}

static int append_text_char(char *text, size_t *len, unsigned char ch) {
  if (*len + 1 >= MAX_TEXT_LEN) return 0;
  text[(*len)++] = (char)ch;
  text[*len] = 0;
  return 1;
}

static int append_text_input(const TextProgram *tp, char *text, size_t *len, const char *input) {
  int changed = 0;
  for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
    if (*p >= 32 && *p < 127) changed |= append_text_char(text, len, closest_supported_char(tp, *p));
  }
  return changed;
}

static int backspace_text(char *text, size_t *len) {
  if (*len == 0) return 0;
  (*len)--;
  text[*len] = 0;
  return 1;
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
  memset(&tp, 0, sizeof(tp));
  char text[MAX_TEXT_LEN];
  strncpy(text, initial_text, sizeof(text) - 1);
  text[sizeof(text) - 1] = 0;
  size_t text_len = strlen(text);
  vm_init_coords();
  if (load_source_vm(source_vm, &tp.src) != 0) return 1;
  if (load_glyph_table(glyph_table, &tp.src, tp.glyph) != 0) return 1;
  if (build_text_program(&tp, text) != 0) return 1;
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
          if (build_text_program(&tp, text) == 0) {
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
