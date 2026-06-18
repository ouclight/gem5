CC ?= gcc
AR ?= ar
GEM5_ROOT ?= $(abspath $(CURDIR)/../../../..)
TARGET ?= page_alias
SRC := page_alias.c
M5OP_SRC := $(GEM5_ROOT)/util/m5/src/abi/x86/m5op.S
M5OP_OBJ := m5op_x86.o
M5OP_LIB := libm5op_x86.a
CFLAGS ?= -O2 -static

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) $(M5OP_LIB)
	$(CC) $(CFLAGS) -I$(GEM5_ROOT)/include -o $@ $(SRC) -L. -lm5op_x86

$(M5OP_OBJ): $(M5OP_SRC)
	$(CC) -I$(GEM5_ROOT)/include -c -o $@ $<

$(M5OP_LIB): $(M5OP_OBJ)
	$(AR) rcs $@ $<

clean:
	$(RM) $(TARGET) $(M5OP_OBJ) $(M5OP_LIB)
