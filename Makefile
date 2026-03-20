# Mini-UnionFS Makefile
#
# Prerequisites (Ubuntu 22.04):
#   sudo apt-get install build-essential libfuse3-dev pkg-config
#
# For older systems with FUSE2:
#   sudo apt-get install build-essential libfuse-dev pkg-config
#   Then run: make FUSE_VERSION=2

CC ?= gcc
CFLAGS = -Wall -Wextra -g -D_FILE_OFFSET_BITS=64

# Default to FUSE3, set FUSE_VERSION=2 for older systems
FUSE_VERSION ?= 3

ifeq ($(FUSE_VERSION),2)
    FUSE_CFLAGS = $(shell pkg-config --cflags fuse 2>/dev/null || echo "-I/usr/include/fuse -D_FILE_OFFSET_BITS=64")
    FUSE_LIBS = $(shell pkg-config --libs fuse 2>/dev/null || echo "-lfuse -pthread")
    CFLAGS += -DFUSE_USE_VERSION=26
else
    FUSE_CFLAGS = $(shell pkg-config --cflags fuse3 2>/dev/null || echo "-I/usr/include/fuse3")
    FUSE_LIBS = $(shell pkg-config --libs fuse3 2>/dev/null || echo "-lfuse3 -pthread")
endif

TARGET = mini_unionfs
SRC = mini_unionfs.c

.PHONY: all clean install test deps

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS)

clean:
	rm -f $(TARGET)

test: $(TARGET)
	./test_unionfs.sh

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Install dependencies (Ubuntu/Debian)
deps:
	sudo apt-get update && sudo apt-get install -y build-essential libfuse3-dev pkg-config fuse3
