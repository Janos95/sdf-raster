LLVM_PREFIX := /opt/homebrew/opt/llvm
LIBOMP_PREFIX := /opt/homebrew/opt/libomp

CC := $(LLVM_PREFIX)/bin/clang
TILE_H ?= 16
THREADS ?= 10
BLOCKTIME ?= infinite
WAIT_POLICY ?= active
PROC_BIND ?= close
PLACES ?= threads

CFLAGS := -O3 -ffast-math -ffp-contract=fast -mcpu=native -fopenmp -DTILE_H=$(TILE_H)
CPPFLAGS := -I$(LIBOMP_PREFIX)/include
LDFLAGS := -L$(LIBOMP_PREFIX)/lib
LDLIBS := -lm

TARGET := vm_omp_tuned
SRC := vm.c main.c
HDR := vm.h
VM := prospero.vm
PPM ?= output.ppm

RUN_ENV := OMP_NUM_THREADS=$(THREADS) KMP_BLOCKTIME=$(BLOCKTIME) OMP_WAIT_POLICY=$(WAIT_POLICY) OMP_PROC_BIND=$(PROC_BIND) OMP_PLACES=$(PLACES)

.PHONY: all run render clean

all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) $(LDFLAGS) $(LDLIBS) -o $@

run: $(TARGET)
	env $(RUN_ENV) ./$(TARGET) $(VM)

render: $(TARGET)
	env $(RUN_ENV) ./$(TARGET) $(VM) $(PPM)

clean:
	rm -f $(TARGET)
