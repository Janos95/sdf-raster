LLVM_PREFIX := /opt/homebrew/opt/llvm
LIBOMP_PREFIX := /opt/homebrew/opt/libomp

CC := $(LLVM_PREFIX)/bin/clang
CFLAGS := -O3 -ffast-math -ffp-contract=fast -mcpu=native -fopenmp
CPPFLAGS := -I. -I$(LIBOMP_PREFIX)/include
LDFLAGS := -L$(LIBOMP_PREFIX)/lib
LDLIBS := -lm

TARGET := vm_omp_tuned
VIEWER := viewer
EXTRACT_TARGET := extract_letters
GLYPH_DIR ?= glyph_pipeline
SRC := vm.c utils.c main.c
VIEWER_SRC := vm.c utils.c viewer.c
HDR := vm.h utils.h
VM := prospero.vm
PPM ?= output.ppm
SDL2_PREFIX ?= /opt/homebrew/opt/sdl2
SDL_CPPFLAGS := -I$(SDL2_PREFIX)/include
SDL_LDFLAGS := -L$(SDL2_PREFIX)/lib -Wl,-rpath,$(SDL2_PREFIX)/lib
SDL_LIBS := -lSDL2
LETTERS_JSON ?= $(GLYPH_DIR)/letters.json
LETTERS_SHEET ?= $(GLYPH_DIR)/letters_sheet.ppm
UNIQUE_LETTERS_SHEET ?= $(GLYPH_DIR)/letters_unique_sheet.ppm
GENERATED_VM ?= $(GLYPH_DIR)/generated_prospero.vm
GENERATED_JSON ?= $(GLYPH_DIR)/generated_prospero.json
GLYPH_TABLE ?= $(GLYPH_DIR)/glyph_table.txt

EMSDK ?= $(abspath ../emsdk)
LLVM_OPENMP_SRC ?= $(abspath ../llvm-project-openmp)
OPENMP_WASM_PREFIX ?= $(abspath ../openmp-wasm-install)
OPENMP_WASM_BUILD ?= $(abspath ../openmp-wasm-build)
WASM_NODE_DIR ?= build-wasm-node
WASM_NODE_TARGET := $(WASM_NODE_DIR)/prospero.js
WASM_STACK_SIZE ?= 8388608
EMSDK_ENV = export EMSDK_QUIET=1 && . $(EMSDK)/emsdk_env.sh >/dev/null
WASM_CFLAGS := -O3 -flto -DNDEBUG -ffast-math -ffp-contract=fast -fopenmp -pthread -msimd128
WASM_LDFLAGS := \
	-sPTHREAD_POOL_SIZE=8 \
	-sPROXY_TO_PTHREAD \
	-sNODERAWFS=1 \
	-sINITIAL_MEMORY=268435456 \
	-sSTACK_SIZE=$(WASM_STACK_SIZE) \
	-sDEFAULT_PTHREAD_STACK_SIZE=$(WASM_STACK_SIZE) \
	-sEXIT_RUNTIME=1

RUN_ENV := OMP_NUM_THREADS=8 KMP_BLOCKTIME=infinite OMP_WAIT_POLICY=active OMP_PROC_BIND=close OMP_PLACES=threads

.PHONY: all wasm-omp run run-viewer run-wasm-omp render extract typeset-expression verify clean

all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) $(LDFLAGS) $(LDLIBS) -o $@

$(VIEWER): $(VIEWER_SRC) $(HDR)
	$(CC) $(CPPFLAGS) $(SDL_CPPFLAGS) $(CFLAGS) $(VIEWER_SRC) $(LDFLAGS) $(SDL_LDFLAGS) $(LDLIBS) $(SDL_LIBS) -o $@

$(EXTRACT_TARGET): vm.c $(GLYPH_DIR)/extract_letters.c vm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) vm.c $(GLYPH_DIR)/extract_letters.c $(LDFLAGS) $(LDLIBS) -o $@

$(OPENMP_WASM_PREFIX)/lib/libomp.a:
	$(EMSDK_ENV) && emcmake cmake -S $(LLVM_OPENMP_SRC)/runtimes -B $(OPENMP_WASM_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_RUNTIMES=openmp -DLLVM_INCLUDE_TESTS=OFF -DLLVM_HOST_TRIPLE=wasm32-unknown-emscripten -DLLVM_DEFAULT_TARGET_TRIPLE=wasm32-unknown-emscripten -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread -DCMAKE_ASM_FLAGS=-pthread -DOPENMP_ENABLE_OMPT_TOOLS=OFF -DOPENMP_ENABLE_LIBOMPTARGET=OFF -DLIBOMP_HAVE_OMPT_SUPPORT=OFF -DLIBOMP_OMPT_SUPPORT=OFF -DLIBOMP_OMPD_SUPPORT=OFF -DLIBOMP_USE_DEBUGGER=OFF -DLIBOMP_FORTRAN_MODULES=OFF -DLIBOMP_ENABLE_SHARED=OFF -DLIBOMP_ARCH=wasm32 -DCMAKE_INSTALL_PREFIX=$(OPENMP_WASM_PREFIX) && cmake --build $(OPENMP_WASM_BUILD) --target install -j8

$(WASM_NODE_TARGET): $(SRC) $(HDR) Makefile $(OPENMP_WASM_PREFIX)/lib/libomp.a
	mkdir -p $(WASM_NODE_DIR) && $(EMSDK_ENV) && emcc $(WASM_CFLAGS) -I$(OPENMP_WASM_PREFIX)/include $(SRC) $(OPENMP_WASM_PREFIX)/lib/libomp.a $(LDLIBS) $(WASM_LDFLAGS) -o $@

wasm-omp: $(WASM_NODE_TARGET)

run: $(TARGET)
	env $(RUN_ENV) ./$(TARGET) $(VM)

run-viewer: $(VIEWER)
	env $(RUN_ENV) ./$(VIEWER) --source-vm $(VM) --glyphs $(GLYPH_TABLE)

run-wasm-omp: $(WASM_NODE_TARGET)
	env OMP_NUM_THREADS=8 node $(WASM_NODE_TARGET) $(VM)

render: $(TARGET)
	env $(RUN_ENV) ./$(TARGET) $(VM) $(PPM)

extract: $(EXTRACT_TARGET)
	mkdir -p $(GLYPH_DIR)
	env $(RUN_ENV) ./$(EXTRACT_TARGET) --sheet $(LETTERS_SHEET) --unique-sheet $(UNIQUE_LETTERS_SHEET) $(VM) $(LETTERS_JSON)

typeset-expression:
	mkdir -p $(GLYPH_DIR)
	python3 $(GLYPH_DIR)/typeset_expression_vm.py --letters $(LETTERS_JSON) --out $(GENERATED_VM) --metadata $(GENERATED_JSON) --glyph-table $(GLYPH_TABLE)

verify:
	python3 $(GLYPH_DIR)/verify_prospero.py

clean:
	rm -f $(TARGET)
	rm -f $(VIEWER)
	rm -f $(EXTRACT_TARGET)
	rm -rf $(WASM_NODE_DIR)
