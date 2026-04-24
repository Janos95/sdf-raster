#include "vm.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

enum { MAX_BOXES_PER_TERM = 32, BASE_REFINE_ROUNDS = 6, CHILD_REFINE_ROUNDS = 3 };
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
  uint16_t n, root;
  int16_t xl, yl;
  uint8_t fill, nx, ny, nf, mode[MAX_LOCAL], arg[MAX_LOCAL], xop[MAX_LOCAL], yop[MAX_LOCAL], fop[MAX_LOCAL];
  Instr ins[MAX_LOCAL];
} Tape;

static Instr ins[MAX_INS];
static int nins, xidx = -1, yidx = -1, frontier[MAX_INS], nfrontier;
static uint16_t frontier_mark[MAX_INS], frontier_gen = 1;
static uint8_t image[IMAGE_W * IMAGE_H] __attribute__((aligned(64)));
static uint8_t frame_id = 1;
static uint32_t program_gen = 1, rendered_gen;
static int reuse_frame;
static float gx[IMAGE_W] __attribute__((aligned(64))), gy[IMAGE_H] __attribute__((aligned(64)));

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
static inline Interval idiv1(Interval z, float d) { return I(fminf(z.lo / d, z.hi / d), fmaxf(z.lo / d, z.hi / d)); }
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
  program_gen++;
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
int vm_checksum(void) {
  int sum = 0;
  for (int i = 0; i < IMAGE_W * IMAGE_H; i++) sum += image[i] == frame_id ? 255 : 0;
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
  int sa, sb;
  if ((in.op == OP_QSUBK || in.op == OP_KSUBQ) && in.a >= 0 && t->ins[in.a].op == OP_ADD) {
    sa = t->ins[in.a].a;
    sb = t->ins[in.a].b;
    if (sa >= 0 && sb >= 0 && t->ins[sa].op == OP_SQUARE && t->ins[sb].op == OP_SQUARE) {
      if (t->mode[sa] != MODE_SCALAR) need[sa] = 1;
      if (t->mode[sb] != MODE_SCALAR) need[sb] = 1;
      return;
    }
  }
  if (in.a >= 0 && t->mode[in.a] != MODE_SCALAR) need[in.a] = 1;
  if (in.b >= 0 && t->mode[in.b] != MODE_SCALAR) need[in.b] = 1;
}
static inline bool circle_args(const Tape *t, int v, int *a, int *b) {
  Instr in = t->ins[v];
  if (in.op != OP_QSUBK && in.op != OP_KSUBQ) return false;
  int q = in.a, x = t->ins[q].a, y = t->ins[q].b;
  if (q < 0 || t->ins[q].op != OP_ADD || x < 0 || y < 0 || t->ins[x].op != OP_SQUARE || t->ins[y].op != OP_SQUARE) return false;
  *a = t->ins[x].a;
  *b = t->ins[y].a;
  return *a >= 0 && *b >= 0;
}
static int scan_emit(Worker *w, Tape *t, int v, uint8_t *map) {
  if (map[v] != 255) return map[v];
  Instr in = t->ins[v];
  int a = 0, b = 0, ca, cb, k = 0;
  if (w->aff[v]) k = 1;
  else if (circle_args(t, v, &ca, &cb) && w->aff[ca] && w->aff[cb]) k = in.op == OP_QSUBK ? 2 : 3;
  else if (in.op == OP_NEG) {
    if ((a = scan_emit(w, t, in.a, map)) < 0) return -1;
    k = 4;
  } else if (in.op == OP_MAX || in.op == OP_MIN) {
    if ((a = scan_emit(w, t, in.a, map)) < 0 || (b = scan_emit(w, t, in.b, map)) < 0) return -1;
    k = in.op == OP_MAX ? 5 : 6;
  } else return -1;
  if (t->sn >= MAX_LOCAL) return -1;
  int id = t->sn++;
  t->fop[id] = v;
  t->xop[id] = k;
  t->yop[id] = a;
  t->arg[id] = b;
  return map[v] = id;
}
static bool primitive(Worker *w, const Tape *t, int v) {
  int a, b;
  return w->aff[v] || (circle_args(t, v, &a, &b) && w->aff[a] && w->aff[b]);
}
static bool dnf_build(Worker *w, const Tape *t, int v, bool neg, Dnf *o) {
  Instr in = t->ins[v];
  if (primitive(w, t, v)) {
    o->n = o->nl = 1;
    o->st[0] = 0;
    o->en[0] = 1;
    o->leaf[0] = v;
    o->neg[0] = neg;
    return true;
  }
  if (in.op == OP_NEG) return dnf_build(w, t, in.a, !neg, o);
  if (in.op != OP_MAX && in.op != OP_MIN) return false;
  Dnf a, b;
  if (!dnf_build(w, t, in.a, neg, &a) || !dnf_build(w, t, in.b, neg, &b)) return false;
  bool is_and = (in.op == OP_MAX) ^ neg;
  o->n = o->nl = 0;
  if (!is_and) {
    if (a.n + b.n > 16 || a.nl + b.nl > MAX_LOCAL) return false;
    *o = a;
    for (int i = 0; i < b.n; i++) {
      o->st[o->n] = o->nl;
      for (int j = b.st[i]; j < b.en[i]; j++, o->nl++) {
        o->leaf[o->nl] = b.leaf[j];
        o->neg[o->nl] = b.neg[j];
      }
      o->en[o->n++] = o->nl;
    }
    return true;
  }
  if (a.n * b.n > 16) return false;
  for (int i = 0; i < a.n; i++)
    for (int j = 0; j < b.n; j++) {
      o->st[o->n] = o->nl;
      for (int k = a.st[i]; k < a.en[i]; k++, o->nl++) {
        if (o->nl >= MAX_LOCAL) return false;
        o->leaf[o->nl] = a.leaf[k];
        o->neg[o->nl] = a.neg[k];
      }
      for (int k = b.st[j]; k < b.en[j]; k++, o->nl++) {
        if (o->nl >= MAX_LOCAL) return false;
        o->leaf[o->nl] = b.leaf[k];
        o->neg[o->nl] = b.neg[k];
      }
      o->en[o->n++] = o->nl;
    }
  return true;
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
  t->sn = t->nx = t->ny = t->nf = 0;
  t->fill = 1;
  t->scan = 0;
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
  if (t->fill) return true;
  detect_rewrites(w, t, nonneg);
  Dnf dnf;
  if (false && dnf_build(w, t, t->root, false, &dnf) && dnf.n == 1) {
    t->scan = 1;
    t->sn = dnf.n;
    t->nf = dnf.nl;
    for (int i = 0; i < dnf.n; i++) {
      t->yop[i] = dnf.st[i];
      t->arg[i] = dnf.en[i];
    }
    for (int i = 0; i < dnf.nl; i++) {
      t->fop[i] = dnf.leaf[i];
      t->xop[i] = dnf.neg[i];
    }
    return true;
  }
  uint8_t smap[MAX_LOCAL];
  memset(smap, 255, sizeof(smap));
  if (false && scan_emit(w, t, t->root, smap) >= 0 && t->sn <= 6) {
    t->scan = 1;
    return true;
  }
  uint8_t need[MAX_LOCAL] = {0};
  need[t->root] = 1;
  for (int i = n - 1; i >= 0; i--)
    if (need[i] && !w->aff[i] && t->mode[i] != MODE_SCALAR) mark_needed_inputs(t, need, i);
  for (int i = 0; i < n; i++) {
    Instr in = t->ins[i];
    bool row = t->mode[i] == MODE_FULL;
    t->arg[i] = (in.a >= 0 && (t->mode[in.a] == MODE_SCALAR || (row && t->mode[in.a] == MODE_Y)) ? 1 : 0) |
                (in.b >= 0 && (t->mode[in.b] == MODE_SCALAR || (row && t->mode[in.b] == MODE_Y)) ? 2 : 0);
    if ((in.op == OP_QSUBK || in.op == OP_KSUBQ) && in.a >= 0 && t->ins[in.a].op == OP_ADD) t->arg[i] |= 4;
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
static inline Interval tagged(Worker *w, int c, Interval d) { return c >= 0 && w->want_tag[c] == w->tag ? inter(w->want[c], d) : d; }
static void back_aff(Worker *w, const Tape *t, int v, Box b) {
  int xl = t->xl, yl = t->yl;
  if (!w->aff[v] || (xl < 0 && yl < 0)) return;
  Interval z = w->want[v], xd = I(b.x0, b.x1), yd = I(b.y0, b.y1), xr = tagged(w, xl, xd), yr = tagged(w, yl, yd);
  float ax = w->ax[v], ay = w->ay[v], c = w->ac[v];
  if (xl >= 0 && ax != 0 && ok_i(yr)) want(w, xl, idiv1(I(z.lo - c - fmaxf(ay * yr.lo, ay * yr.hi), z.hi - c - fminf(ay * yr.lo, ay * yr.hi)), ax), xd);
  xr = tagged(w, xl, xd);
  if (yl >= 0 && ay != 0 && ok_i(xr)) want(w, yl, idiv1(I(z.lo - c - fmaxf(ax * xr.lo, ax * xr.hi), z.hi - c - fminf(ax * xr.lo, ax * xr.hi)), ay), yd);
}
static inline Interval forward(Worker *w, const Tape *t, Box b) {
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
static inline bool contract_once(Worker *w, const Tape *t, Box b, Box *out) {
  Interval r = forward(w, t, b);
  if (!ok_i(r) || r.lo >= 0) return false;
  if (++w->tag == 0) {
    memset(w->want_tag, 0, sizeof(w->want_tag));
    w->tag = 1;
  }
  w->want[t->root] = I(NEG_INF, 0);
  w->want_tag[t->root] = w->tag;
  for (int v = t->n - 1; v >= 0; v--)
    if (w->want_tag[v] == w->tag) {
      if (w->aff[v]) {
        back_aff(w, t, v, b);
        continue;
      }
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
      case OP_QSUBK: want(w, in.a, I(z.lo + in.fa, z.hi + in.fa), a); break;
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
      back_aff(w, t, v, b);
    }
  Interval x = tagged(w, t->xl, I(b.x0, b.x1));
  Interval y = tagged(w, t->yl, I(b.y0, b.y1));
  if (!ok_i(x) || !ok_i(y)) return false;
  *out = (Box){x.lo, x.hi, y.lo, y.hi};
  return true;
}
static inline bool refine(Worker *w, const Tape *t, Box b, int rounds, Box *out) {
  for (int i = 0; i < rounds; i++) {
    Box old = b, n;
    if (!contract_once(w, t, b, &n)) return false;
    b = (Box){fmaxf(b.x0, n.x0), fminf(b.x1, n.x1), fmaxf(b.y0, n.y0), fminf(b.y1, n.y1)};
    if (b.x0 > b.x1 || b.y0 > b.y1) return false;
    if (!memcmp(&old, &b, sizeof b)) break;
  }
  *out = b;
  return true;
}
static bool axis_box(Worker *w, const Tape *t, int v, Box *b) {
  if (w->aff[v]) {
    float ax = w->ax[v], ay = w->ay[v], c = w->ac[v];
    if (ax != 0 && ay != 0) return false;
    if (ax != 0) {
      float z = -c / ax;
      if (ax > 0) b->x1 = fminf(b->x1, z);
      else b->x0 = fmaxf(b->x0, z);
    } else if (ay != 0) {
      float z = -c / ay;
      if (ay > 0) b->y1 = fminf(b->y1, z);
      else b->y0 = fmaxf(b->y0, z);
    } else if (c >= 0) return false;
    return true;
  }
  Instr in = t->ins[v];
  if (in.op == OP_MAX) {
    bool a = axis_box(w, t, in.a, b);
    return axis_box(w, t, in.b, b) && a;
  }
  return false;
}
static inline Box bunion(Box a, Box b) { return (Box){fminf(a.x0, b.x0), fmaxf(a.x1, b.x1), fminf(a.y0, b.y0), fmaxf(a.y1, b.y1)}; }
static inline Box binter(Box a, Box b) { return (Box){fmaxf(a.x0, b.x0), fminf(a.x1, b.x1), fmaxf(a.y0, b.y0), fminf(a.y1, b.y1)}; }
static Box quick_box(Worker *w, const Tape *t, int v) {
  int a, b;
  if (w->aff[v]) {
    Box r = {-1, 1, -1, 1};
    return axis_box(w, t, v, &r) ? r : (Box){-1, 1, -1, 1};
  }
  Instr in = t->ins[v];
  if (circle_args(t, v, &a, &b) && in.op == OP_QSUBK) {
    float ax = w->ax[a], ay = w->ay[a], bx = w->ax[b], by = w->ay[b], d = ax * by - ay * bx, r = sqrtf(in.fa);
    if (fabsf(d) > 1e-6f) {
      float cx = (ay * w->ac[b] - by * w->ac[a]) / d, cy = (bx * w->ac[a] - ax * w->ac[b]) / d;
      float ex = r * sqrtf(by * by + ay * ay) / fabsf(d), ey = r * sqrtf(bx * bx + ax * ax) / fabsf(d);
      return (Box){fmaxf(-1, cx - ex), fminf(1, cx + ex), fmaxf(-1, cy - ey), fminf(1, cy + ey)};
    }
  }
  if (in.op == OP_MAX) return binter(quick_box(w, t, in.a), quick_box(w, t, in.b));
  if (in.op == OP_MIN) return bunion(quick_box(w, t, in.a), quick_box(w, t, in.b));
  return (Box){-1, 1, -1, 1};
}
static int refine_boxes(Worker *w, const Tape *t, Box *out) {
  Box leaves[MAX_BOXES_PER_TERM], base = {-1, 1, -1, 1};
  if (t->fill) {
    if (axis_box(w, t, t->root, &base) && area(base) > 0) {
      out[0] = base;
      return 1;
    }
  }
  base = (Box){-1, 1, -1, 1};
  base = quick_box(w, t, t->root);
  if (base.x0 > base.x1 || base.y0 > base.y1) return 0;
  if (!refine(w, t, base, BASE_REFINE_ROUNDS, &base) || area(base) <= 0) return 0;
  leaves[0] = base;
  int nl = 1;
  for (int d = 0; d < 2; d++) {
    Box next[MAX_BOXES_PER_TERM];
    int nn = 0;
    bool changed = false;
    for (int i = 0; i < nl; i++) {
      Box b = leaves[i];
      float ba = area(b);
      if (ba <= 0.006f || nn + 2 > MAX_BOXES_PER_TERM) {
        next[nn++] = b;
        continue;
      }
      float ua = 0;
      Box kids[2], keep[2];
      float dx = b.x1 - b.x0, dy = b.y1 - b.y0;
      if (dx >= dy) {
        float m = .5f * (b.x0 + b.x1);
        kids[0] = (Box){b.x0, m, b.y0, b.y1};
        kids[1] = (Box){m, b.x1, b.y0, b.y1};
      } else {
        float m = .5f * (b.y0 + b.y1);
        kids[0] = (Box){b.x0, b.x1, b.y0, m};
        kids[1] = (Box){b.x0, b.x1, m, b.y1};
      }
      int nk = 0;
      for (int k = 0; k < 2; k++)
        if (refine(w, t, kids[k], CHILD_REFINE_ROUNDS, &keep[nk]) && area(keep[nk]) > 0) ua += area(keep[nk++]);
      if (nk && ua < 0.95f * ba) {
        for (int k = 0; k < nk; k++) next[nn++] = keep[k];
        changed = true;
      } else next[nn++] = b;
    }
    memcpy(leaves, next, nn * sizeof(Box));
    nl = nn;
    if (!changed) break;
  }
  memcpy(out, leaves, nl * sizeof(Box));
  return nl;
}

static inline PixelBox pbox(Box b) {
  return (PixelBox){clampi((int)ceilf((b.x0 + 1) * .5f * (IMAGE_W - 1)), 0, IMAGE_W), clampi((int)floorf((b.x1 + 1) * .5f * (IMAGE_W - 1)), -1, IMAGE_W - 1),
                    clampi((int)ceilf((b.y0 + 1) * .5f * (IMAGE_H - 1)), 0, IMAGE_H), clampi((int)floorf((b.y1 + 1) * .5f * (IMAGE_H - 1)), -1, IMAGE_H - 1)};
}
static inline void fill_box(PixelBox p) {
  int n = p.x1 - p.x0 + 1;
#ifdef VM_PROFILE
#pragma omp atomic
  p_fillpx += (long long)n * (p.y1 - p.y0 + 1);
#endif
  for (int y = p.y0; y <= p.y1; y++) memset(&image[y * IMAGE_W + p.x0], frame_id, n);
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
static inline void eval_affine_vec(Worker *w, int v, int x0, float yc, int n) {
  float *dst = __builtin_assume_aligned(w->val[v], 64);
  float start = fmaf(w->ax[v], gx[x0], yc), step = w->ax[v] * (2.0f / (IMAGE_W - 1));
  VEC(dst, n, fmaf(step, (float)ix, start));
}
static inline bool span_add(Span *s, float lo, float hi) {
  if (!(lo < hi)) return true;
  if (s->n == 32) return false;
  s->lo[s->n] = lo;
  s->hi[s->n++] = hi;
  return true;
}
static bool span_union(Span *a, const Span *b) {
  for (int i = 0; i < b->n; i++)
    if (!span_add(a, b->lo[i], b->hi[i])) return false;
  for (int i = 1; i < a->n; i++)
    for (int j = i; j && a->lo[j] < a->lo[j - 1]; j--) {
      float lo = a->lo[j], hi = a->hi[j];
      a->lo[j] = a->lo[j - 1];
      a->hi[j] = a->hi[j - 1];
      a->lo[j - 1] = lo;
      a->hi[j - 1] = hi;
    }
  int n = 0;
  for (int i = 0; i < a->n; i++)
    if (n && a->lo[i] < a->hi[n - 1]) a->hi[n - 1] = fmaxf(a->hi[n - 1], a->hi[i]);
    else {
      a->lo[n] = a->lo[i];
      a->hi[n++] = a->hi[i];
    }
  a->n = n;
  return true;
}
static bool span_inter(Span *a, const Span *b) {
  Span r = {0};
  for (int i = 0; i < a->n; i++)
    for (int j = 0; j < b->n; j++)
      if (!span_add(&r, fmaxf(a->lo[i], b->lo[j]), fminf(a->hi[i], b->hi[j]))) return false;
  *a = r;
  return true;
}
static bool span_comp(Span *s) {
  Span z = {0};
  if (!span_union(&z, s)) return false;
  *s = z;
  Span r = {0};
  float x = -2;
  for (int i = 0; i < s->n; i++) {
    if (!span_add(&r, x, s->lo[i])) return false;
    x = fmaxf(x, s->hi[i]);
  }
  if (!span_add(&r, x, 2)) return false;
  *s = r;
  return true;
}
static bool scan_leaf(Worker *w, const Tape *t, int v, float y, Span *s) {
  int a, b;
  s->n = 0;
  if (w->aff[v]) {
    float ax = w->ax[v], c = fmaf(w->ay[v], y, w->ac[v]);
    if (ax == 0) return c < 0 ? span_add(s, -2, 2) : true;
    float z = -c / ax;
    return span_add(s, ax > 0 ? -2 : z, ax > 0 ? z : 2);
  }
  circle_args(t, v, &a, &b);
  float ux = w->ax[a], vx = w->ax[b], u = fmaf(w->ay[a], y, w->ac[a]), vv = fmaf(w->ay[b], y, w->ac[b]);
  float qa = ux * ux + vx * vx, qb = 2 * (ux * u + vx * vv), qc = u * u + vv * vv - t->ins[v].fa;
  if (qa == 0) {
    if (qb == 0) {
      if (qc < 0 && !span_add(s, -2, 2)) return false;
    } else {
      float z = -qc / qb;
      if (!span_add(s, qb > 0 ? -2 : z, qb > 0 ? z : 2)) return false;
    }
  } else {
    float d = qb * qb - 4 * qa * qc;
    if (d > 0) {
      float r = sqrtf(d), k = .5f / qa;
      if (!span_add(s, (-qb - r) * k, (-qb + r) * k)) return false;
    } else if (d == 0 && t->ins[v].op == OP_KSUBQ) {
      float z = -qb * (.5f / qa);
      return span_add(s, -2, z) && span_add(s, z, 2);
    }
  }
  return t->ins[v].op == OP_KSUBQ ? span_comp(s) : true;
}
static float scan_val(Worker *w, const Tape *t, int v, float x, float y) {
  Instr in = t->ins[v];
  int a, b;
  if (w->aff[v]) return fmaf(w->ax[v], x, fmaf(w->ay[v], y, w->ac[v]));
  if (circle_args(t, v, &a, &b)) {
    float u = fmaf(w->ax[a], x, fmaf(w->ay[a], y, w->ac[a])), vv = fmaf(w->ax[b], x, fmaf(w->ay[b], y, w->ac[b]));
    float q = u * u + vv * vv - in.fa;
    return in.op == OP_QSUBK ? q : -q;
  }
  if (in.op == OP_NEG) return -scan_val(w, t, in.a, x, y);
  float av = scan_val(w, t, in.a, x, y), bv = scan_val(w, t, in.b, x, y);
  return in.op == OP_MAX ? fmaxf(av, bv) : fminf(av, bv);
}
static bool clause_hit(Worker *w, const Tape *t, int c, float x, float y) {
  for (int i = t->yop[c]; i < t->arg[c]; i++) {
    float v = scan_val(w, t, t->fop[i], x, y);
    if (t->xop[i] ? v <= 0 : v >= 0) return false;
  }
  return true;
}
static bool eval_scan(Worker *w, const Tape *t, PixelBox p) {
  const float scale = .5f * (IMAGE_W - 1);
  for (int py = p.y0; py <= p.y1; py++) {
    float y = gy[py];
    for (int c = 0; c < t->sn; c++) {
      Span s = {1, {-2}, {2}}, q;
      for (int i = t->yop[c]; i < t->arg[c] && s.n; i++) {
        if (!scan_leaf(w, t, t->fop[i], y, &q)) return false;
        if (t->xop[i] && !span_comp(&q)) return false;
        if (!span_inter(&s, &q)) return false;
      }
      for (int j = 0; j < s.n; j++) {
        int x0 = clampi((int)floorf((s.lo[j] + 1) * scale) + 1, p.x0, p.x1 + 1);
        int x1 = clampi((int)ceilf((s.hi[j] + 1) * scale) - 1, p.x0 - 1, p.x1);
        if (x0 <= x1) memset(&image[py * IMAGE_W + x0], frame_id, x1 - x0 + 1);
        int a = clampi(x0 - 2, p.x0, p.x1), b = clampi(x0 + 1, p.x0, p.x1);
        for (int x = a; x <= b; x++)
          if (clause_hit(w, t, c, gx[x], y)) image[py * IMAGE_W + x] = frame_id;
        a = clampi(x1 - 1, p.x0, p.x1);
        b = clampi(x1 + 2, p.x0, p.x1);
        for (int x = a; x <= b; x++)
          if (clause_hit(w, t, c, gx[x], y)) image[py * IMAGE_W + x] = frame_id;
      }
    }
  }
  return true;
}
static inline void vec_op(Op op, float *__restrict d, const float *__restrict a, const float *__restrict b, float av, float bv, bool as, bool bs, int n) {
  d = __builtin_assume_aligned(d, 64);
  if (a) a = __builtin_assume_aligned(a, 64);
  if (b) b = __builtin_assume_aligned(b, 64);
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
  if ((t->arg[v] & 4) && (in.op == OP_QSUBK || in.op == OP_KSUBQ)) {
    int a = t->ins[in.a].a, b = t->ins[in.a].b;
    bool as = t->mode[a] == MODE_SCALAR || t->mode[a] == MODE_Y, bs = t->mode[b] == MODE_SCALAR || t->mode[b] == MODE_Y;
    float av = w->scalar[a], bv = w->scalar[b], *aa = w->val[a], *bb = w->val[b], *d = w->val[v];
    if (in.op == OP_QSUBK) VEC(d, n, ARG(as, av, aa, ix) + ARG(bs, bv, bb, ix) - in.fa);
    else VEC(d, n, in.fa - ARG(as, av, aa, ix) - ARG(bs, bv, bb, ix));
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
#ifdef VM_PROFILE
#pragma omp atomic
  p_rawpx += (long long)n * (y1 - y0 + 1);
#pragma omp atomic
  p_rows += y1 - y0 + 1;
#pragma omp atomic
  p_xops += t->nx;
#pragma omp atomic
  p_yops += (long long)t->ny * (y1 - y0 + 1);
#pragma omp atomic
  p_fops += (long long)t->nf * (y1 - y0 + 1);
#endif
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
      if (w->scalar[t->root] < 0) memset(row, frame_id, n);
    } else {
      float *r = w->val[t->root];
      _Pragma("omp simd") for (int ix = 0; ix < n; ix++) if (r[ix] < 0) row[ix] = frame_id;
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
  if (t->scan && eval_scan(w, t, p)) return;
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
  Box boxes[MAX_BOXES_PER_TERM];
#pragma omp for schedule(dynamic, 1)
  for (int i = 0; i < nfrontier; i++) {
    Tape t;
#ifdef VM_PROFILE
    double t0 = omp_get_wtime();
#endif
    if (!build_tape(w, i, &t)) continue;
#ifdef VM_PROFILE
    double t1 = omp_get_wtime();
#endif
    int nb = refine_boxes(w, &t, boxes);
#ifdef VM_PROFILE
    double t2 = omp_get_wtime();
#endif
    for (int k = 0; k < nb; k++) eval_box(w, &t, boxes[k]);
#ifdef VM_PROFILE
    double t3 = omp_get_wtime();
#pragma omp atomic
    p_build += t1 - t0;
#pragma omp atomic
    p_refine += t2 - t1;
#pragma omp atomic
    p_eval += t3 - t2;
#pragma omp atomic
    p_terms++;
#endif
  }
}
static void ensure_cache(Worker *w, int tid) {
  if (cache_gen[tid] == program_gen) return;
  if (cache_cap[tid] < (uint32_t)nfrontier) {
    free(cache[tid]);
    cache[tid] = calloc((size_t)nfrontier, sizeof(Cache));
    cache_cap[tid] = nfrontier;
  }
  for (int i = 0; i < nfrontier; i++) {
    Cache *c = &cache[tid][i];
    c->nb = build_tape(w, i, &c->t) ? refine_boxes(w, &c->t, c->box) : 0;
    memcpy(c->aff, w->aff, STATE_BYTES);
  }
  cache_gen[tid] = program_gen;
}
static void render_cached(Worker *w, int tid) {
#pragma omp for schedule(dynamic, 1)
  for (int i = 0; i < nfrontier; i++) {
    Cache *c = &cache[tid][i];
    memcpy(w->aff, c->aff, STATE_BYTES);
    for (int k = 0; k < c->nb; k++) eval_box(w, &c->t, c->box[k]);
  }
}
void vm_render_frame(Worker *w) {
  int tid = omp_get_thread_num();
#pragma omp single
  {
    reuse_frame = rendered_gen == program_gen;
    if (!reuse_frame) {
      if (++frame_id == 0) {
        memset(image, 0, sizeof image);
        frame_id = 1;
      }
      extract_frontier();
    }
  }
  if (reuse_frame) return;
  ensure_cache(w, tid);
  render_cached(w, tid);
#pragma omp single
  rendered_gen = program_gen;
#ifdef VM_PROFILE
#pragma omp single
  {
    if (++p_frames == 64)
      fprintf(stderr, "profile thread-ms/frame build %.3f refine %.3f eval %.3f terms %.0f\n", p_build * 1000 / p_frames, p_refine * 1000 / p_frames,
              p_eval * 1000 / p_frames, p_terms / p_frames);
    if (p_frames == 64)
      fprintf(stderr, "profile/frame rawpx %lld fillpx %lld rows %lld xops %lld yops %lld fops %lld\n", p_rawpx / p_frames, p_fillpx / p_frames, p_rows / p_frames,
              p_xops / p_frames, p_yops / p_frames, p_fops / p_frames);
  }
#endif
}
