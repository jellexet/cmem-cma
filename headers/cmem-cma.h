#ifndef CMEM_CMA_H
#define CMEM_CMA_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DEVICE_NAME "cmem_cma" /**< Device name as seen in /dev */
#define CLASS_NAME "cmem_cma"  /**< Device class name */

/**
 * @def CMEM_CMA_IOC_MAGIC
 * @brief IOCTL magic number used for CMA DMA commands
 *
 * 'C' is chosen arbitrarily but it simply must be unique among all IOCTL
 * command groups. 'C' is used by soundcard.h or capi.h, which should not create
 * any problems. See:
 * https://www.kernel.org/doc/html/latest/userspace-api/ioctl/ioctl-number.html
 */
#define CMEM_CMA_IOC_MAGIC 'C'

/**
 * @def CMEM_CMA_ALLOC
 * @brief IOCTL command to allocate a DMA buffer
 */
#define CMEM_CMA_ALLOC _IOWR(CMEM_CMA_IOC_MAGIC, 1, struct cmem_cma_alloc_req)

/**
 * @def CMEM_CMA_FREE
 * @brief IOCTL command to free a previously allocated DMA buffer
 */
#define CMEM_CMA_FREE _IOW(CMEM_CMA_IOC_MAGIC, 2, struct cmem_cma_free_req)

/**
 * @def CMEM_CMA_GET_INFO
 * @brief IOCTL command to retrieve general information from the driver
 */
#define CMEM_CMA_GET_INFO _IOR(CMEM_CMA_IOC_MAGIC, 3, struct cmem_cma_info)

/**
 * @struct cmem_cma_alloc_req
 * @brief Request/response structure for DMA buffer allocation
 *
 * Userspace initializes `size` and optionally `numa_node`. Kernel fills in the
 * rest.
 */
struct cmem_cma_alloc_req {
  size_t size;    /**< Requested size of the DMA buffer (in bytes) */
  int numa_node;  /**< NUMA node to allocate from, or -1 for default */
  __u64 dma_addr; /**< (Out) DMA address (physical) of the allocated buffer */
  unsigned long user_addr; /**< (Out) User-space accessible virtual address */
  int buffer_id;           /**< (Out) Internal ID of the allocated buffer */
};

/**
 * @struct cmem_cma_free_req
 * @brief Request structure for DMA buffer deallocation
 */
struct cmem_cma_free_req {
  int buffer_id; /**< ID of the buffer to free */
};

/**
 * @struct cmem_cma_info
 * @brief Information structure returned by the CMEM_CMA_GET_INFO ioctl
 */
struct cmem_cma_info {
  int num_buffers;          /**< Current number of allocated buffers */
  size_t total_allocated;   /**< Total allocated memory (in bytes) */
  int numa_nodes_available; /**< Number of NUMA nodes detected by the kernel */
};

#endif /* CMEM_CMA_H */
