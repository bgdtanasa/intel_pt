SRC_DIR=src
SOURCES=
SOURCES+=$(SRC_DIR)/kmod.c
SOURCES+=$(SRC_DIR)/main.c
SOURCES+=$(SRC_DIR)/x_elf.c
SOURCES+=$(SRC_DIR)/proc.c
SOURCES+=$(SRC_DIR)/xed.c
SOURCES+=$(SRC_DIR)/intel_pt.c

OBJECTS=$(SOURCES:%.c=%.o)
DEPS=$(OBJECTS:%.o=%.d)
TARGET=myperf
XED_KIT=xed-install-base-2025-12-04-lin-x86-64

INC_DIR=inc
INC_SUBDIRS=$(shell find $(INC_DIR) -type d)
INC_FLAGS=$(addprefix -I,$(INC_SUBDIRS))
INC_FLAGS+=-I../xed/kits/$(XED_KIT)/include/xed

LIB_DIR=../xed/kits/$(XED_KIT)/lib

CC=gcc
CXX=gcc

CPP_FLAGS=
CPP_FLAGS+=-DEN_MTC
#CPP_FLAGS+=-DEN_RET_COMPRESSION
CPP_FLAGS+=-DEN_VMLINUX
CPP_FLAGS+=-DEN_VMLINUX_WIDE
#CPP_FLAGS+=-DEN_PMU
CPP_FLAGS+=-DEN_PTRACE_UNWIND
#CPP_FLAGS+=-DEN_PTRACE_BRK

ifneq ($(filter -DEN_PMU, $(CPP_FLAGS)), )
SOURCES+=$(SRC_DIR)/pmu.c
endif
ifneq ($(filter -DEN_PTRACE_UNWIND, $(CPP_FLAGS)), )
SOURCES+=$(SRC_DIR)/x_dwarf.c
SOURCES+=$(SRC_DIR)/x_unwind.c
endif
ifneq ($(filter -DEN_PTRACE_BRK, $(CPP_FLAGS)), )
SOURCES+=$(SRC_DIR)/brk.c
endif

ifneq ($(filter -DEN_VMLINUX, $(CPP_FLAGS)), )
CPP_FLAGS+=-DVMLINUX_STEXT=0xFFFFFFFF81000000llu
endif

CFLAGS=$(INC_FLAGS) $(CPP_FLAGS) -flto -Wno-unused-result -m64 -mavx -pedantic -Wall -Wextra -O3 -ggdb -std=gnu99 -D_GNU_SOURCE -MMD -MP
#CFLAGS+=-fsanitize=leak
#CFLAGS+=-fsanitize=address
#CFLAGS+=-fsanitize=undefined
#CFLAGS+=-fsanitize=address

-include $(DEPS)

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(LINK.cc) $^ -flto -ggdb -L$(LIB_DIR) -lxed -o $@

.PHONY: clean
clean:
	rm -f $(TARGET) $(OBJECTS) $(DEPS)
