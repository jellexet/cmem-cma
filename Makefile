# Makefile for cmem-cma kernel module

# Module name and object files
obj-m := cmem-cma.o

# Source files for the module
cmem-cma-objs := src/cmem-cma.o

# Kernel build directory
KDIR := /lib/modules/$(shell uname -r)/build

# Current directory
PWD := $(shell pwd)

# Userspace programs
USERSPACE_PROG := cmem_cma_test

# Debug flag
DEBUG ?= 0

# Normal flags
ccflags-y += \
	-Wall \
	-O2 \
	-I$(src)/headers \
	-I$(src)/tests

ifeq ($(DEBUG),1)
ccflags-y += -DDEBUG
endif


# Default target
all: module userspace

# Build kernel module
module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# compile with debug output enabled
debug:
	$(MAKE) module DEBUG=1

# Build userspace test program (original)
userspace:
	gcc -Iheaders -o $(USERSPACE_PROG) tests/cmem-cma-test.c -lnuma -Wall -O2

# Clean build artifacts
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(USERSPACE_PROG) \
		*.o\
		*.mod\
		*.ko\
		*.order\
		*.symvers

# Install module (requires root)
install: module
	sudo insmod cmem-cma.ko

# Remove module (requires root)
remove:
	sudo rmmod cmem_cma || true

# Reload module (remove then install)
reload: remove install

# Check module status (requires root)
status:
	sudo lsmod | grep cmem_cma || echo "Module not loaded"
	sudo dmesg | tail -20 | grep cmem_cma || echo "No recent kernel messages"

# Show device info
info:
	ls -la /dev/cmem_cma 2>/dev/null || echo "Device file not found"
	cat /proc/devices | grep cmem_cma || echo "Device not registered"

# Run basic tests (requires module to be loaded)
test: userspace
	sudo ./$(USERSPACE_PROG) -t

# Help target
help:
	@echo "Available targets:"
	@echo ""
	@echo "Module targets:"
	@echo "  module   - Build kernel module only"
	@echo "  debug    - Build kernel module only whith debug output enabled"
	@echo "  install  - Install kernel module"
	@echo "  remove   - Remove kernel module" 
	@echo "  reload   - Remove and reinstall kernel module"
	@echo "  status   - Check module status and recent kernel messages"
	@echo ""
	@echo "Userspace targets:"
	@echo "  userspace- Build original userspace test program"
	@echo "  all      - Build kernel module and both userspace programs"
	@echo ""
	@echo "Testing targets:"
	@echo "  test         - Run basic CMEM CMA tests"
	@echo ""
	@echo "Other targets:"
	@echo "  clean    - Clean build artifacts"
	@echo "  info     - Show device file and registration info"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Example usage:"
	@echo "  make all              # Build everything"
	@echo "  make reload           # Reload kernel module"

.PHONY: all module userspace clean install remove reload status info test help
