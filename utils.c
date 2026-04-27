#include "utils.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int source_idx;
  int out_idx;
} MemoEntry;

static int emit_failed;

const char *DEFAULT_VIEWER_TEXT =
    "But this rough magic I here abjure, and when I have required Some heavenly music, "
    "which even now I do, To work mine end upon their senses that This airy charm is for, "
    "I'll break my staff, bury it certain fathoms in the earth, And deeper than did ever "
    "plummet sound I'll drown my book.";

static int parse_op(const char *s) {
  static const char *name[] = {"const", "var-x", "var-y", "add", "sub", "mul", "neg", "square", "sqrt", "min", "max"};
  for (int i = 0; i < (int)(sizeof name / sizeof name[0]); i++)
    if (!strcmp(s, name[i])) return i;
  return -1;
}

static int idof(const UtilIdMap *ids, int nids, const char *s) {
  for (int i = 0; i < nids; i++)
    if (!strcmp(ids[i].key, s)) return ids[i].idx;
  return -1;
}

static void parse_arg(const UtilIdMap *ids, int nids, const char *s, int32_t *ref, float *value) {
  if (s[0] == '_') *ref = idof(ids, nids, s);
  else *value = strtof(s, NULL);
}

int vm_load_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    perror("fopen");
    return -1;
  }
  vm_reset_program();
  UtilIdMap ids[MAX_INS];
  int nids = 0;
  char line[256], id[32], op[32], a[64], b[64];
  while (fgets(line, sizeof(line), f)) {
    a[0] = b[0] = 0;
    int n = sscanf(line, "%31s %31s %63s %63s", id, op, a, b);
    if (n < 2 || line[0] == '#') continue;
    if (nids >= MAX_INS) {
      fprintf(stderr, "too many instruction ids\n");
      fclose(f);
      return -1;
    }
    int opcode = parse_op(op);
    if (opcode < 0) {
      fprintf(stderr, "unknown op %s\n", op);
      fclose(f);
      return -1;
    }
    int idx = vm_instruction_count();
    strcpy(ids[nids].key, id);
    ids[nids++].idx = idx;
    Instr in = {opcode, -1, -1, 0, 0};
    if (n >= 3) parse_arg(ids, nids, a, &in.a, &in.fa);
    if (n >= 4) parse_arg(ids, nids, b, &in.b, &in.fb);
    if (vm_add_instr(in) != idx) {
      fprintf(stderr, "too many instructions\n");
      fclose(f);
      return -1;
    }
  }
  fclose(f);
  return 0;
}

static int source_idof(const SourceProgram *p, const char *s) {
  return idof(p->ids, p->nids, s);
}

static void parse_source_arg(const SourceProgram *p, const char *s, int32_t *ref, float *value) {
  parse_arg(p->ids, p->nids, s, ref, value);
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
    int opcode = parse_op(op);
    if (opcode < 0) {
      fprintf(stderr, "%s: unknown op %s\n", path, op);
      fclose(f);
      return -1;
    }
    if (p->nins >= MAX_INS || p->nids >= UTIL_MAX_SOURCE_IDS) {
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

static int load_glyph_table(const char *path, const SourceProgram *src, Glyph glyph[UTIL_MAX_GLYPHS]) {
  memset(glyph, 0, sizeof(Glyph) * UTIL_MAX_GLYPHS);
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
    if (code < 0 || code >= UTIL_MAX_GLYPHS) continue;
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
    if (g->nterms < 0 || g->nterms > UTIL_MAX_GLYPH_TERMS || n < 11 + g->nterms) {
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

int text_program_load(TextProgram *tp, const char *source_vm, const char *glyph_table) {
  memset(tp, 0, sizeof(*tp));
  if (load_source_vm(source_vm, &tp->src) != 0) return -1;
  return load_glyph_table(glyph_table, &tp->src, tp->glyph);
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

static float text_width(const char *begin, const char *end, const Glyph glyph[UTIL_MAX_GLYPHS], float scale, float tracking, float space_width) {
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

int text_program_build(TextProgram *tp, const char *text) {
  const float scale = 0.76f, margin_x = 36.0f, margin_y = 54.0f, max_width = 955.0f;
  const float tracking = 4.0f, space_width = 18.0f, line_gap = 9.0f;
  float max_ascent = 0, max_descent = 0;
  for (int ch = 0; ch < UTIL_MAX_GLYPHS; ch++) {
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

int text_char_supported(const TextProgram *tp, unsigned char ch) {
  return ch == ' ' || ch == '\n' || tp->glyph[ch].present;
}

unsigned char closest_supported_char(const TextProgram *tp, unsigned char ch) {
  if (text_char_supported(tp, ch)) return ch;
  int best = -1, best_dist = 1000;
  for (int c = 32; c < 127; c++) {
    if (!text_char_supported(tp, (unsigned char)c)) continue;
    int dist = abs(c - (int)ch);
    if (dist < best_dist) {
      best = c;
      best_dist = dist;
    }
  }
  return best >= 0 ? (unsigned char)best : ' ';
}

int append_text_char(char *text, size_t *len, unsigned char ch) {
  if (*len + 1 >= UTIL_MAX_TEXT_LEN) return 0;
  text[(*len)++] = (char)ch;
  text[*len] = 0;
  return 1;
}

int append_text_input(const TextProgram *tp, char *text, size_t *len, const char *input) {
  int changed = 0;
  for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
    if (*p >= 32 && *p < 127) changed |= append_text_char(text, len, closest_supported_char(tp, *p));
  }
  return changed;
}

int backspace_text(char *text, size_t *len) {
  if (*len == 0) return 0;
  (*len)--;
  text[*len] = 0;
  return 1;
}
