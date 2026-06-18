HIPCC ?= hipcc
CC ?= gcc
CXX ?= g++
AR ?= ar
GEM5_ROOT ?= $(abspath $(CURDIR)/../../../..)
TARGET ?= peer_vram_hip
SRC := peer_vram_hip.cpp
M5OP_SRC := $(GEM5_ROOT)/util/m5/src/abi/x86/m5op.S
M5OP_OBJ := m5op_x86.o
M5OP_LIB := libm5op_x86.a
HSACO_TARGET ?= peer_vram_kernels.hsaco
HSACO_SRC := peer_vram_kernels.hip
HSACO_BUNDLE_TARGET ?= peer_vram_kernels.bundle
HSA_TARGET ?= hsa_agent_count
HSA_SRC := hsa_agent_count.cpp
HSA_LOAD_TARGET ?= hsa_module_load
HSA_LOAD_SRC := hsa_module_load.cpp
HIPCCFLAGS ?=
HSACO_HIPCCFLAGS ?= -mno-code-object-v3
HSA_CXXFLAGS ?= -O2
LLVM_BIN ?= $(ROCM_PATH)/llvm/bin

ifneq ($(ROCM_PATH),)
HIPCCFLAGS += --rocm-path=$(ROCM_PATH)
HIP_LDFLAGS += -L$(ROCM_PATH)/lib
HSA_CXXFLAGS += -I$(ROCM_PATH)/include -L$(ROCM_PATH)/lib
endif

.PHONY: all clean

all: $(TARGET) $(HSA_TARGET) $(HSA_LOAD_TARGET)

$(TARGET): $(SRC) $(HSACO_TARGET) $(M5OP_LIB)
	$(HIPCC) $(HIPCCFLAGS) -O2 -I$(GEM5_ROOT)/include -DPEER_VRAM_HSACO_PATH=\"$(abspath $(HSACO_TARGET))\" -o $@ $(SRC) -L. -lm5op_x86 $(HIP_LDFLAGS) -lhsa-runtime64 -lhsakmt

$(M5OP_OBJ): $(M5OP_SRC)
	$(CC) -I$(GEM5_ROOT)/include -c -o $@ $<

$(M5OP_LIB): $(M5OP_OBJ)
	$(AR) rcs $@ $<

$(HSACO_TARGET): $(HSACO_BUNDLE_TARGET)
	$(LLVM_BIN)/clang-offload-bundler -unbundle -type=o -targets=host-x86_64-unknown-linux,hip-amdgcn-amd-amdhsa-gfx900 -inputs=$< -outputs=/dev/null,$@

$(HSACO_BUNDLE_TARGET): $(HSACO_SRC)
	$(HIPCC) $(HIPCCFLAGS) $(HSACO_HIPCCFLAGS) -O2 --genco --offload-arch=gfx900 -o $@ $<

$(HSA_TARGET): $(HSA_SRC)
	$(CXX) $(HSA_CXXFLAGS) -o $@ $< -lhsa-runtime64 -lhsakmt

$(HSA_LOAD_TARGET): $(HSA_LOAD_SRC) $(HSACO_TARGET)
	$(CXX) $(HSA_CXXFLAGS) -DPEER_VRAM_HSACO_PATH=\"$(abspath $(HSACO_TARGET))\" -o $@ $(HSA_LOAD_SRC) -lhsa-runtime64 -lhsakmt

clean:
	$(RM) $(TARGET) $(M5OP_OBJ) $(M5OP_LIB) $(HSACO_TARGET) $(HSACO_BUNDLE_TARGET) $(HSA_TARGET) $(HSA_LOAD_TARGET)
