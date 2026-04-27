#include "vm.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

static inline double now_sec(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
  vm_init_coords();
  if (vm_load_file(argc > 1 ? argv[1] : "prospero.vm") != 0) return 1;
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
