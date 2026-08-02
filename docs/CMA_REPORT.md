# REPORT: Why CMA Memory Didn't Work with Libfabric's RDMA

| Date: 22 August 2025
|
| From: Mario Shehu - mario.shehu@cern.ch

### Abstract
This report is to explain why zero-copy Remote Direct Memory Access (RDMA)
can't work with memory allocated via the Contiguous Memory Allocator (CMA) in the Linux kernel. 
A custom driver was developed to allocate contiguous memory for RDMA, as a replacement for the `cmem_rcc` module.
While the driver functioned correctly for most use cases, it failed during the RDMA memory registration 
step when using libfabric. 
The root cause was identified as a fundamental conflict between the "migratable" nature of CMA memory, and the requirement
of RDMA for long-term memory pinning.

### Setup
The server used for testing runs Almalinux 10  with the Linux Kernel 6.12. The source code shown in the file also comes
from the Linux kernel 6.12.
The server mounts a Mellanox Technologies MT27700 Family [ConnectX-4] NIC, and a FELIX 712 with 5.3 FULL mode firmware. 
To produce the FULL mode data for testing I used the internal emulator.

### What Went Wrong

The failure manifested when attempting to register a CMA-allocated memory region with libfabric the fi_mr_reg() function.
The debug output shows the physical and virtual addresses of the allocated CMA buffer.

--daq-unbuffered flag, in a program called felix-tohost.

```
--- CMA Buffer Allocated ---
USER VIRTUAL ADDRESS (After mmap): 0x7fcb56c01000
PHYS ADDRESS (DMA_ADDR): 0x27f400000
SIZE: 536870912 bytes
--------------------------
```

A check of the process's memory maps confirmed that the virtual address was correctly mapped with read, write, and shared permissions (`rw-s`)
to the custom driver's device file (`/dev/cma_dma_alloc`).

```sh
[mshehu@pc-tbed-felix-02 cma-dma-numa]$ cat /proc/644156/maps | grep 7fcb56c01000
7fcb56c01000-7fcb76c01000 rw-s 00001000 00:06 1004 /dev/cma_dma_alloc
```

The debug output just before fi_mr_reg() shows that the virtual address of the buffer is correct and the size (In hex) also is correct.
```sh
fi_mr_reg: buffer 0x7fcb56c01000 size 20000000
```

The return value of the function was an `EFAULT` anyways.

```
#define EFAULT 14 /* Bad address */

```

SInce the debug output looked fine to me, the error pointed me toward a problem with either CMA itself or the Infiniband library.

### Finding the Root Cause

The problem comes from a conflict between how CMA works and what RDMA needs.

CMA when allocating pageblocks, marks them as `MIGRATE_CMA`, which is a more restrictive `ZONE_MOVABLE` migratetype. 

```c
include/linux/mmzone.h, line 65 (as a enumerator)


#ifdef CONFIG_CMA
	/*
	 * MIGRATE_CMA migration type is designed to mimic the way
	 * ZONE_MOVABLE works.  Only movable pages can be allocated
	 * from MIGRATE_CMA pageblocks and page allocator never
	 * implicitly change migration type of MIGRATE_CMA pageblock.
	 *
	 * The way to use it is to change migratetype of a range of
	 * pageblocks to MIGRATE_CMA which can be done by
	 * __free_pageblock_cma() function.
	 */
	MIGRATE_CMA,
#endif
```
Page migration allows the moving of the physical location of pages between nodes in a numa system while the process is running. 
This means that the virtual addresses that the process sees do not change. 
However, the system rearranges the physical location of those pages.
The definition of `ZONE_MOVABLE` is:

```c
include/linux/mmzone.h, line 806 (as a enumerator)

	/*
	 * ZONE_MOVABLE is similar to ZONE_NORMAL, except that it contains
	 * movable pages with few exceptional cases described below. Main use
	 * cases for ZONE_MOVABLE are to make memory offlining/unplug more
	 * likely to succeed, and to locally limit unmovable allocations - e.g.,
	 * to increase the number of THP/huge pages. Notable special cases are:
	 *
	 * 1. Pinned pages: (long-term) pinning of movable pages might
	 *    essentially turn such pages unmovable. Therefore, we do not allow
	 *    pinning long-term pages in ZONE_MOVABLE. When pages are pinned and
	 *    faulted, they come from the right zone right away. However, it is
	 *    still possible that address space already has pages in
	 *    ZONE_MOVABLE at the time when pages are pinned (i.e. user has
	 *    touches that memory before pinning). In such case we migrate them
	 *    to a different zone. When migration fails - pinning fails.
	 * 2. memblock allocations: kernelcore/movablecore setups might create
	 *    situations where ZONE_MOVABLE contains unmovable allocations
	 *    after boot. Memory offlining and allocations fail early.
	 * 3. Memory holes: kernelcore/movablecore setups might create very rare
	 *    situations where ZONE_MOVABLE contains memory holes after boot,
	 *    for example, if we have sections that are only partially
	 *    populated. Memory offlining and allocations fail early.
	 * 4. PG_hwpoison pages: while poisoned pages can be skipped during
	 *    memory offlining, such pages cannot be allocated.
	 * 5. Unmovable PG_offline pages: in paravirtualized environments,
	 *    hotplugged memory blocks might only partially be managed by the
	 *    buddy (e.g., via XEN-balloon, Hyper-V balloon, virtio-mem). The
	 *    parts not manged by the buddy are unmovable PG_offline pages. In
	 *    some cases (virtio-mem), such pages can be skipped during
	 *    memory offlining, however, cannot be moved/allocated. These
	 *    techniques might use alloc_contig_range() to hide previously
	 *    exposed pages from the buddy again (e.g., to implement some sort
	 *    of memory unplug in virtio-mem).
	 * 6. ZERO_PAGE(0), kernelcore/movablecore setups might create
	 *    situations where ZERO_PAGE(0) which is allocated differently
	 *    on different platforms may end up in a movable zone. ZERO_PAGE(0)
	 *    cannot be migrated.
	 * 7. Memory-hotplug: when using memmap_on_memory and onlining the
	 *    memory to the MOVABLE zone, the vmemmap pages are also placed in
	 *    such zone. Such pages cannot be really moved around as they are
	 *    self-stored in the range, but they are treated as movable when
	 *    the range they describe is about to be offlined.
	 *
	 * In general, no unmovable allocations that degrade memory offlining
	 * should end up in ZONE_MOVABLE. Allocators (like alloc_contig_range())
	 * have to expect that migrating pages in ZONE_MOVABLE can fail (even
	 * if has_unmovable_pages() states that there are no unmovable pages,
	 * there can be false negatives).
	 */
	ZONE_MOVABLE,
```

Essentially a `ZONE_MOVABLE` pageblock can not be pinned long-term, thus neither can a `MIGRATE_CMA` type.

To pin and map userspace memory in Infiniband:

```c
include/linux/mm_types.h
/**
 * ib_umem_get - Pin and DMA map userspace memory.
 *
 * @device: IB device to connect UMEM
 * @addr: userspace virtual address to start at
 * @size: length of region to pin
 * @access: IB_ACCESS_xxx flags for memory being pinned
 */
struct ib_umem *ib_umem_get(struct ib_device *device, unsigned long addr,
			    size_t size, int access)
{
	struct ib_umem *umem;
	struct page **page_list;
	unsigned long lock_limit;
	unsigned long new_pinned;
	unsigned long cur_base;
	unsigned long dma_attr = 0;
	struct mm_struct *mm;
	unsigned long npages;
	int pinned, ret;
	unsigned int gup_flags = FOLL_LONGTERM;

...
```

`FOLL_LONGTERM` definition:

```c
include/linux/mm_types.h

...

/*
	 * FOLL_LONGTERM indicates that the page will be held for an indefinite
	 * time period _often_ under userspace control.  This is in contrast to
	 * iov_iter_get_pages(), whose usages are transient.
	 */
	FOLL_LONGTERM = 1 << 8,

...

```

Why `FOLL_LONGTERM`?

```
https://www.kernel.org/doc/Documentation/core-api/pin_user_pages.rst

CASE 2: RDMA
------------
There are GUP references to pages that are serving as DMA
buffers. These buffers are needed for a long time ("long term"). No special
synchronization with folio_mkclean() or munmap() is provided. Therefore, flags
to set at the call site are: ::

    FOLL_PIN | FOLL_LONGTERM

NOTE: Some pages, such as DAX pages, cannot be pinned with longterm pins. That's
because DAX pages do not have a separate page cache, and so "pinning" implies
locking down file system blocks, which is not (yet) supported in that way.
```


As a final diagnostic step, I tried changing the Memory Type Range Registers (MTRRs). 
MTRRs controls the caching policies for physical memory ranges. 
While this can sometimes influence memory behavior, it did not resolve the pinning issue. 

NOTE: modifying MTRRs is not a viable solution as I noticed significant system slowdown and increased CPU usage.

```sh
[mshehu@pc-tbed-felix-02 cma-dma-numa]$ sudo cat /proc/mtrr
reg00: base=0x080000000 ( 2048MB), size= 2048MB, count=1: uncachable
reg01: base=0x380000000000 (58720256MB), size=524288MB, count=1: uncachable
reg02: base=0x383ffa000000 (58982304MB), size=   32MB, count=1: write-through
reg03: base=0x383ffc000000 (58982336MB), size=   32MB, count=1: write-through
```

## Conclusion
Contiguous Memory Allocator (CMA) is an easy to use mechanism for providing large, physically contiguous memory blocks, 
that reservers for you at boot time the contiguus memry pool.
However, it is fundamentally incompatible with  zero-copy RDMA (at least how we implement it) that require long-term memory pinning.
