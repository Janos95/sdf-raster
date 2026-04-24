LLVM_PREFIX := /opt/homebrew/opt/llvm
LIBOMP_PREFIX := /opt/homebrew/opt/libomp

CC := $(LLVM_PREFIX)/bin/clang
CFLAGS := -O3 -ffast-math -ffp-contract=fast -mcpu=native -fopenmp
CPPFLAGS := -I$(LIBOMP_PREFIX)/include
LDFLAGS := -L$(LIBOMP_PREFIX)/lib
LDLIBS := -lm

TARGET := vm_omp_tuned
SRC := vm.c main.c
HDR := vm.h
VM := prospero.vm
PPM ?= output.ppm

RUN_ENV := OMP_NUM_THREADS=8 KMP_BLOCKTIME=infinite OMP_WAIT_POLICY=active OMP_PROC_BIND=close OMP_PLACES=threads

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
