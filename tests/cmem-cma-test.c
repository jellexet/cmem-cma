#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sched.h>
#include <numa.h>
#include <numaif.h>
#include <time.h>
#include "cmem-cma.h"

#define DEVICE_PATH "/dev/cmem_cma"
#define MAX_TEST_BUFFERS 8

struct test_buffer {
    int buffer_id;
    size_t size;
    int numa_node;
    uint64_t dma_addr;
    unsigned long mmap_offset;
    void *mapped_addr; 
};

static void print_usage(const char *prog_name)
{
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -h, --help        Show this help message\n");
    printf("  -i, --info        Show device information\n");
    printf("  -a, --alloc       Run allocation test with memory access\n");
    printf("  -s, --size SIZE   Buffer size in bytes (default: 4096)\n");
    printf("  -n, --node NODE   NUMA node (-1 for any, default: -1)\n");
    printf("  -c, --count COUNT Number of buffers to allocate (default: 4)\n");
    printf("  -t, --test        Run comprehensive test with memory access\n");
    printf("  -b, --bench       Run memory benchmark\n");
}

static int get_numa_info(void)
{
    if (numa_available() == -1) {
        printf("NUMA is not available on this system\n");
        return -1;
    }

    printf("NUMA Information:\n");
    printf("  Available nodes: %d\n", numa_num_configured_nodes());
    printf("  Current node: %d\n", numa_node_of_cpu(sched_getcpu()));
    
    struct bitmask *nodes = numa_get_mems_allowed();
    printf("  Allowed memory nodes: ");
    for (int i = 0; i < numa_num_configured_nodes(); i++) {
        if (numa_bitmask_isbitset(nodes, i)) {
            printf("%d ", i);
        }
    }
    printf("\n");
    numa_bitmask_free(nodes);

    return 0;
}

static const char *g_prog_name = "cmem_cma_test";

static void explain_alloc_errno(int err)
{
    if (err == EPERM) {
        fprintf(stderr,
            "  -> CMEM_CMA_ALLOC requires the CAP_SYS_RAWIO capability "
            "(it returns a raw DMA/physical\n"
            "     address to userspace). Run this test as root, e.g. "
            "'sudo %s ...', or grant the\n"
            "     binary the capability directly: "
            "'sudo setcap cap_sys_rawio+ep %s'\n",
            g_prog_name, g_prog_name);
    }
}

static int show_device_info(int fd)
{
    struct cmem_cma_info info;

    if (ioctl(fd, CMEM_CMA_GET_INFO, &info) < 0) {
        perror("Failed to get device info");
        return -1;
    }

    printf("Device Information:\n");
    printf("  Active buffers: %d\n", info.num_buffers);
    printf("  Total allocated: %zu bytes (%.2f MB)\n", 
           info.total_allocated, (double)info.total_allocated / (1024*1024));
    printf("  NUMA nodes available: %d\n", info.numa_nodes_available);

    return 0;
}

static void* map_dma_buffer(int fd, int buffer_id, size_t size, off_t mmap_offset)
{
    void *mapped_addr;

    mapped_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mmap_offset);
    if (mapped_addr == MAP_FAILED) {
        perror("mmap failed");
        return NULL;
    }

    printf("  Mapped buffer %d to userspace address: %p\n", buffer_id, mapped_addr);
    return mapped_addr;
}

static int unmap_dma_buffer(void *mapped_addr, size_t size)
{
    if (munmap(mapped_addr, size) == -1) {
        perror("munmap failed");
        return -1;
    }
    return 0;
}

static int test_memory_access(void *mapped_addr, size_t size, int buffer_id)
{
    uint32_t *buffer = (uint32_t *)mapped_addr;
    size_t num_words = size / sizeof(uint32_t);
    uint32_t test_pattern = 0xDEADBEEF + buffer_id;

    printf("  Testing memory access on buffer %d:\n", buffer_id);
    printf("    Size: %zu bytes (%zu words)\n", size, num_words);
    printf("    Test pattern: 0x%08X\n", test_pattern);

    printf("    Writing test pattern... ");
    for (size_t i = 0; i < num_words; i++) {
        buffer[i] = test_pattern + i;
    }
    printf("OK\n");

    printf("    Verifying data... ");
    for (size_t i = 0; i < num_words; i++) {
        if (buffer[i] != (test_pattern + i)) {
            printf("FAILED at word %zu: expected 0x%08X, got 0x%08X\n", 
                   i, (unsigned int)(test_pattern + i), (unsigned int)buffer[i]);
            return -1;
        }
    }
    printf("OK\n");

    uint8_t *byte_buffer = (uint8_t *)mapped_addr;
    printf("    Testing byte access... ");
    byte_buffer[0] = 0xAA;
    byte_buffer[size - 1] = 0x55;
    if (byte_buffer[0] != 0xAA || byte_buffer[size - 1] != 0x55) {
        printf("FAILED\n");
        return -1;
    }
    printf("OK\n");

    return 0;
}

static int benchmark_memory(void *mapped_addr, size_t size, int buffer_id)
{
    uint64_t *buffer = (uint64_t *)mapped_addr;
    size_t num_words = size / sizeof(uint64_t);
    struct timespec start, end;
    double elapsed, bandwidth;

    printf("  Memory benchmark for buffer %d (%zu bytes):\n", buffer_id, size);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (size_t i = 0; i < num_words; i++) {
        buffer[i] = i;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    bandwidth = (size / (1024.0 * 1024.0)) / elapsed;
    printf("    Write: %.2f MB/s (%.6f seconds)\n", bandwidth, elapsed);

    volatile uint64_t dummy = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (size_t i = 0; i < num_words; i++) {
        dummy += buffer[i];
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    bandwidth = (size / (1024.0 * 1024.0)) / elapsed;
    printf("    Read: %.2f MB/s (%.6f seconds)\n", bandwidth, elapsed);

    if (dummy == 0xFFFFFFFF) printf("Impossible\n");

    return 0;
}

static int test_allocation_with_access(int fd, size_t size, int numa_node)
{
    struct cmem_cma_alloc_req alloc_req = {0};
    struct cmem_cma_free_req free_req = {0};
    void *mapped_addr = NULL;
    int ret = 0;

    alloc_req.size = size;
    alloc_req.numa_node = numa_node;

    printf("Allocating %zu bytes on NUMA node %d...\n", size, numa_node);

    if (ioctl(fd, CMEM_CMA_ALLOC, &alloc_req) < 0) {
        perror("Allocation failed");
        explain_alloc_errno(errno);
        return -1;
    }

    printf("Success!\n");
    printf("  Buffer ID: %d\n", alloc_req.buffer_id);
    printf("  DMA Address: 0x%lx\n", (unsigned long)alloc_req.dma_addr);
    printf("  mmap offset: 0x%lx (pass as the offset argument to mmap())\n",
           alloc_req.mmap_offset);

    mapped_addr = map_dma_buffer(fd, alloc_req.buffer_id, size, alloc_req.mmap_offset);
    if (!mapped_addr) {
        ret = -1;
        goto cleanup;
    }

    if (test_memory_access(mapped_addr, size, alloc_req.buffer_id) < 0) {
        ret = -1;
        goto cleanup;
    }

    benchmark_memory(mapped_addr, size, alloc_req.buffer_id);

cleanup:
    if (mapped_addr) {
        unmap_dma_buffer(mapped_addr, size);
    }

    free_req.buffer_id = alloc_req.buffer_id;
    printf("Freeing buffer %d...\n", free_req.buffer_id);

    if (ioctl(fd, CMEM_CMA_FREE, &free_req) < 0) {
        perror("Free failed");
        return -1;
    }

    printf("Buffer freed successfully\n");
    return ret;
}

static int comprehensive_test(int fd)
{
    struct test_buffer buffers[MAX_TEST_BUFFERS];
    size_t test_sizes[] = {4096, 8192, 65536, 1024*1024};
    int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);
    int numa_nodes = numa_num_configured_nodes();
    int allocated_count = 0;

    printf("Running comprehensive test with memory access...\n");
    printf("Testing %d different buffer sizes\n", num_sizes);

    for (int node = -1; node < numa_nodes && allocated_count < MAX_TEST_BUFFERS; node++) {
        for (int i = 0; i < num_sizes && allocated_count < MAX_TEST_BUFFERS; i++) {
            struct cmem_cma_alloc_req req = {0};

            req.size = test_sizes[i];
            req.numa_node = node;

            printf("Test %d: Allocating %zu bytes on node %d... ", 
                   allocated_count + 1, req.size, node);

            if (ioctl(fd, CMEM_CMA_ALLOC, &req) < 0) {
                printf("FAILED (%s)\n", strerror(errno));
                explain_alloc_errno(errno);
                continue;
            }

            printf("OK (ID: %d, DMA: 0x%lx)\n", 
                   req.buffer_id, (unsigned long)req.dma_addr);

            buffers[allocated_count].buffer_id = req.buffer_id;
            buffers[allocated_count].size = req.size;
            buffers[allocated_count].numa_node = node;
            buffers[allocated_count].dma_addr = req.dma_addr;
            buffers[allocated_count].mmap_offset = req.mmap_offset;

            buffers[allocated_count].mapped_addr =
                map_dma_buffer(fd, req.buffer_id, req.size, req.mmap_offset);
            if (!buffers[allocated_count].mapped_addr) {
                printf("  WARNING: Failed to map buffer %d\n", req.buffer_id);
            }

            allocated_count++;
        }
    }
    
    printf("\nAllocated %d buffers total\n", allocated_count);

    printf("\nTesting memory access on all buffers:\n");
    for (int i = 0; i < allocated_count; i++) {
        if (buffers[i].mapped_addr) {
            printf("Buffer %d:\n", buffers[i].buffer_id);
            test_memory_access(buffers[i].mapped_addr, buffers[i].size, buffers[i].buffer_id);
        }
    }

    printf("\nCurrent device status:\n");
    show_device_info(fd);

    printf("\nCleaning up buffers...\n");
    for (int i = 0; i < allocated_count; i++) {
        if (buffers[i].mapped_addr) {
            printf("Unmapping buffer %d... ", buffers[i].buffer_id);
            if (unmap_dma_buffer(buffers[i].mapped_addr, buffers[i].size) == 0) {
                printf("OK\n");
            } else {
                printf("FAILED\n");
            }
        }

        struct cmem_cma_free_req free_req = {0};
        free_req.buffer_id = buffers[i].buffer_id;

        printf("Freeing buffer %d (size %zu)... ", 
               buffers[i].buffer_id, buffers[i].size);
        
        if (ioctl(fd, CMEM_CMA_FREE, &free_req) < 0) {
            printf("FAILED (%s)\n", strerror(errno));
        } else {
            printf("OK\n");
        }
    }

    // Final device info
    printf("\nFinal device status:\n");
    show_device_info(fd);

    return 0;
}

static int memory_benchmark_test(int fd)
{
    size_t test_sizes[] = {4096, 64*1024, 1024*1024, 4*1024*1024, 16*1024*1024};
    int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

    printf("Running memory benchmark on different buffer sizes...\n");

    for (int i = 0; i < num_sizes; i++) {
        struct cmem_cma_alloc_req req = {0};
        struct cmem_cma_free_req free_req = {0};
        void *mapped_addr = NULL;

        req.size = test_sizes[i];
        req.numa_node = -1;  // Any node

        printf("\nBenchmark %d: %zu bytes (%.1f MB)\n", 
               i + 1, req.size, (double)req.size / (1024*1024));

        if (ioctl(fd, CMEM_CMA_ALLOC, &req) < 0) {
            printf("  Allocation failed: %s\n", strerror(errno));
            explain_alloc_errno(errno);
            continue;
        }

        mapped_addr = map_dma_buffer(fd, req.buffer_id, req.size, req.mmap_offset);
        if (!mapped_addr) {
            printf("  Mapping failed\n");
            goto cleanup_bench;
        }

        benchmark_memory(mapped_addr, req.size, req.buffer_id);

        unmap_dma_buffer(mapped_addr, req.size);

cleanup_bench:
        free_req.buffer_id = req.buffer_id;
        ioctl(fd, CMEM_CMA_FREE, &free_req);
    }

    return 0;
}


int main(int argc, char *argv[])
{
    int fd;
    int show_info = 0, run_alloc = 0, run_test = 0, run_bench = 0;
    size_t buffer_size = 4096;
    int numa_node = -1;

    g_prog_name = argv[0];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--info") == 0) {
            show_info = 1;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--alloc") == 0) {
            run_alloc = 1;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--test") == 0) {
            run_test = 1;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bench") == 0) {
            run_bench = 1;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--size") == 0) {
            if (i + 1 < argc) {
                buffer_size = strtoull(argv[++i], NULL, 0);
            }
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--node") == 0) {
            if (i + 1 < argc) {
                numa_node = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--count") == 0) {
            if (i + 1 < argc) {
                i++; // Skip the count value for now
            }
        }
    }

    if (!show_info && !run_alloc && !run_test && !run_bench) {
        print_usage(argv[0]);
        return 1;
    }

    printf("CMA DMA Buffer Test Program with Memory Access\n");
    printf("==============================================\n\n");

    get_numa_info();
    printf("\n");

    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        printf("Make sure the kernel module is loaded and device exists at %s\n", DEVICE_PATH);
        return 1;
    }

    printf("Successfully opened device %s\n\n", DEVICE_PATH);

    if (show_info) {
        show_device_info(fd);
        printf("\n");
    }

    if (run_alloc) {
        test_allocation_with_access(fd, buffer_size, numa_node);
        printf("\n");
    }

    if (run_test) {
        comprehensive_test(fd);
        printf("\n");
    }

    if (run_bench) {
        memory_benchmark_test(fd);
        printf("\n");
    }

    close(fd);
    return 0;
}
