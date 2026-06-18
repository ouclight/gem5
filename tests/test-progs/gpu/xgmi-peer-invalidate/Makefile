HIPCC ?= hipcc
CC ?= gcc
AR ?= ar
GEM5_ROOT ?= $(abspath $(CURDIR)/../../../..)
TARGET ?= invalidate_hip
SRC := invalidate_hip.cpp
M5OP_SRC := $(GEM5_ROOT)/util/m5/src/abi/x86/m5op.S
M5OP_OBJ := m5op_x86.o
M5OP_LIB := libm5op_x86.a
HSACO_TARGET ?= invalidate_kernels.hsaco
HSACO_SRC := invalidate_kernels.hip
HSACO_BUNDLE_TARGET ?= invalidate_kernels.bundle
HIPCCFLAGS ?=
HSACO_HIPCCFLAGS ?= -mno-code-object-v3
LLVM_BIN ?= $(ROCM_PATH)/llvm/bin

ifneq ($(ROCM_PATH),)
HIPCCFLAGS += --rocm-path=$(ROCM_PATH)
HIP_LDFLAGS += -L$(ROCM_PATH)/lib
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) $(HSACO_TARGET) $(M5OP_LIB)
	$(HIPCC) $(HIPCCFLAGS) -O2 -I$(GEM5_ROOT)/include -DINVALIDATE_HSACO_PATH=\"$(abspath $(HSACO_TARGET))\" -o $@ $(SRC) -L. -lm5op_x86 $(HIP_LDFLAGS) -lhsa-runtime64 -lhsakmt

$(M5OP_OBJ): $(M5OP_SRC)
	$(CC) -I$(GEM5_ROOT)/include -c -o $@ $<

$(M5OP_LIB): $(M5OP_OBJ)
	$(AR) rcs $@ $<

$(HSACO_TARGET): $(HSACO_BUNDLE_TARGET)
	$(LLVM_BIN)/clang-offload-bundler -unbundle -type=o -targets=host-x86_64-unknown-linux,hip-amdgcn-amd-amdhsa-gfx900 -inputs=$< -outputs=/dev/null,$@

$(HSACO_BUNDLE_TARGET): $(HSACO_SRC)
	$(HIPCC) $(HIPCCFLAGS) $(HSACO_HIPCCFLAGS) -O2 --genco --offload-arch=gfx900 -o $@ $<

clean:
	$(RM) $(TARGET) $(M5OP_OBJ) $(M5OP_LIB) $(HSACO_TARGET) $(HSACO_BUNDLE_TARGET)
