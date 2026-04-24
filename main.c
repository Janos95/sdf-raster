#include "vm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  char key[32];
  int idx;
} IdMap;

static IdMap ids[MAX_INS];
static int nids;

static inline double now_sec(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}
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
  return -1;
}
static inline void parse_arg(const char *s, int16_t *ref, float *value) {
  if (s[0] == '_') *ref = idof(s);
  else *value = strtof(s, NULL);
}
static void load_vm(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    perror("fopen");
    exit(1);
  }
  vm_reset_program();
  nids = 0;
  char line[256], id[32], op[32], a[64], b[64];
  while (fgets(line, sizeof(line), f)) {
    a[0] = b[0] = 0;
    int n = sscanf(line, "%31s %31s %63s %63s", id, op, a, b);
    if (n < 2 || line[0] == '#') continue;
    int idx = vm_instruction_count();
    strcpy(ids[nids].key, id);
    ids[nids++].idx = idx;
    Instr in = {parse_op(op), -1, -1, 0, 0};
    if (n >= 3) parse_arg(a, &in.a, &in.fa);
    if (n >= 4) parse_arg(b, &in.b, &in.fb);
    if (vm_add_instr(in) != idx) {
      fprintf(stderr, "too many instructions\n");
      exit(1);
    }
  }
  fclose(f);
}

int main(int argc, char **argv) {
  vm_init_coords();
  load_vm(argc > 1 ? argv[1] : "prospero.vm");
  double mean = 0, ss = 0, mn = 1e9, mx = 0, t0 = 0;
#pragma omp parallel
  {
    Worker w;
    vm_worker_init(&w);
    for (int r = 0; r < BENCH_RUNS; r++) vm_render_frame(&w);
    for (int r = 0; r < BENCH_RUNS; r++) {
#pragma omp single
      t0 = now_sec();
      vm_render_frame(&w);
#pragma omp single
      {
        double dt = (now_sec() - t0) * 1000;
        mean += dt;
        ss += dt * dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
      }
    }
  }
  mean /= BENCH_RUNS;
  printf("instructions=%d frontier_terms=%d image=%dx%d checksum=%d\n", vm_instruction_count(), vm_frontier_count(), IMAGE_W, IMAGE_H, vm_checksum());
  printf("mean_ms=%.3f std_ms=%.3f min_ms=%.3f max_ms=%.3f\n", mean, sqrt(fmax(0, ss / BENCH_RUNS - mean * mean)), mn, mx);
  return 0;
}
