SRC_DIR=src
SOURCES=$(shell find $(SRC_DIR) -name "*.c")
OBJECTS=$(SOURCES:%.c=%.o)
DEPS=$(OBJECTS:%.o=%.d)
TARGET=myperf
XED_KIT=xed-install-base-2025-08-31-lin-x86-64

INC_DIR=inc
INC_SUBDIRS=$(shell find $(INC_DIR) -type d)
INC_FLAGS=$(addprefix -I,$(INC_SUBDIRS))
INC_FLAGS+=-I../xed/kits/$(XED_KIT)/include/xed

LIB_DIR=../xed/kits/$(XED_KIT)/lib

CC=gcc
CXX=gcc
CPP_FLAGS=
CPP_FLAGS+=-DEN_RET_COMPRESSION
CPP_FLAGS+=-DEN_VMLINUX
CPP_FLAGS+=-DEN_PMU
CFLAGS=$(INC_FLAGS) $(CPP_FLAGS) -Wno-unused-result -flto -m64 -mavx -static -static-libgcc -pedantic -Wall -Wextra -O3 -ggdb -std=gnu99 -D_GNU_SOURCE -MMD -MP

-include $(DEPS)

.PHONY: all kmod_all
all: $(TARGET) kmod_all

$(TARGET): $(OBJECTS)
	$(LINK.cc) $^ -flto -ggdb -static -static-libgcc -L$(LIB_DIR) -lxed -o $@

kmod_all:
	$(MAKE) -C kmod

.PHONY: clean
clean:
	rm -f $(TARGET) $(OBJECTS) $(DEPS)
	$(MAKE) -C kmod clean
