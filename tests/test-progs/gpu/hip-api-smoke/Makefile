HIPCC ?= hipcc
CC ?= gcc
AR ?= ar
GEM5_ROOT ?= $(abspath $(CURDIR)/../../../..)
TARGET ?= hip_api_smoke
SRC := hip_api_smoke.hip
PRELOAD_SMOKE_TARGET := hip_memset_preload_smoke
PRELOAD_SMOKE_SRC := hip_memset_preload_smoke.hip
MALLOC_SMOKE_TARGET := hip_malloc_smoke
MALLOC_SMOKE_SRC := hip_malloc_smoke.hip
MEMCPY_PRELOAD_SMOKE_TARGET := hip_memcpy_preload_smoke
MEMCPY_PRELOAD_SMOKE_SRC := hip_memcpy_preload_smoke.hip
M5OP_SRC := $(GEM5_ROOT)/util/m5/src/abi/x86/m5op.S
M5OP_OBJ := m5op_x86.o
M5OP_LIB := libm5op_x86.a
HIPCCFLAGS ?= -mno-code-object-v3

ifneq ($(ROCM_PATH),)
HIPCCFLAGS += --rocm-path=$(ROCM_PATH)
endif

.PHONY: all clean

all: $(TARGET) $(PRELOAD_SMOKE_TARGET) $(MALLOC_SMOKE_TARGET) \
	$(MEMCPY_PRELOAD_SMOKE_TARGET)

$(TARGET): $(SRC)
	$(HIPCC) $(HIPCCFLAGS) -O2 --offload-arch=gfx900 -o $@ $<

$(PRELOAD_SMOKE_TARGET): $(PRELOAD_SMOKE_SRC) $(M5OP_LIB)
	$(HIPCC) $(HIPCCFLAGS) -O2 --offload-arch=gfx900 \
		-I$(GEM5_ROOT)/include -o $@ $< -L. -lm5op_x86

$(MALLOC_SMOKE_TARGET): $(MALLOC_SMOKE_SRC) $(M5OP_LIB)
	$(HIPCC) $(HIPCCFLAGS) -O2 --offload-arch=gfx900 \
		-I$(GEM5_ROOT)/include -o $@ $< -L. -lm5op_x86

$(MEMCPY_PRELOAD_SMOKE_TARGET): $(MEMCPY_PRELOAD_SMOKE_SRC) $(M5OP_LIB)
	$(HIPCC) $(HIPCCFLAGS) -O2 --offload-arch=gfx900 \
		-I$(GEM5_ROOT)/include -o $@ $< -L. -lm5op_x86

$(M5OP_OBJ): $(M5OP_SRC)
	$(CC) -I$(GEM5_ROOT)/include -c -o $@ $<

$(M5OP_LIB): $(M5OP_OBJ)
	$(AR) rcs $@ $<

clean:
	$(RM) $(TARGET) $(PRELOAD_SMOKE_TARGET) $(MALLOC_SMOKE_TARGET) \
		$(MEMCPY_PRELOAD_SMOKE_TARGET) $(M5OP_OBJ) $(M5OP_LIB)
