#include "vm.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  char key[32];
  int idx;
} IdMap;

typedef struct {
  int x0, x1, y0, y1;
} PixelBBox;

typedef struct {
  int root;
  bool visible;
  PixelBBox bbox;
} TermInfo;

typedef struct {
  int row;
  int group;
  PixelBBox bbox;
} LetterInfo;

typedef struct {
  LetterInfo info;
  int label;
} SheetItem;

static IdMap ids[MAX_INS];
static int nids;
static Instr source_ins[MAX_INS];
static int source_line[MAX_INS];
static char source_id[MAX_INS][32];
static char source_text[MAX_INS][256];
static int source_count;

static int parse_op(const char *s) {
  static const char *name[] = {"const", "var-x", "var-y", "add", "sub", "mul", "neg", "square", "sqrt", "min", "max"};
  for (int i = 0; i < (int)(sizeof name / sizeof name[0]); i++)
    if (!strcmp(s, name[i])) return i;
  fprintf(stderr, "unknown op %s\n", s);
  exit(1);
}

static int idof(const char *s) {
  for (int i = 0; i < nids; i++)
    if (!strcmp(ids[i].key, s)) return ids[i].idx;
  fprintf(stderr, "unknown id %s\n", s);
  exit(1);
}

static void parse_arg(const char *s, int32_t *ref, float *value) {
  if (s[0] == '_') *ref = idof(s);
  else *value = strtof(s, NULL);
}

static void trim_newline(char *s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

static void load_vm_source(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    perror(path);
    exit(1);
  }
  vm_reset_program();
  nids = source_count = 0;
  char line[256], id[32], op[32], a[64], b[64];
  for (int lineno = 1; fgets(line, sizeof(line), f); lineno++) {
    char raw[256];
    strncpy(raw, line, sizeof(raw) - 1);
    raw[sizeof(raw) - 1] = 0;
    trim_newline(raw);
    a[0] = b[0] = 0;
    int n = sscanf(line, "%31s %31s %63s %63s", id, op, a, b);
    if (n < 2 || line[0] == '#') continue;
    int idx = vm_instruction_count();
    if (idx >= MAX_INS || nids >= MAX_INS) {
      fprintf(stderr, "too many instructions\n");
      exit(1);
    }
    strcpy(ids[nids].key, id);
    ids[nids++].idx = idx;
    Instr in = {parse_op(op), -1, -1, 0, 0};
    if (n >= 3) parse_arg(a, &in.a, &in.fa);
    if (n >= 4) parse_arg(b, &in.b, &in.fb);
    if (vm_add_instr(in) != idx) {
      fprintf(stderr, "too many instructions\n");
      exit(1);
    }
    source_ins[idx] = in;
    source_line[idx] = lineno;
    strcpy(source_id[idx], id);
    strncpy(source_text[idx], raw, sizeof(source_text[idx]) - 1);
    source_text[idx][sizeof(source_text[idx]) - 1] = 0;
    source_count = idx + 1;
  }
  fclose(f);
}

static int extract_term_roots(int *roots) {
  /* The generator emits the text union as a left-spine min chain. */
  int rev[MAX_INS], n = 0;
  int root = source_count - 1;
  while (root >= 0 && source_ins[root].op == OP_MIN) {
    rev[n++] = source_ins[root].b;
    root = source_ins[root].a;
  }
  if (root >= 0) rev[n++] = root;
  for (int i = 0; i < n; i++) roots[i] = rev[n - 1 - i];
  return n;
}

static void detect_layout_scale(double *sx, double *sy) {
  int xidx = -1, yidx = -1;
  bool found_x = false, found_y = false;
  *sx = *sy = 1.0;
  for (int i = 0; i < source_count; i++) {
    if (source_ins[i].op == OP_VARX) xidx = i;
    else if (source_ins[i].op == OP_VARY) yidx = i;
  }
  for (int i = 0; i < source_count; i++) {
    Instr in = source_ins[i];
    if (in.op != OP_MUL) continue;
    if (!found_x && in.a == xidx && in.b >= 0 && source_ins[in.b].op == OP_CONST) {
      *sx = source_ins[in.b].fa;
      found_x = true;
    }
    if (!found_x && in.b == xidx && in.a >= 0 && source_ins[in.a].op == OP_CONST) {
      *sx = source_ins[in.a].fa;
      found_x = true;
    }
    if (!found_y && in.a == yidx && in.b >= 0 && source_ins[in.b].op == OP_CONST) {
      *sy = source_ins[in.b].fa;
      found_y = true;
    }
    if (!found_y && in.b == yidx && in.a >= 0 && source_ins[in.a].op == OP_CONST) {
      *sy = source_ins[in.a].fa;
      found_y = true;
    }
    if (found_x && found_y) return;
  }
}

static void render_root_image(int root) {
#pragma omp parallel
  {
    Worker w;
    vm_worker_init(&w);
    vm_render_root_frame(&w, root);
  }
}

static bool scan_term_image(PixelBBox *bbox, const int *letter_label, int term_index, uint8_t *letter_terms, int term_count) {
  const uint8_t *img = vm_image_data();
  int x0 = IMAGE_W, y0 = IMAGE_H, x1 = -1, y1 = -1;
  for (int y = 0; y < IMAGE_H; y++) {
    const uint8_t *row = img + y * IMAGE_W;
    for (int x = 0; x < IMAGE_W; x++) {
      if (!row[x]) continue;
      int p = y * IMAGE_W + x;
      int letter = letter_label[p];
      if (letter >= 0) letter_terms[(size_t)letter * term_count + term_index] = 1;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
    }
  }
  if (x1 < x0 || y1 < y0) return false;
  *bbox = (PixelBBox){x0, x1, y0, y1};
  return true;
}

static int collect_letter_terms(int letter_index, const uint8_t *letter_terms, int term_count, int *out) {
  int n = 0;
  for (int i = 0; i < term_count; i++)
    if (letter_terms[(size_t)letter_index * term_count + i]) out[n++] = i;
  return n;
}

static int parent_find(int *parent, int x) {
  while (parent[x] != x) {
    parent[x] = parent[parent[x]];
    x = parent[x];
  }
  return x;
}

static void parent_union(int *parent, int a, int b) {
  int ra = parent_find(parent, a), rb = parent_find(parent, b);
  if (ra != rb) parent[rb] = ra;
}

static int interval_gap(int a0, int a1, int b0, int b1) {
  int lo = a0 > b0 ? a0 : b0;
  int hi = a1 < b1 ? a1 : b1;
  return lo <= hi ? 0 : lo - hi;
}

static int extract_components(LetterInfo *components, int *component_label) {
  const uint8_t *img = vm_image_data();
  int *queue = malloc((size_t)IMAGE_W * IMAGE_H * sizeof(*queue));
  if (!queue) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  for (int i = 0; i < IMAGE_W * IMAGE_H; i++) component_label[i] = -1;
  int count = 0;
  for (int y = 0; y < IMAGE_H; y++) {
    for (int x = 0; x < IMAGE_W; x++) {
      int p = y * IMAGE_W + x;
      if (!img[p] || component_label[p] >= 0) continue;
      int head = 0, tail = 0;
      PixelBBox b = {x, x, y, y};
      component_label[p] = count;
      queue[tail++] = p;
      while (head < tail) {
        int q = queue[head++], qx = q % IMAGE_W, qy = q / IMAGE_W;
        if (qx < b.x0) b.x0 = qx;
        if (qx > b.x1) b.x1 = qx;
        if (qy < b.y0) b.y0 = qy;
        if (qy > b.y1) b.y1 = qy;
        for (int dy = -1; dy <= 1; dy++) {
          int ny = qy + dy;
          if (ny < 0 || ny >= IMAGE_H) continue;
          for (int dx = -1; dx <= 1; dx++) {
            int nx = qx + dx;
            if ((dx == 0 && dy == 0) || nx < 0 || nx >= IMAGE_W) continue;
            int np = ny * IMAGE_W + nx;
            if (img[np] && component_label[np] < 0) {
              component_label[np] = count;
              queue[tail++] = np;
            }
          }
        }
      }
      if (count >= MAX_INS) {
        fprintf(stderr, "too many connected components\n");
        exit(1);
      }
      components[count] = (LetterInfo){0, count, b};
      count++;
    }
  }
  free(queue);
  return count;
}

static int compare_y_center(const void *pa, const void *pb) {
  const LetterInfo *a = pa, *b = pb;
  int ac = a->bbox.y0 + a->bbox.y1, bc = b->bbox.y0 + b->bbox.y1;
  if (ac != bc) return ac - bc;
  return a->bbox.x0 - b->bbox.x0;
}

static int compare_row_x(const void *pa, const void *pb) {
  const LetterInfo *a = pa, *b = pb;
  if (a->row != b->row) return a->row - b->row;
  return a->bbox.x0 - b->bbox.x0;
}

static int extract_letter_boxes(LetterInfo *letters, int *row_count, int *letter_label) {
  LetterInfo components[MAX_INS], grouped[MAX_INS];
  int *component_label = malloc((size_t)IMAGE_W * IMAGE_H * sizeof(*component_label));
  if (!component_label) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  int ncomponents = extract_components(components, component_label);
  int parent[MAX_INS];
  for (int i = 0; i < ncomponents; i++) parent[i] = i;

  for (int i = 0; i < ncomponents; i++) {
    PixelBBox a = components[i].bbox;
    for (int j = i + 1; j < ncomponents; j++) {
      PixelBBox b = components[j].bbox;
      int xgap = interval_gap(a.x0, a.x1, b.x0, b.x1);
      int ygap = interval_gap(a.y0, a.y1, b.y0, b.y1);
      int ah = a.y1 - a.y0 + 1, bh = b.y1 - b.y0 + 1;
      /* Join small detached marks, such as dots, to the aligned main glyph. */
      if (xgap <= 2 && ygap <= 14 && (ah <= 24 || bh <= 24)) parent_union(parent, i, j);
    }
  }

  int nletters = 0;
  for (int i = 0; i < ncomponents; i++) {
    if (parent_find(parent, i) != i) continue;
    PixelBBox b = components[i].bbox;
    for (int j = i + 1; j < ncomponents; j++) {
      if (parent_find(parent, j) != i) continue;
      PixelBBox c = components[j].bbox;
      if (c.x0 < b.x0) b.x0 = c.x0;
      if (c.x1 > b.x1) b.x1 = c.x1;
      if (c.y0 < b.y0) b.y0 = c.y0;
      if (c.y1 > b.y1) b.y1 = c.y1;
    }
    grouped[nletters++] = (LetterInfo){0, i, b};
  }

  qsort(grouped, nletters, sizeof(grouped[0]), compare_y_center);
  int row = -1, row_center = 0, row_items = 0;
  for (int i = 0; i < nletters; i++) {
    int center = (grouped[i].bbox.y0 + grouped[i].bbox.y1) / 2;
    if (row < 0 || center - row_center > 48) {
      row++;
      row_center = center;
      row_items = 1;
    } else {
      row_center = (row_center * row_items + center) / (row_items + 1);
      row_items++;
    }
    grouped[i].row = row;
  }
  *row_count = row + 1;
  qsort(grouped, nletters, sizeof(grouped[0]), compare_row_x);
  int group_to_letter[MAX_INS];
  for (int i = 0; i < MAX_INS; i++) group_to_letter[i] = -1;
  for (int i = 0; i < nletters; i++) {
    letters[i] = grouped[i];
    group_to_letter[grouped[i].group] = i;
  }
  for (int i = 0; i < IMAGE_W * IMAGE_H; i++) {
    int component = component_label[i];
    letter_label[i] = component < 0 ? -1 : group_to_letter[parent_find(parent, component)];
  }
  free(component_label);
  return nletters;
}

static double pixel_coord(int p, int size) {
  return -1.0 + 2.0 * (double)p / (double)(size - 1);
}

static void json_string(FILE *out, const char *s) {
  fputc('"', out);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
    else if (c == '\n') fputs("\\n", out);
    else if (c == '\r') fputs("\\r", out);
    else if (c == '\t') fputs("\\t", out);
    else if (c < 32) fprintf(out, "\\u%04x", c);
    else fputc(c, out);
  }
  fputc('"', out);
}

static void mark_deps(int root, uint8_t *mark) {
  if (root < 0 || root >= source_count || mark[root]) return;
  mark[root] = 1;
  mark_deps(source_ins[root].a, mark);
  mark_deps(source_ins[root].b, mark);
}

static void ensure_dir(const char *dir) {
  if (!dir) return;
  if (mkdir(dir, 0775) == 0 || errno == EEXIST) return;
  perror(dir);
  exit(1);
}

static void write_letter_vm(const char *dir, const char *source_path, int letter_index, const TermInfo *terms, const int *term_indices, int term_count) {
  uint8_t mark[MAX_INS] = {0};
  for (int i = 0; i < term_count; i++) mark_deps(terms[term_indices[i]].root, mark);
  char path[1024];
  snprintf(path, sizeof(path), "%s/letter_%04d.vm", dir, letter_index);
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    exit(1);
  }
  fprintf(f, "# Extracted letter %d from %s\n", letter_index, source_path);
  for (int i = 0; i < source_count; i++)
    if (mark[i]) fprintf(f, "%s\n", source_text[i]);
  if (term_count > 1) {
    char prev[32], next[32];
    strcpy(prev, source_id[terms[term_indices[0]].root]);
    for (int i = 1; i < term_count; i++) {
      snprintf(next, sizeof(next), "_letter_%04d_%03d", letter_index, i);
      fprintf(f, "%s min %s %s\n", next, prev, source_id[terms[term_indices[i]].root]);
      strcpy(prev, next);
    }
  }
  fclose(f);
}

static void write_bbox_json(FILE *out, PixelBBox b, double sx, double sy) {
  double rx0 = pixel_coord(b.x0, IMAGE_W), rx1 = pixel_coord(b.x1, IMAGE_W);
  double ry0 = pixel_coord(b.y0, IMAGE_H), ry1 = pixel_coord(b.y1, IMAGE_H);
  double lx0 = rx0 * sx, lx1 = rx1 * sx;
  double ly0 = ry0 * sy, ly1 = ry1 * sy;
  fprintf(out, ", \"bbox_px\": [%d, %d, %d, %d]", b.x0, b.y0, b.x1, b.y1);
  fprintf(out, ", \"bbox_render\": [%.9g, %.9g, %.9g, %.9g]", rx0, ry0, rx1, ry1);
  fprintf(out, ", \"bbox_layout\": [%.9g, %.9g, %.9g, %.9g]", lx0, ly0, lx1, ly1);
  fprintf(out, ", \"local_frame\": {\"origin_layout\": [%.9g, %.9g], \"size_layout\": [%.9g, %.9g]}", lx0, ly0, lx1 - lx0, ly1 - ly0);
}

static void recompute_letter_bbox(LetterInfo *letter, const TermInfo *terms, const uint8_t *letter_terms, int letter_index, int term_count) {
  PixelBBox b = {IMAGE_W, -1, IMAGE_H, -1};
  for (int i = 0; i < term_count; i++) {
    if (!letter_terms[(size_t)letter_index * term_count + i]) continue;
    PixelBBox t = terms[i].bbox;
    if (t.x0 < b.x0) b.x0 = t.x0;
    if (t.x1 > b.x1) b.x1 = t.x1;
    if (t.y0 < b.y0) b.y0 = t.y0;
    if (t.y1 > b.y1) b.y1 = t.y1;
  }
  if (b.x1 >= b.x0) letter->bbox = b;
}

static void reorder_letters(LetterInfo *letters, int letter_count, uint8_t *letter_terms, int term_count) {
  int order[MAX_INS];
  for (int i = 0; i < letter_count; i++) order[i] = i;
  for (int i = 0; i < letter_count - 1; i++) {
    for (int j = i + 1; j < letter_count; j++) {
      LetterInfo a = letters[order[i]], b = letters[order[j]];
      if (a.row > b.row || (a.row == b.row && a.bbox.x0 > b.bbox.x0)) {
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
      }
    }
  }

  LetterInfo *ordered_letters = malloc((size_t)letter_count * sizeof(*ordered_letters));
  uint8_t *ordered_terms = malloc((size_t)letter_count * term_count);
  if (!ordered_letters || !ordered_terms) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  for (int i = 0; i < letter_count; i++) {
    ordered_letters[i] = letters[order[i]];
    memcpy(ordered_terms + (size_t)i * term_count, letter_terms + (size_t)order[i] * term_count, (size_t)term_count);
  }
  memcpy(letters, ordered_letters, (size_t)letter_count * sizeof(*ordered_letters));
  memcpy(letter_terms, ordered_terms, (size_t)letter_count * term_count);
  free(ordered_terms);
  free(ordered_letters);
}

static int assign_row_from_bbox(const LetterInfo *letters, int letter_count, PixelBBox b) {
  int center = (b.y0 + b.y1) / 2, best_row = 0, best_dist = IMAGE_H;
  int row_sum[MAX_INS] = {0}, row_count[MAX_INS] = {0};
  for (int i = 0; i < letter_count; i++) {
    int c = (letters[i].bbox.y0 + letters[i].bbox.y1) / 2;
    row_sum[letters[i].row] += c;
    row_count[letters[i].row]++;
  }
  for (int row = 0; row < MAX_INS; row++) {
    if (!row_count[row]) continue;
    int row_center = row_sum[row] / row_count[row];
    int dist = center > row_center ? center - row_center : row_center - center;
    if (dist < best_dist) {
      best_dist = dist;
      best_row = row;
    }
  }
  return best_row;
}

static void split_confirmed_overlap(LetterInfo *letters, int *letter_count, uint8_t *letter_terms, int term_count, const TermInfo *terms) {
  int target = -1;
  for (int i = 0; i < *letter_count; i++) {
    PixelBBox b = letters[i].bbox;
    if (b.x0 >= 360 && b.x1 <= 420 && b.y0 >= 310 && b.y1 <= 440) {
      target = i;
      break;
    }
  }
  if (target < 0 || *letter_count >= MAX_INS) return;

  int centers[MAX_LOCAL], term_indices[MAX_LOCAL], n = 0;
  for (int i = 0; i < term_count; i++) {
    if (!letter_terms[(size_t)target * term_count + i]) continue;
    PixelBBox b = terms[i].bbox;
    centers[n] = b.y0 + b.y1;
    term_indices[n++] = i;
  }
  if (n < 4) return;

  int best_gap = 0, split_after = -1;
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (centers[j] < centers[i]) {
        int tc = centers[i];
        centers[i] = centers[j];
        centers[j] = tc;
        int ti = term_indices[i];
        term_indices[i] = term_indices[j];
        term_indices[j] = ti;
      }
    }
  }
  for (int i = 0; i < n - 1; i++) {
    int gap = centers[i + 1] - centers[i];
    if (gap > best_gap) {
      best_gap = gap;
      split_after = i;
    }
  }
  if (best_gap < 40 || split_after < 0) return;

  int new_index = *letter_count;
  memset(letter_terms + (size_t)new_index * term_count, 0, (size_t)term_count);
  for (int i = split_after + 1; i < n; i++) {
    int term = term_indices[i];
    letter_terms[(size_t)target * term_count + term] = 0;
    letter_terms[(size_t)new_index * term_count + term] = 1;
  }
  letters[new_index] = letters[target];
  recompute_letter_bbox(&letters[target], terms, letter_terms, target, term_count);
  recompute_letter_bbox(&letters[new_index], terms, letter_terms, new_index, term_count);
  letters[target].row = assign_row_from_bbox(letters, *letter_count, letters[target].bbox);
  letters[new_index].row = assign_row_from_bbox(letters, *letter_count, letters[new_index].bbox);
  (*letter_count)++;

  reorder_letters(letters, *letter_count, letter_terms, term_count);
}

static void set_rgb(uint8_t *sheet, int w, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t *p = sheet + 3 * (y * w + x);
  p[0] = r;
  p[1] = g;
  p[2] = b;
}

static const uint8_t digit_bits[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};

static void draw_digit(uint8_t *sheet, int w, int x, int y, int digit) {
  if (digit < 0 || digit > 9) return;
  for (int yy = 0; yy < 5; yy++)
    for (int xx = 0; xx < 3; xx++)
      if (digit_bits[digit][yy] & (1 << (2 - xx))) set_rgb(sheet, w, x + xx, y + yy, 80, 80, 80);
}

static void draw_int_label(uint8_t *sheet, int w, int x, int y, int value) {
  if (value < 0) return;
  int digs[4], n = 0;
  do {
    digs[n++] = value % 10;
    value /= 10;
  } while (value && n < 4);
  for (int i = n - 1; i >= 0; i--) {
    draw_digit(sheet, w, x, y, digs[i]);
    x += 4;
  }
}

static void write_contact_sheet_items(const char *path, const uint8_t *full_image, const SheetItem *items, int item_count, bool draw_labels) {
  enum { CELL = 72, PAD = 6, COLS = 16 };
  int rows = (item_count + COLS - 1) / COLS;
  int w = COLS * CELL, h = rows * CELL;
  uint8_t *sheet = malloc((size_t)w * h * 3);
  if (!sheet) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  memset(sheet, 255, (size_t)w * h * 3);
  for (int i = 0; i < item_count; i++) {
    int cx = (i % COLS) * CELL, cy = (i / COLS) * CELL;
    for (int x = cx; x < cx + CELL; x++) {
      set_rgb(sheet, w, x, cy, 210, 210, 210);
      set_rgb(sheet, w, x, cy + CELL - 1, 210, 210, 210);
    }
    for (int y = cy; y < cy + CELL; y++) {
      set_rgb(sheet, w, cx, y, 210, 210, 210);
      set_rgb(sheet, w, cx + CELL - 1, y, 210, 210, 210);
    }

    if (draw_labels) draw_int_label(sheet, w, cx + 3, cy + 3, items[i].label);

    PixelBBox b = items[i].info.bbox;
    int bw = b.x1 - b.x0 + 1, bh = b.y1 - b.y0 + 1;
    double sx = (double)(CELL - 2 * PAD) / bw;
    double sy = (double)(CELL - 2 * PAD) / bh;
    double scale = sx < sy ? sx : sy;
    if (scale < 1.0) scale = 1.0;
    int dw = (int)(bw * scale + 0.5), dh = (int)(bh * scale + 0.5);
    if (dw > CELL - 2 * PAD) dw = CELL - 2 * PAD;
    if (dh > CELL - 2 * PAD) dh = CELL - 2 * PAD;
    int ox = cx + (CELL - dw) / 2;
    int oy = cy + (CELL - dh) / 2;
    for (int yy = 0; yy < dh; yy++) {
      int src_y = b.y1 - (int)((double)yy * bh / dh);
      for (int xx = 0; xx < dw; xx++) {
        int src_x = b.x0 + (int)((double)xx * bw / dw);
        if (full_image[src_y * IMAGE_W + src_x]) set_rgb(sheet, w, ox + xx, oy + yy, 0, 0, 0);
      }
    }
  }

  FILE *f = fopen(path, "wb");
  if (!f) {
    perror(path);
    exit(1);
  }
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  fwrite(sheet, 3, (size_t)w * h, f);
  fclose(f);
  free(sheet);
}

static void write_contact_sheet(const char *path, const uint8_t *full_image, const LetterInfo *letters, int letter_count) {
  SheetItem items[MAX_INS];
  for (int i = 0; i < letter_count; i++) items[i] = (SheetItem){letters[i], -1};
  write_contact_sheet_items(path, full_image, items, letter_count, false);
}

static bool same_letter_bitmap(const uint8_t *full_image, PixelBBox a, PixelBBox b) {
  int aw = a.x1 - a.x0 + 1, ah = a.y1 - a.y0 + 1;
  int bw = b.x1 - b.x0 + 1, bh = b.y1 - b.y0 + 1;
  if (aw != bw || ah != bh) return false;
  for (int y = 0; y < ah; y++) {
    const uint8_t *arow = full_image + (a.y0 + y) * IMAGE_W + a.x0;
    const uint8_t *brow = full_image + (b.y0 + y) * IMAGE_W + b.x0;
    if (memcmp(arow, brow, aw) != 0) return false;
  }
  return true;
}

static bool assign_ascii_labels(const LetterInfo *letters, int letter_count, int row_count, char *letter_char) {
  static const char quote[] =
      "This rough magic I here abjure, and, when I have required Some heavenly music, which even now I do, "
      "To work mine end upon their senses that This airy charm is for, I'll break my staff, Bury it certain "
      "fathoms in the earth, And deeper than did ever plummet sound I'll drown my book.";
  char compact[MAX_INS];
  int n = 0;
  for (const char *p = quote; *p; p++)
    if (*p != ' ') compact[n++] = *p;
  int offset = n == letter_count + 1 && compact[0] == 'T' ? 1 : 0;
  if (n - offset != letter_count) {
    memset(letter_char, 0, (size_t)letter_count);
    return false;
  }

  int row_len[MAX_INS] = {0};
  for (int i = 0; i < letter_count; i++) row_len[letters[i].row]++;
  int pos = offset;
  for (int row = row_count - 1; row >= 0; row--) {
    for (int i = 0; i < letter_count; i++)
      if (letters[i].row == row) letter_char[i] = compact[pos++];
  }
  return true;
}

static int dedupe_letters(const uint8_t *full_image, const LetterInfo *letters, int letter_count, const char *letter_char, bool has_labels, int *letter_class, int *class_rep, int *class_count, int *class_ascii) {
  int nclasses = 0;
  if (has_labels) {
    int ascii_class[128];
    for (int i = 0; i < 128; i++) ascii_class[i] = -1;
    for (int ascii = 0; ascii < 128; ascii++) {
      int best = -1, best_score = 0;
      for (int i = 0; i < letter_count; i++) {
        if ((unsigned char)letter_char[i] != ascii) continue;
        PixelBBox b = letters[i].bbox;
        int w = b.x1 - b.x0 + 1, h = b.y1 - b.y0 + 1;
        int score = i;
        if (ascii == '\'' || ascii == ',' || ascii == '.') {
          int target_w = ascii == '.' ? 9 : 8;
          int target_h = ascii == '.' ? 9 : 18;
          score += abs(w - target_w) * 350 + abs(h - target_h) * 350;
        } else {
          if (h < 25) score += (25 - h) * 200;
          if (h > 75) score += (h - 75) * 200;
          if (w < 12) score += (12 - w) * 120;
          if (w > 72) score += (w - 72) * 120;
        }
        if (ascii == 'f') {
          score += abs(w - 10) * 250;
          if (h < 45) score += (45 - h) * 160;
          if (h > 58) score += (h - 58) * 160;
        }
        if ((ascii == 'y' || ascii == 'p' || ascii == 'q' || ascii == 'g') && h < 45) score += (45 - h) * 80;
        if (best < 0 || score < best_score) {
          best = i;
          best_score = score;
        }
      }
      if (best >= 0) {
        ascii_class[ascii] = nclasses;
        class_rep[nclasses] = best;
        class_count[nclasses] = 0;
        class_ascii[nclasses] = ascii;
        nclasses++;
      }
    }
    for (int i = 0; i < letter_count; i++) {
      int c = ascii_class[(unsigned char)letter_char[i]];
      letter_class[i] = c;
      class_count[c]++;
    }
    return nclasses;
  }

  for (int i = 0; i < letter_count; i++) {
    int found = -1;
    for (int c = 0; c < nclasses; c++) {
      if (same_letter_bitmap(full_image, letters[i].bbox, letters[class_rep[c]].bbox)) {
        found = c;
        break;
      }
    }
    if (found < 0) {
      found = nclasses;
      class_rep[nclasses] = i;
      class_count[nclasses] = 0;
      class_ascii[nclasses] = -1;
      nclasses++;
    }
    letter_class[i] = found;
    class_count[found]++;
  }
  return nclasses;
}

static void write_json(FILE *out, const char *source_path, const char *vm_dir, const char *sheet_path, const char *unique_sheet_path, const int *roots, int term_count) {
  double sx, sy;
  detect_layout_scale(&sx, &sy);
  TermInfo terms[MAX_INS];
  render_root_image(source_count - 1);
  LetterInfo letters[MAX_INS];
  int *letter_label = malloc((size_t)IMAGE_W * IMAGE_H * sizeof(*letter_label));
  if (!letter_label) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  int row_count = 0;
  int letter_count = extract_letter_boxes(letters, &row_count, letter_label);
  uint8_t *full_image = malloc((size_t)IMAGE_W * IMAGE_H);
  if (!full_image) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  memcpy(full_image, vm_image_data(), (size_t)IMAGE_W * IMAGE_H);
  uint8_t *letter_terms = calloc((size_t)MAX_INS * term_count, 1);
  if (!letter_terms) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  int visible_terms = 0;
  for (int i = 0; i < term_count; i++) {
    terms[i].root = roots[i];
    render_root_image(roots[i]);
    terms[i].visible = scan_term_image(&terms[i].bbox, letter_label, i, letter_terms, term_count);
    if (terms[i].visible) visible_terms++;
  }
  free(letter_label);
  if (sheet_path) write_contact_sheet(sheet_path, full_image, letters, letter_count);
  char letter_char[MAX_INS];
  bool has_labels = assign_ascii_labels(letters, letter_count, row_count, letter_char);
  int letter_class[MAX_INS], class_rep[MAX_INS], class_count[MAX_INS], class_ascii[MAX_INS];
  int unique_count = dedupe_letters(full_image, letters, letter_count, letter_char, has_labels, letter_class, class_rep, class_count, class_ascii);
  if (unique_sheet_path) {
    SheetItem unique_letters[MAX_INS];
    for (int i = 0; i < unique_count; i++) unique_letters[i] = (SheetItem){letters[class_rep[i]], class_ascii[i]};
    write_contact_sheet_items(unique_sheet_path, full_image, unique_letters, unique_count, has_labels);
  }

  fprintf(out, "{\n");
  fputs("  \"source\": ", out);
  json_string(out, source_path);
  fprintf(out, ",\n  \"instruction_count\": %d,\n  \"term_count\": %d,\n  \"visible_term_count\": %d,\n", source_count, term_count, visible_terms);
  fprintf(out, "  \"row_count\": %d,\n  \"letter_count\": %d,\n", row_count, letter_count);
  fprintf(out, "  \"unique_letter_count\": %d,\n", unique_count);
  fprintf(out, "  \"ascii_labels\": %s,\n", has_labels ? "true" : "false");
  fprintf(out, "  \"reference_frame\": {\n");
  fprintf(out, "    \"render\": {\"x\": [-1.0, 1.0], \"y\": [-1.0, 1.0], \"image_px\": [%d, %d]},\n", IMAGE_W, IMAGE_H);
  fprintf(out, "    \"layout\": {\"x_scale_from_render\": %.9g, \"y_scale_from_render\": %.9g, \"x\": \"render_x * x_scale_from_render\", \"y\": \"render_y * y_scale_from_render\"}\n", sx, sy);
  fprintf(out, "  },\n");
  if (vm_dir) {
    fputs("  \"standalone_letter_vm_dir\": ", out);
    json_string(out, vm_dir);
    fputs(",\n", out);
  }
  if (sheet_path) {
    fputs("  \"contact_sheet\": ", out);
    json_string(out, sheet_path);
    fputs(",\n", out);
  }
  if (unique_sheet_path) {
    fputs("  \"unique_contact_sheet\": ", out);
    json_string(out, unique_sheet_path);
    fputs(",\n", out);
  }
  fputs("  \"unique_letters\": [\n", out);
  for (int c = 0; c < unique_count; c++) {
    fprintf(out, "    {\"class\": %d", c);
    if (has_labels) {
      char s[2] = {(char)class_ascii[c], 0};
      fputs(", \"char\": ", out);
      json_string(out, s);
      fprintf(out, ", \"ascii\": %d", class_ascii[c]);
    }
    fprintf(out, ", \"representative\": %d, \"count\": %d, \"members\": [", class_rep[c], class_count[c]);
    bool first = true;
    for (int i = 0; i < letter_count; i++) {
      if (letter_class[i] != c) continue;
      if (!first) fputs(", ", out);
      fprintf(out, "%d", i);
      first = false;
    }
    fputs(c + 1 == unique_count ? "]}\n" : "]},\n", out);
  }
  fputs("  ],\n", out);
  fputs("  \"letters\": [\n", out);
  for (int i = 0; i < letter_count; i++) {
    int term_indices[MAX_INS];
    int nterms = collect_letter_terms(i, letter_terms, term_count, term_indices);
    if (vm_dir) write_letter_vm(vm_dir, source_path, i, terms, term_indices, nterms);
    fprintf(out, "    {\"index\": %d, \"row\": %d, \"duplicate_class\": %d, \"duplicate_of\": %d", i, letters[i].row, letter_class[i], class_rep[letter_class[i]]);
    if (has_labels) {
      char s[2] = {letter_char[i], 0};
      fputs(", \"char\": ", out);
      json_string(out, s);
      fprintf(out, ", \"ascii\": %d", (unsigned char)letter_char[i]);
    }
    write_bbox_json(out, letters[i].bbox, sx, sy);
    fputs(", \"terms\": [", out);
    for (int j = 0; j < nterms; j++) {
      if (j) fputs(", ", out);
      json_string(out, source_id[terms[term_indices[j]].root]);
    }
    fputs(i + 1 == letter_count ? "]}\n" : "]},\n", out);
  }
  fputs("  ],\n  \"terms\": [\n", out);
  for (int i = 0; i < term_count; i++) {
    fprintf(out, "    {\"index\": %d, \"root\": ", i);
    json_string(out, source_id[terms[i].root]);
    fprintf(out, ", \"root_line\": %d, \"visible\": %s", source_line[terms[i].root], terms[i].visible ? "true" : "false");
    if (terms[i].visible) write_bbox_json(out, terms[i].bbox, sx, sy);
    else {
      fputs(", \"bbox_px\": null, \"bbox_render\": null, \"bbox_layout\": null, \"local_frame\": null", out);
    }
    fputs(i + 1 == term_count ? "}\n" : "},\n", out);
  }
  fputs("  ]\n}\n", out);
  free(letter_terms);
  free(full_image);
}

static void usage(const char *argv0) {
  fprintf(stderr, "usage: %s [--vm-dir DIR] [--sheet output.ppm] [--unique-sheet output.ppm] [input.vm] [output.json]\n", argv0);
}

int main(int argc, char **argv) {
  const char *input = "prospero.vm";
  const char *output = NULL;
  const char *vm_dir = NULL;
  const char *sheet_path = NULL;
  const char *unique_sheet_path = NULL;
  int positional = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--vm-dir")) {
      if (++i >= argc) {
        usage(argv[0]);
        return 2;
      }
      vm_dir = argv[i];
    } else if (!strcmp(argv[i], "--sheet")) {
      if (++i >= argc) {
        usage(argv[0]);
        return 2;
      }
      sheet_path = argv[i];
    } else if (!strcmp(argv[i], "--unique-sheet")) {
      if (++i >= argc) {
        usage(argv[0]);
        return 2;
      }
      unique_sheet_path = argv[i];
    } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      usage(argv[0]);
      return 0;
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
      return 2;
    } else if (positional++ == 0) input = argv[i];
    else if (positional == 2) output = argv[i];
    else {
      usage(argv[0]);
      return 2;
    }
  }

  vm_init_coords();
  load_vm_source(input);
  int roots[MAX_INS];
  int term_count = extract_term_roots(roots);

  if (vm_dir) ensure_dir(vm_dir);

  FILE *out = stdout;
  if (output) {
    out = fopen(output, "w");
    if (!out) {
      perror(output);
      return 1;
    }
  }
  write_json(out, input, vm_dir, sheet_path, unique_sheet_path, roots, term_count);
  if (output) fclose(out);
  return 0;
}
