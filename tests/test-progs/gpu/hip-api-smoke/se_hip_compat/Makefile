HIPCC ?= hipcc
CXX ?= g++
ROCM_PATH ?= /opt/rocm
LLVM_BIN := $(ROCM_PATH)/llvm/bin

LIB_TARGET := libse_hip_compat.so
LIB_SRC := se_hip_compat.cpp
HSACO_TARGET := se_hip_memset.hsaco
HSACO_BUNDLE_TARGET := se_hip_memset.bundle
HSACO_SRC := se_hip_memset.hip

.PHONY: all clean

all: $(LIB_TARGET) $(HSACO_TARGET)

$(LIB_TARGET): $(LIB_SRC)
	$(CXX) -std=c++17 -O2 -fPIC -shared -pthread \
		-D__HIP_PLATFORM_HCC__ -I$(ROCM_PATH)/hip/include \
		-I$(ROCM_PATH)/include \
		-L$(ROCM_PATH)/lib -o $@ $< -ldl -lhsa-runtime64

$(HSACO_TARGET): $(HSACO_BUNDLE_TARGET)
	$(LLVM_BIN)/clang-offload-bundler -unbundle -type=o \
		-targets=host-x86_64-unknown-linux,hip-amdgcn-amd-amdhsa-gfx900 \
		-inputs=$< -outputs=/dev/null,$@

$(HSACO_BUNDLE_TARGET): $(HSACO_SRC)
	$(HIPCC) --rocm-path=$(ROCM_PATH) -mno-code-object-v3 -O2 \
		--genco --offload-arch=gfx900 -o $@ $<

clean:
	$(RM) $(LIB_TARGET) $(HSACO_TARGET) $(HSACO_BUNDLE_TARGET)
