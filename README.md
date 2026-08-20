## For what is this useful

This kernel module provides physically contiguous, cache coherent, NUMA aware DMA buffers.
It is useful for embedded and SoC Linux applications like video capture, audio DSPs, FPGA interfaces or custom peripherals, where real hardware needs to bypass the CPU for RW operations in memory.

`cmem_cma` is the wrong tool if there is no real hardware doing DMA.

This project was an attempt to replace [cmem](https://gitlab.cern.ch/atlas-tdaq-felix/drivers/cmem) for the [FELIX project](https://atlas-project-felix.web.cern.ch/atlas-project-felix/) at [CERN](https://home.cern/).
Unfortunately it does not play well with RDMA, [here](docs/CMA_REPORT.md) is the report I wrote about it.

## Before starting

`kernel-devel` and `kernel-headers` need to be installed in order to compile the code.

```sh
sudo dnf install kernel-devel-$(uname -r) kernel-headers-$(uname -r)
```

Install the NUMA library

```sh
sudo dnf install numactl-devel
```

## Usage

In order to match the allocation size of `cmem_cma` with the size of the `CMA` pool, look for `CmaTotal` in `/proc/meminfo` and set `max_allocation_size` accordingly when installing the module.
Beware that `max_allocation_size` is in Bytes.
Unfortunately for an out-of-tree kernel module it is not possible to reference some symbols in `cma.h` that would allow to get the total size of the `CMA` pool.

See the [example below](#1-load-the-kernel-module).

### 1. Load the Kernel Module

```sh
# Load module and create device with proper permissions
sudo make install MODULE_PARAMS="max_allocation_size=536870912" # 512 MB allocation

# Verify module is loaded
lsmod | grep cmem_cma

# Check device was created
ls -l /dev/cmem_cma
```

### 3. Monitor CMA usage

```sh
# Check cmem_cma allocation status
sudo cat /proc/cmem_cma

# Monitor kernel messages
sudo dmesg -w | grep cmem_cma

# Check NUMA memory info
numactl --hardware
```


## Kernel Boot Parameters

CMA configuration is located in `/etc/kernel/cmdline`:

```sh
root=UUID=f571ac33-446a-4ed6-b9d3-9ed70e0e9499 ro rootflags=subvol=root rhgb quiet cma=8G cma_pernuma=8G
```

This exampole allocates:

- 8GB total CMA memory
- 8GB per NUMA node (distributed across nodes)

## Check CMA status

```sh
# Total CMA memory
grep -i cma /proc/meminfo

# CMA allocation details
cat /proc/buddyinfo | grep DMA

# NUMA memory layout
cat /proc/zoneinfo | grep -A5 Node
```

### To remove the module

```sh
sudo rmmod cmem_cma

# OR

make remove
```

### To fix BTF generation

```sh
sudo dnf install dwarves
sudo cp /sys/kernel/btf/vmlinux /usr/lib/modules/$(uname -r)/build
```

The reason is a change in location of the file `vmlinux`

## Userspace

`usperspace` directory contains C++ files that provide an interface to access the kernel module functionalities.
To compile:

```bash
cmake -S . -B build
cd build
make -j $(nproc)
```
