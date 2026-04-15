# Makefile
#
# GIVEN BY PROFESSOR (structure), WE FILLED IN (targets)
#
# How to use:
#   make          → builds everything
#   make clean    → removes compiled files
#   make userspace → builds only user-space programs (no kernel headers needed)
#
# What gets built:
#   engine        ← the main supervisor + CLI binary
#   cpu_hog       ← test workload (CPU-bound)
#   io_pulse      ← test workload (I/O-bound)
#   memory_hog    ← test workload (RAM consumer)
#   monitor.ko    ← kernel module (needs linux-headers installed)

# ── Kernel module build ──────────────────────────────────────
obj-m += monitor.o

KDIR  := /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

# ── User-space compiler flags ────────────────────────────────
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g -pthread
LDFLAGS = -pthread

# ── Default target: build everything ─────────────────────────
all: userspace modules

# ── User-space programs ───────────────────────────────────────
userspace: engine cpu_hog io_pulse memory_hog

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine engine.c $(LDFLAGS)
	@echo "  ✓ engine built"

cpu_hog: cpu_hog.c
	$(CC) $(CFLAGS) -o cpu_hog cpu_hog.c
	@echo "  ✓ cpu_hog built"

io_pulse: io_pulse.c
	$(CC) $(CFLAGS) -o io_pulse io_pulse.c
	@echo "  ✓ io_pulse built"

memory_hog: memory_hog.c
	$(CC) $(CFLAGS) -o memory_hog memory_hog.c
	@echo "  ✓ memory_hog built"

# ── Kernel module ─────────────────────────────────────────────
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@echo "  ✓ monitor.ko built"

# ── Clean ─────────────────────────────────────────────────────
clean:
	rm -f engine cpu_hog io_pulse memory_hog
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	@echo "  ✓ cleaned"

.PHONY: all userspace modules clean
