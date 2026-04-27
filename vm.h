#ifndef VM_H
#define VM_H

#include <stdint.h>

enum {
  MAX_INS = 50000,
  MAX_LOCAL = 128,
  IMAGE_W = 1024,
  IMAGE_H = 1024,
  BENCH_RUNS = 32
};

typedef enum { OP_CONST, OP_VARX, OP_VARY, OP_ADD, OP_SUB, OP_MUL, OP_NEG, OP_SQUARE, OP_SQRT, OP_MIN, OP_MAX, OP_QSUBK, OP_KSUBQ, OP_COUNT } Op;
typedef struct {
  Op op;
  int32_t a, b;
  float fa, fb;
} Instr;
typedef struct {
  float lo, hi;
} Interval;
typedef struct Worker {
  Interval fwd[MAX_LOCAL], want[MAX_LOCAL];
  uint16_t want_tag[MAX_LOCAL], mark[MAX_INS], map[MAX_INS], tag, mark_id;
  int post[MAX_LOCAL], stack[MAX_LOCAL * 2];
  uint8_t aff[MAX_LOCAL];
  float ax[MAX_LOCAL], ay[MAX_LOCAL], ac[MAX_LOCAL], scalar[MAX_LOCAL], val[MAX_LOCAL][IMAGE_W + 8] __attribute__((aligned(64)));
} Worker;

void vm_reset_program(void);
void vm_init_coords(void);
int vm_load_file(const char *path);
int vm_add_instr(Instr in);
int vm_instruction_count(void);
int vm_frontier_count(void);
int vm_checksum(void);
const uint8_t *vm_image_data(void);
void vm_worker_init(Worker *w);
/* Call once per worker inside a shared OpenMP parallel region. */
void vm_render_frame(Worker *w);
void vm_render_root_frame(Worker *w, int root);

#endif
