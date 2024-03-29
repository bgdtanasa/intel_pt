SRC_DIR=src
SOURCES=$(shell find $(SRC_DIR) -name "*.c")
OBJECTS=$(SOURCES:%.c=%.o)
DEPS=$(OBJECTS:%.o=%.d)
TARGET=myperf

INC_DIR=inc
INC_SUBDIRS=$(shell find $(INC_DIR) -type d)
INC_FLAGS=$(addprefix -I,$(INC_SUBDIRS))

CC=gcc
CXX=gcc
CFLAGS=$(INC_FLAGS) -m64 -static -static-libgcc -pedantic -Wall -Wextra -O3 -ggdb -std=gnu99 -D_GNU_SOURCE -MMD -MP

-include $(DEPS)

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(LINK.cc) $^ -static -static-libgcc -o $@

.PHONY: clean
clean:
	rm -f $(TARGET) $(OBJECTS) $(DEPS)
