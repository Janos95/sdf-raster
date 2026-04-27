#include "vm.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  MAX_REGION_POINTS = MAX_LOCAL * 2 + 8
};
#define NEG_INF (-1e30f)
#define VEC(dst, n, expr)                                                                                                                                      \
  do { _Pragma("omp simd") for (int ix = 0; ix < (n); ix++)(dst)[ix] = (expr); } while (0)
#define ARG(is_scalar, value, ptr, i) ((is_scalar) ? (value) : (ptr)[i])

typedef enum { MODE_SCALAR, MODE_X, MODE_Y, MODE_FULL } Mode;
typedef struct {
  float x0, x1, y0, y1;
} Box;
typedef struct {
  int x0, x1, y0, y1;
} PixelBox;
typedef struct {
  int n;
  float x[MAX_REGION_POINTS], y[MAX_REGION_POINTS];
} Region;
typedef struct {
  uint16_t n, root;
  int16_t xl, yl;
  uint8_t fill, nx, ny, nf, mode[MAX_LOCAL], arg[MAX_LOCAL], xop[MAX_LOCAL], yop[MAX_LOCAL], fop[MAX_LOCAL];
  Instr ins[MAX_LOCAL];
} Tape;
typedef struct {
  char key[32];
  int idx;
} IdMap;

static Instr ins[MAX_INS];
static int nins, xidx = -1, yidx = -1, frontier[MAX_INS], nfrontier;
static IdMap ids[MAX_INS];
static int nids;
static uint16_t frontier_mark[MAX_INS], frontier_gen = 1;
static uint8_t image[IMAGE_W * IMAGE_H];
static float gx[IMAGE_W], gy[IMAGE_H];

static inline Interval I(float lo, float hi) { return (Interval){lo, hi}; }
static inline bool ok_i(Interval a) { return a.lo <= a.hi; }
static inline Interval inter(Interval a, Interval b) { return I(fmaxf(a.lo, b.lo), fminf(a.hi, b.hi)); }
static inline float min4(float a, float b, float c, float d) { return fminf(fminf(a, b), fminf(c, d)); }
static inline float max4(float a, float b, float c, float d) { return fmaxf(fmaxf(a, b), fmaxf(c, d)); }
static inline Interval iadd(Interval a, Interval b) { return I(a.lo + b.lo, a.hi + b.hi); }
static inline Interval isub(Interval a, Interval b) { return I(a.lo - b.hi, a.hi - b.lo); }
static inline Interval imul(Interval a, Interval b) {
  float p = a.lo * b.lo, q = a.lo * b.hi, r = a.hi * b.lo, s = a.hi * b.hi;
  return I(min4(p, q, r, s), max4(p, q, r, s));
}
static inline Interval ineg(Interval a) { return I(-a.hi, -a.lo); }
static inline Interval isqr(Interval a) {
  if (a.lo <= 0 && a.hi >= 0) return I(0, fmaxf(a.lo * a.lo, a.hi * a.hi));
  return I(fminf(a.lo * a.lo, a.hi * a.hi), fmaxf(a.lo * a.lo, a.hi * a.hi));
}
static inline Interval isqrt_i(Interval a) { return a.hi < 0 ? I(1, 0) : I(sqrtf(fmaxf(0, a.lo)), sqrtf(fmaxf(0, a.hi))); }
static inline Interval idiv(Interval z, Interval d) {
  return I(min4(z.lo / d.lo, z.lo / d.hi, z.hi / d.lo, z.hi / d.hi), max4(z.lo / d.lo, z.lo / d.hi, z.hi / d.lo, z.hi / d.hi));
}
static inline Interval iaff(float ax, float ay, float c, Box b) {
  float x0 = ax * b.x0, x1 = ax * b.x1, y0 = ay * b.y0, y1 = ay * b.y1;
  return I(fminf(x0, x1) + fminf(y0, y1) + c, fmaxf(x0, x1) + fmaxf(y0, y1) + c);
}
static inline float area(Box b) { return fmaxf(0, b.x1 - b.x0) * fmaxf(0, b.y1 - b.y0); }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint8_t mode2(uint8_t a, uint8_t b) {
  if (a == MODE_FULL || b == MODE_FULL) return MODE_FULL;
  if (a == MODE_SCALAR) return b;
  if (b == MODE_SCALAR) return a;
  return a == b ? a : MODE_FULL;
}
static int parse_op(const char *s) {
  static const char *name[] = {"const", "var-x", "var-y", "add", "sub", "mul", "neg", "square", "sqrt", "min", "max"};
  for (int i = 0; i < (int)(sizeof name / sizeof name[0]); i++)
    if (!strcmp(s, name[i])) return i;
  return -1;
}
static int idof(const char *s) {
  for (int i = 0; i < nids; i++)
    if (!strcmp(ids[i].key, s)) return ids[i].idx;
  return -1;
}
static inline void parse_arg(const char *s, int16_t *ref, float *value) {
  if (s[0] == '_') *ref = idof(s);
  else *value = strtof(s, NULL);
}
static inline float eval_scalar(Op op, float a, float b) {
  switch (op) {
  case OP_ADD: return a + b;
  case OP_SUB: return a - b;
  case OP_MUL: return a * b;
  case OP_MIN: return fminf(a, b);
  case OP_MAX: return fmaxf(a, b);
  case OP_NEG: return -a;
  case OP_SQUARE: return a * a;
  case OP_SQRT: return sqrtf(a);
  default: return a;
  }
}

void vm_reset_program(void) {
  nins = nfrontier = 0;
  xidx = yidx = -1;
  frontier_gen = 1;
  memset(frontier_mark, 0, sizeof(frontier_mark));
}
int vm_add_instr(Instr in) {
  if (nins >= MAX_INS) return -1;
  if (in.op == OP_VARX) xidx = nins;
  if (in.op == OP_VARY) yidx = nins;
  ins[nins] = in;
  return nins++;
}
int vm_instruction_count(void) { return nins; }
int vm_frontier_count(void) { return nfrontier; }
const uint8_t *vm_image_data(void) { return image; }
int vm_checksum(void) {
  int sum = 0;
  for (int i = 0; i < IMAGE_W * IMAGE_H; i++) sum += image[i];
  return sum;
}
void vm_worker_init(Worker *w) {
  memset(w, 0, sizeof(*w));
  w->tag = w->mark_id = 1;
}
void vm_init_coords(void) {
  for (int i = 0; i < IMAGE_W; i++) gx[i] = -1 + 2 * (float)i / (IMAGE_W - 1);
  for (int i = 0; i < IMAGE_H; i++) gy[i] = -1 + 2 * (float)i / (IMAGE_H - 1);
}
int vm_load_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    perror("fopen");
    return -1;
  }
  vm_reset_program();
  nids = 0;
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
    if (n >= 3) parse_arg(a, &in.a, &in.fa);
    if (n >= 4) parse_arg(b, &in.b, &in.fb);
    if (vm_add_instr(in) != idx) {
      fprintf(stderr, "too many instructions\n");
      fclose(f);
      return -1;
    }
  }
  fclose(f);
  return 0;
}
static void extract_frontier(void) {
  nfrontier = 0;
  if (++frontier_gen == 0) {
    memset(frontier_mark, 0, sizeof(frontier_mark));
    frontier_gen = 1;
  }
  int st[MAX_INS], sp = 0;
  st[sp++] = nins - 1;
  while (sp) {
    int v = st[--sp];
    if (frontier_mark[v] == frontier_gen) continue;
    frontier_mark[v] = frontier_gen;
    if (ins[v].op == OP_MIN) {
      if (ins[v].a >= 0) st[sp++] = ins[v].a;
      if (ins[v].b >= 0) st[sp++] = ins[v].b;
    } else frontier[nfrontier++] = v;
  }
}

static inline void set_aff(Worker *w, int i, float ax, float ay, float ac) {
  w->aff[i] = 1;
  w->ax[i] = ax;
  w->ay[i] = ay;
  w->ac[i] = ac;
}
static void analyze_affine(Worker *w, const Tape *t, int i) {
  Instr in = t->ins[i];
  w->aff[i] = 0;
  if (in.op == OP_CONST) set_aff(w, i, 0, 0, in.fa);
  else if (in.op == OP_VARX) set_aff(w, i, 1, 0, 0);
  else if (in.op == OP_VARY) set_aff(w, i, 0, 1, 0);
  else if (in.op == OP_NEG && w->aff[in.a]) set_aff(w, i, -w->ax[in.a], -w->ay[in.a], -w->ac[in.a]);
  else if ((in.op == OP_ADD || in.op == OP_SUB) && w->aff[in.a] && w->aff[in.b]) {
    float s = in.op == OP_ADD ? 1 : -1;
    set_aff(w, i, w->ax[in.a] + s * w->ax[in.b], w->ay[in.a] + s * w->ay[in.b], w->ac[in.a] + s * w->ac[in.b]);
  } else if (in.op == OP_MUL && t->mode[in.a] == MODE_SCALAR && w->aff[in.b]) {
    float s = w->scalar[in.a];
    set_aff(w, i, s * w->ax[in.b], s * w->ay[in.b], s * w->ac[in.b]);
  } else if (in.op == OP_MUL && t->mode[in.b] == MODE_SCALAR && w->aff[in.a]) {
    float s = w->scalar[in.b];
    set_aff(w, i, s * w->ax[in.a], s * w->ay[in.a], s * w->ac[in.a]);
  }
}
static inline void detect_rewrites(Worker *w, Tape *t, const uint8_t *nonneg) {
  uint8_t sign[MAX_LOCAL] = {0};
  sign[t->root] = 1;
  for (int i = t->n - 1; i >= 0; i--)
    if (sign[i]) {
      Instr in = t->ins[i];
      if (in.op == OP_MIN || in.op == OP_MAX) {
        if (in.a >= 0) sign[in.a] = 1;
        if (in.b >= 0) sign[in.b] = 1;
      } else if (in.op == OP_NEG && in.a >= 0) sign[in.a] = 1;
    }
  for (int i = 0; i < t->n; i++)
    if (sign[i] && t->ins[i].op == OP_SUB) {
      Instr in = t->ins[i];
      int q = -1;
      float c = -1;
      Op op = OP_COUNT;
      if (in.a >= 0 && in.b >= 0 && t->ins[in.a].op == OP_SQRT && t->mode[in.b] == MODE_SCALAR) {
        q = t->ins[in.a].a;
        c = w->scalar[in.b];
        op = OP_QSUBK;
      } else if (in.a >= 0 && in.b >= 0 && t->mode[in.a] == MODE_SCALAR && t->ins[in.b].op == OP_SQRT) {
        q = t->ins[in.b].a;
        c = w->scalar[in.a];
        op = OP_KSUBQ;
      }
      if (q >= 0 && c >= 0 && nonneg[q]) t->ins[i] = (Instr){op, q, -1, c * c, 0};
    }
}
static inline void mark_needed_inputs(const Tape *t, uint8_t *need, int i) {
  Instr in = t->ins[i];
  if (in.a >= 0 && t->mode[in.a] != MODE_SCALAR) need[in.a] = 1;
  if (in.b >= 0 && t->mode[in.b] != MODE_SCALAR) need[in.b] = 1;
}
static inline bool collect_postorder(Worker *w, int fr, int *out_n) {
  int sp = 0, n = 0;
  if (++w->mark_id == 0) {
    memset(w->mark, 0, sizeof(w->mark));
    w->mark_id = 1;
  }
  w->stack[sp++] = fr;
  w->stack[sp++] = 0;
  while (sp) {
    int state = w->stack[--sp], v = w->stack[--sp];
    if (!state) {
      if (w->mark[v] == w->mark_id) continue;
      w->mark[v] = w->mark_id;
      w->stack[sp++] = v;
      w->stack[sp++] = 1;
      if (ins[v].b >= 0) {
        w->stack[sp++] = ins[v].b;
        w->stack[sp++] = 0;
      }
      if (ins[v].a >= 0) {
        w->stack[sp++] = ins[v].a;
        w->stack[sp++] = 0;
      }
    } else {
      if (n >= MAX_LOCAL) return false;
      w->post[n++] = v;
    }
  }
  *out_n = n;
  return true;
}
static bool build_tape(Worker *w, int fi, Tape *t) {
  int fr = frontier[fi], n = 0;
  if (!collect_postorder(w, fr, &n)) return false;
  t->n = n;
  t->xl = t->yl = -1;
  t->nx = t->ny = t->nf = 0;
  t->fill = 1;
  uint8_t nonneg[MAX_LOCAL] = {0};
  for (int i = 0; i < n; i++) w->map[w->post[i]] = i;
  for (int i = 0; i < n; i++) {
    Instr in = ins[w->post[i]];
    if (in.a >= 0) in.a = w->map[in.a];
    if (in.b >= 0) in.b = w->map[in.b];
    t->ins[i] = in;
    t->mode[i] = in.op == OP_CONST                                             ? MODE_SCALAR
                 : in.op == OP_VARX                                            ? MODE_X
                 : in.op == OP_VARY                                            ? MODE_Y
                 : (in.op == OP_NEG || in.op == OP_SQUARE || in.op == OP_SQRT) ? (in.a >= 0 ? t->mode[in.a] : MODE_SCALAR)
                                                                               : mode2(t->mode[in.a], t->mode[in.b]);
    if (t->mode[i] == MODE_SCALAR) w->scalar[i] = in.op == OP_CONST ? in.fa : eval_scalar(in.op, w->scalar[in.a], in.b >= 0 ? w->scalar[in.b] : 0);
    analyze_affine(w, t, i);
    switch (in.op) {
    case OP_CONST: nonneg[i] = in.fa >= 0; break;
    case OP_SQUARE:
    case OP_SQRT: nonneg[i] = 1; break;
    case OP_ADD:
    case OP_MUL:
    case OP_MIN: nonneg[i] = nonneg[in.a] && nonneg[in.b]; break;
    case OP_MAX: nonneg[i] = nonneg[in.a] || nonneg[in.b]; break;
    default: break;
    }
    if (in.op == OP_MIN || (t->mode[i] == MODE_FULL && in.op != OP_MAX)) t->fill = 0;
    int gv = w->post[i];
    if (gv == xidx) t->xl = i;
    if (gv == yidx) t->yl = i;
    if (gv == fr) t->root = i;
  }
  detect_rewrites(w, t, nonneg);
  uint8_t need[MAX_LOCAL] = {0};
  need[t->root] = 1;
  for (int i = n - 1; i >= 0; i--)
    if (need[i] && !w->aff[i] && t->mode[i] != MODE_SCALAR) mark_needed_inputs(t, need, i);
  for (int i = 0; i < n; i++) {
    Instr in = t->ins[i];
    bool row = t->mode[i] == MODE_FULL;
    t->arg[i] = (in.a >= 0 && (t->mode[in.a] == MODE_SCALAR || (row && t->mode[in.a] == MODE_Y)) ? 1 : 0) |
                (in.b >= 0 && (t->mode[in.b] == MODE_SCALAR || (row && t->mode[in.b] == MODE_Y)) ? 2 : 0);
    if (!need[i]) continue;
    if (t->mode[i] == MODE_X) t->xop[t->nx++] = i;
    else if (t->mode[i] == MODE_Y) t->yop[t->ny++] = i;
    else if (t->mode[i] == MODE_FULL) t->fop[t->nf++] = i;
  }
  return true;
}

static inline void want(Worker *w, int c, Interval iv, Interval f) {
  if (c < 0) return;
  iv = inter(iv, f);
  if (!ok_i(iv)) return;
  if (w->want_tag[c] == w->tag) w->want[c] = inter(w->want[c], iv);
  else {
    w->want[c] = iv;
    w->want_tag[c] = w->tag;
  }
}
static void region_from_box(Region *r, Box b) {
  r->n = 4;
  r->x[0] = b.x0;
  r->y[0] = b.y0;
  r->x[1] = b.x1;
  r->y[1] = b.y0;
  r->x[2] = b.x1;
  r->y[2] = b.y1;
  r->x[3] = b.x0;
  r->y[3] = b.y1;
}
static bool region_clip(Region *r, float ax, float ay, float hi) {
  float ox[MAX_REGION_POINTS], oy[MAX_REGION_POINTS];
  int m = 0;
  if (!r->n) return false;
  float px = r->x[r->n - 1], py = r->y[r->n - 1], pv = ax * px + ay * py - hi;
  bool pin = pv <= 1e-6f;
  for (int i = 0; i < r->n; i++) {
    float cx = r->x[i], cy = r->y[i], cv = ax * cx + ay * cy - hi;
    bool cin = cv <= 1e-6f;
    if (pin != cin) {
      if (m >= MAX_REGION_POINTS) return false;
      float t = pv / (pv - cv);
      ox[m] = px + t * (cx - px);
      oy[m++] = py + t * (cy - py);
    }
    if (cin) {
      if (m >= MAX_REGION_POINTS) return false;
      ox[m] = cx;
      oy[m++] = cy;
    }
    px = cx;
    py = cy;
    pv = cv;
    pin = cin;
  }
  memcpy(r->x, ox, m * sizeof(float));
  memcpy(r->y, oy, m * sizeof(float));
  r->n = m;
  return m != 0;
}
static bool region_meet_linear(Region *r, float ax, float ay, float c, Interval z) {
  if (ax == 0 && ay == 0) return z.lo <= c && c <= z.hi;
  if (z.lo <= -1e20f && z.hi >= 1e20f) return true;
  if (z.hi < 1e20f && !region_clip(r, ax, ay, z.hi - c)) return false;
  if (z.lo > -1e20f && !region_clip(r, -ax, -ay, c - z.lo)) return false;
  return true;
}
static bool region_bbox(const Region *r, Box *out) {
  if (!r->n) return false;
  Box b = {r->x[0], r->x[0], r->y[0], r->y[0]};
  for (int i = 1; i < r->n; i++) {
    b.x0 = fminf(b.x0, r->x[i]);
    b.x1 = fmaxf(b.x1, r->x[i]);
    b.y0 = fminf(b.y0, r->y[i]);
    b.y1 = fmaxf(b.y1, r->y[i]);
  }
  *out = b;
  return true;
}
static bool restrict_region(Worker *w, const Tape *t, Box b, Box *out) {
  Region r;
  region_from_box(&r, b);
  for (int v = 0; v < t->n; v++)
    if (w->want_tag[v] == w->tag && w->aff[v] && !region_meet_linear(&r, w->ax[v], w->ay[v], w->ac[v], w->want[v])) return false;
  return region_bbox(&r, out);
}
static inline void next_want_tag(Worker *w) {
  if (++w->tag == 0) {
    memset(w->want_tag, 0, sizeof(w->want_tag));
    w->tag = 1;
  }
}
static Interval forward(Worker *w, const Tape *t, Box b) {
  for (int v = 0; v < t->n; v++) {
    if (w->aff[v]) {
      w->fwd[v] = iaff(w->ax[v], w->ay[v], w->ac[v], b);
      continue;
    }
    Instr in = t->ins[v];
    switch (in.op) {
    case OP_ADD: w->fwd[v] = iadd(w->fwd[in.a], w->fwd[in.b]); break;
    case OP_SUB: w->fwd[v] = isub(w->fwd[in.a], w->fwd[in.b]); break;
    case OP_MUL: w->fwd[v] = imul(w->fwd[in.a], w->fwd[in.b]); break;
    case OP_NEG: w->fwd[v] = ineg(w->fwd[in.a]); break;
    case OP_SQUARE: w->fwd[v] = isqr(w->fwd[in.a]); break;
    case OP_SQRT: w->fwd[v] = isqrt_i(w->fwd[in.a]); break;
    case OP_MIN: w->fwd[v] = I(fminf(w->fwd[in.a].lo, w->fwd[in.b].lo), fminf(w->fwd[in.a].hi, w->fwd[in.b].hi)); break;
    case OP_MAX: w->fwd[v] = I(fmaxf(w->fwd[in.a].lo, w->fwd[in.b].lo), fmaxf(w->fwd[in.a].hi, w->fwd[in.b].hi)); break;
    case OP_QSUBK: w->fwd[v] = isub(w->fwd[in.a], I(in.fa, in.fa)); break;
    case OP_KSUBQ: w->fwd[v] = isub(I(in.fa, in.fa), w->fwd[in.a]); break;
    default: break;
    }
  }
  return w->fwd[t->root];
}
static bool contract_once(Worker *w, const Tape *t, Box b, Box *out) {
  Interval r = forward(w, t, b);
  if (!ok_i(r) || r.lo >= 0) return false;
  next_want_tag(w);
  w->want[t->root] = I(NEG_INF, 0);
  w->want_tag[t->root] = w->tag;
  for (int v = t->n - 1; v >= 0; v--)
    if (w->want_tag[v] == w->tag) {
      if (w->aff[v]) continue;
      Instr in = t->ins[v];
      Interval z = w->want[v], a = in.a >= 0 ? w->fwd[in.a] : I(in.fa, in.fa), bb = in.b >= 0 ? w->fwd[in.b] : I(in.fb, in.fb);
      switch (in.op) {
      case OP_ADD:
        want(w, in.a, I(z.lo - bb.hi, z.hi - bb.lo), a);
        want(w, in.b, I(z.lo - a.hi, z.hi - a.lo), bb);
        break;
      case OP_SUB:
        want(w, in.a, I(z.lo + bb.lo, z.hi + bb.hi), a);
        want(w, in.b, I(a.lo - z.hi, a.hi - z.lo), bb);
        break;
      case OP_MUL:
        if (bb.lo > 0 || bb.hi < 0) want(w, in.a, idiv(z, bb), a);
        if (a.lo > 0 || a.hi < 0) want(w, in.b, idiv(z, a), bb);
        break;
      case OP_NEG: want(w, in.a, I(-z.hi, -z.lo), a); break;
      case OP_SQUARE: want(w, in.a, I(-sqrtf(fmaxf(0, z.hi)), sqrtf(fmaxf(0, z.hi))), a); break;
      case OP_SQRT:
        if (z.hi >= 0) want(w, in.a, I(fmaxf(0, z.lo) * fmaxf(0, z.lo), fmaxf(0, z.hi) * fmaxf(0, z.hi)), a);
        break;
      case OP_QSUBK:
        want(w, in.a, I(z.lo + in.fa, z.hi + in.fa), a);
        break;
      case OP_KSUBQ: want(w, in.a, I(in.fa - z.hi, in.fa - z.lo), a); break;
      case OP_MAX:
        want(w, in.a, I(NEG_INF, z.hi), a);
        want(w, in.b, I(NEG_INF, z.hi), bb);
        break;
      case OP_MIN:
        want(w, in.a, I(z.lo, a.hi), a);
        want(w, in.b, I(z.lo, bb.hi), bb);
        if (a.lo > z.hi) want(w, in.b, I(NEG_INF, z.hi), bb);
        if (bb.lo > z.hi) want(w, in.a, I(NEG_INF, z.hi), a);
        break;
      default: break;
      }
    }
  return restrict_region(w, t, b, out);
}
static inline PixelBox pbox(Box b) {
  return (PixelBox){clampi((int)ceilf((b.x0 + 1) * .5f * (IMAGE_W - 1)), 0, IMAGE_W), clampi((int)floorf((b.x1 + 1) * .5f * (IMAGE_W - 1)), -1, IMAGE_W - 1),
                    clampi((int)ceilf((b.y0 + 1) * .5f * (IMAGE_H - 1)), 0, IMAGE_H), clampi((int)floorf((b.y1 + 1) * .5f * (IMAGE_H - 1)), -1, IMAGE_H - 1)};
}
static inline void fill_box(PixelBox p) {
  int n = p.x1 - p.x0 + 1;
  for (int y = p.y0; y <= p.y1; y++) memset(&image[y * IMAGE_W + p.x0], 255, n);
}
static inline Box pdom(PixelBox p) { return (Box){gx[p.x0], gx[p.x1], gy[p.y0], gy[p.y1]}; }
static inline void push_split(PixelBox p, PixelBox *st, int *sp) {
  int pw = p.x1 - p.x0 + 1, ph = p.y1 - p.y0 + 1;
  if (pw >= ph) {
    int m = (p.x0 + p.x1) >> 1;
    st[(*sp)++] = (PixelBox){m + 1, p.x1, p.y0, p.y1};
    p.x1 = m;
  } else {
    int m = (p.y0 + p.y1) >> 1;
    st[(*sp)++] = (PixelBox){p.x0, p.x1, m + 1, p.y1};
    p.y1 = m;
  }
  st[(*sp)++] = p;
}
static inline void eval_affine_vec(Worker *w, int v, int x0, float yc, int n) { VEC(w->val[v], n, fmaf(w->ax[v], gx[x0 + ix], yc)); }
static void vec_op(Op op, float *d, const float *a, const float *b, float av, float bv, bool as, bool bs, int n) {
  switch (op) {
  case OP_NEG: VEC(d, n, -a[ix]); break;
  case OP_SQUARE: VEC(d, n, a[ix] * a[ix]); break;
  case OP_SQRT: VEC(d, n, sqrtf(a[ix])); break;
  case OP_ADD: VEC(d, n, ARG(as, av, a, ix) + ARG(bs, bv, b, ix)); break;
  case OP_SUB: VEC(d, n, ARG(as, av, a, ix) - ARG(bs, bv, b, ix)); break;
  case OP_MUL: VEC(d, n, ARG(as, av, a, ix) * ARG(bs, bv, b, ix)); break;
  case OP_MIN: VEC(d, n, fminf(ARG(as, av, a, ix), ARG(bs, bv, b, ix))); break;
  case OP_MAX: VEC(d, n, fmaxf(ARG(as, av, a, ix), ARG(bs, bv, b, ix))); break;
  default: break;
  }
}
static inline void eval_vec_node(Worker *w, const Tape *t, int v, int x0, float yy, int n) {
  Instr in = t->ins[v];
  if (w->aff[v]) {
    eval_affine_vec(w, v, x0, fmaf(w->ay[v], yy, w->ac[v]), n);
    return;
  }
  if (in.op == OP_QSUBK) {
    VEC(w->val[v], n, w->val[in.a][ix] - in.fa);
    return;
  }
  if (in.op == OP_KSUBQ) {
    VEC(w->val[v], n, in.fa - w->val[in.a][ix]);
    return;
  }
  vec_op(in.op, w->val[v], w->val[in.a], in.b >= 0 ? w->val[in.b] : NULL, w->scalar[in.a], in.b >= 0 ? w->scalar[in.b] : 0, t->arg[v] & 1, t->arg[v] & 2, n);
}
static void eval_box_raw(Worker *w, const Tape *t, PixelBox p) {
  int x0 = p.x0, x1 = p.x1, y0 = p.y0, y1 = p.y1, n = x1 - x0 + 1;
  if (x1 < x0 || y1 < y0) return;
  for (int ii = 0; ii < t->nx; ii++) eval_vec_node(w, t, t->xop[ii], x0, 0, n);
  for (int py = y0; py <= y1; py++) {
    float yy = gy[py];
    uint8_t *row = &image[py * IMAGE_W + x0];
    for (int ii = 0; ii < t->ny; ii++) {
      int v = t->yop[ii];
      Instr in = t->ins[v];
      if (w->aff[v]) w->scalar[v] = fmaf(w->ay[v], yy, w->ac[v]);
      else if (in.op == OP_QSUBK) w->scalar[v] = w->scalar[in.a] - in.fa;
      else if (in.op == OP_KSUBQ) w->scalar[v] = in.fa - w->scalar[in.a];
      else w->scalar[v] = eval_scalar(in.op, w->scalar[in.a], in.b >= 0 ? w->scalar[in.b] : 0);
    }
    for (int ii = 0; ii < t->nf; ii++) eval_vec_node(w, t, t->fop[ii], x0, yy, n);
    if (t->mode[t->root] == MODE_SCALAR || t->mode[t->root] == MODE_Y) {
      if (w->scalar[t->root] < 0) memset(row, 255, n);
    } else {
      float *r = w->val[t->root];
      _Pragma("omp simd") for (int ix = 0; ix < n; ix++) if (r[ix] < 0) row[ix] = 255;
    }
  }
}
static void eval_box(Worker *w, const Tape *t, Box b) {
  PixelBox st[128], p = pbox(b);
  int sp = 0;
  if (p.x1 < p.x0 || p.y1 < p.y0) return;
  if (t->fill) {
    fill_box(p);
    return;
  }
  st[sp++] = p;
  while (sp) {
    p = st[--sp];
    int pw = p.x1 - p.x0 + 1, ph = p.y1 - p.y0 + 1;
    if (pw * ph <= 4096) {
      eval_box_raw(w, t, p);
      continue;
    }
    Interval r = forward(w, t, pdom(p));
    if (!ok_i(r) || r.lo >= 0) continue;
    if (r.hi < 0) {
      fill_box(p);
      continue;
    }
    if (sp + 2 >= 128) {
      eval_box_raw(w, t, p);
      continue;
    }
    push_split(p, st, &sp);
  }
}

static void render_terms(Worker *w) {
#pragma omp for schedule(dynamic, 1) nowait
  for (int i = 0; i < nfrontier; i++) {
    Tape t;
    if (!build_tape(w, i, &t)) continue;
    Box b;
    if (contract_once(w, &t, (Box){-1, 1, -1, 1}, &b) && area(b) > 0) eval_box(w, &t, b);
  }
}
void vm_render_frame(Worker *w) {
#pragma omp single nowait
  extract_frontier();
#pragma omp for schedule(static) nowait
  for (int y = 0; y < IMAGE_H; y++) memset(&image[y * IMAGE_W], 0, IMAGE_W);
#pragma omp barrier
  render_terms(w);
#pragma omp barrier
}
