#ifndef UTILS_H
#define UTILS_H

#include "vm.h"

#include <stddef.h>

enum {
  UTIL_MAX_SOURCE_IDS = MAX_INS,
  UTIL_MAX_GLYPHS = 256,
  UTIL_MAX_GLYPH_TERMS = 64,
  UTIL_MAX_TEXT_LEN = 4096
};

typedef struct {
  char key[32];
  int idx;
} UtilIdMap;

typedef struct {
  Instr in;
  char id[32];
} SourceInstr;

typedef struct {
  SourceInstr ins[MAX_INS];
  UtilIdMap ids[UTIL_MAX_SOURCE_IDS];
  int nins, nids;
} SourceProgram;

typedef struct {
  int present;
  int letter_index;
  int x0, y0, x1, y1;
  float sx0, sy0, sx1, sy1;
  int nterms;
  char term_id[UTIL_MAX_GLYPH_TERMS][32];
  int term_idx[UTIL_MAX_GLYPH_TERMS];
} Glyph;

typedef struct {
  SourceProgram src;
  Glyph glyph[UTIL_MAX_GLYPHS];
  int out_x, out_y, root;
} TextProgram;

extern const char *DEFAULT_VIEWER_TEXT;

int vm_load_file(const char *path);

int text_program_load(TextProgram *tp, const char *source_vm, const char *glyph_table);
int text_program_build(TextProgram *tp, const char *text);
int text_char_supported(const TextProgram *tp, unsigned char ch);
unsigned char closest_supported_char(const TextProgram *tp, unsigned char ch);
int append_text_char(char *text, size_t *len, unsigned char ch);
int append_text_input(const TextProgram *tp, char *text, size_t *len, const char *input);
int backspace_text(char *text, size_t *len);

#endif
