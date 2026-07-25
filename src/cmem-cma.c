/**
 * @file cmem_cma_module.c
 * @brief Kernel module for coherent DMA buffer allocation using CMA with NUMA awareness
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/topology.h>
#include <linux/mm.h>
#include <linux/dma-map-ops.h>
#include <linux/types.h>
#include "cmem-cma.h"

/**
 * @struct dma_buffer
 * @brief Internal structure used to track allocated DMA buffers
 */
struct dma_buffer {
    void *vaddr;         /**< Virtual address visible to the kernel */
    dma_addr_t dma_addr; /**< Physical (DMA) address of the buffer */
    size_t size;         /**< Size of the buffer in bytes */
    int numa_node;       /**< NUMA node to which the buffer is bound */
    bool allocated;      /**< Allocation state of the buffer */
    struct page *pages;  /**< Page pointer for mmap support */
    unsigned long pfn;   /**< Page frame number for mmap */
};

static int major_number;
static struct class *cmem_cma_class = NULL;
static struct device *cmem_cma_device = NULL;
static struct platform_device *pdev = NULL;
static struct dma_buffer buffers[MAX_BUFFERS];
static DEFINE_MUTEX(buffer_mutex);
static const struct file_operations cmem_cma_fops;
static const struct vm_operations_struct cmem_cma_vmops;

/**
 * @brief Callback for releasing the platform device (I2C for example)
 *
 * @param dev Pointer to the device being released
 */
static void cmem_cma_pdev_release(struct device *dev)
{
    // Nothing to do here at the moment.
}

/**
 * @struct cmem_cma_pdev
 *
 * @brief Platform device structure
 */
static struct platform_device cmem_cma_pdev = {
    .name = "cma-dma-device",
    .id = -1,
    .dev = {
        .release = cmem_cma_pdev_release,
        .coherent_dma_mask = DMA_BIT_MASK(64),
        .dma_mask = &cmem_cma_pdev.dev.coherent_dma_mask,
    },
};

/**
 * @brief Find the index of the first available buffer slot
 *
 * @return Index of the free buffer slot, or -1 if all are occupied
 */
static int find_free_buffer_slot(void)
{
    int i;
    for (i = 0; i < MAX_BUFFERS; i++) {
        if (!buffers[i].allocated)
            return i;
    }
    return -1;
}

/**
 * @brief Function called when starting virtual memory operations
 */
static void cmem_cma_vmopen(struct vm_area_struct *vma)
{
    pr_info("cmem_cma: Started vm operation\n");
}

/**
 * @brief Function called when ending virtual memory operations
 */
static void cmem_cma_vmclose(struct vm_area_struct *vma)
{
    pr_info("cmem_cma: Finished vm operation\n");
}

/**
 *
 */
static vm_fault_t cmem_cma_nopage(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
    int buffer_id = offset / PAGE_SIZE;
    unsigned long pfn = buffers[buffer_id].pfn;

    if(!pfn_valid(pfn))
    {
        pr_err("cmem_cma: Invalid page frame: %lx", pfn);
        return VM_FAULT_SIGBUS;
    }

    struct page *page = pfn_to_page(pfn);
    get_page(page);
    vmf->page = page;

    return 0;
}

/**
 * @struct cmem_cma_vmops
 *
 * @brief Virtual memory operations structure for the CMA DMA device
 */
static const struct vm_operations_struct cmem_cma_vmops = {
    .open = cmem_cma_vmopen,
    .close = cmem_cma_vmclose,
    .fault = cmem_cma_nopage,
};

/**
 * @brief Allocate a DMA buffer from the CMA region
 *
 * @param req Pointer to allocation request structure from user space
 * @return 0 on success, -ENOMEM on failure
 */
static int cmem_cma_alloc_buffer(struct cmem_cma_alloc_req *req)
{
    int slot;
    void *vaddr;
    dma_addr_t dma_addr;
    int target_node = req->numa_node;

    mutex_lock(&buffer_mutex);

    slot = find_free_buffer_slot();
    if (slot < 0) {
        pr_err("cmem_cma: No free buffer slots available\n");
        mutex_unlock(&buffer_mutex);
        return -ENOMEM;
    }

    // Validate NUMA node
    if (target_node < -1 || target_node >= nr_node_ids) {
        pr_err("cmem_cma: Invalid NUMA node %d\n", target_node);
        mutex_unlock(&buffer_mutex);
        return -EINVAL;
    }

    // Set device NUMA node preference if specified
    if (target_node != NUMA_NO_NODE && target_node != -1) {
        set_dev_node(&pdev->dev, target_node);
    }

    // Allocate DMA coherent memory from CMA
    vaddr = dma_alloc_coherent(&pdev->dev, req->size, &dma_addr, GFP_KERNEL);
    if (!vaddr) {
        pr_err("cmem_cma: Failed to allocate DMA buffer of size %zu\n", req->size);
        mutex_unlock(&buffer_mutex);
        return -ENOMEM;
    }

    // Store buffer information
    buffers[slot].vaddr = vaddr;
    buffers[slot].dma_addr = dma_addr;
    buffers[slot].size = req->size;
    buffers[slot].numa_node = target_node;
    buffers[slot].allocated = true;

    // Get page information for mmap support
    buffers[slot].pfn = page_to_pfn(buffers[slot].pages);
    buffers[slot].pages = virt_to_page(vaddr);

    // Return information to userspace
    req->dma_addr = dma_addr;
    req->user_addr = (unsigned long)vaddr;
    req->buffer_id = slot;

    pr_info("cmem_cma: Allocated %zu bytes at DMA addr 0x%llx, buffer ID %d, NUMA node %d, PFN 0x%lx\n",
            req->size, dma_addr, slot, target_node, buffers[slot].pfn);

    mutex_unlock(&buffer_mutex);

    return 0;
}

/**
 * @brief Free a previously allocated DMA buffer
 *
 * @param req Pointer to free request containing the buffer ID
 * @return 0 on success, -EINVAL if invalid ID or already free
 */
static int cmem_cma_free_buffer(struct cmem_cma_free_req *req)
{
    int slot = req->buffer_id;

    if (slot < 0 || slot >= MAX_BUFFERS)
        return -EINVAL;

    mutex_lock(&buffer_mutex);

    if (!buffers[slot].allocated) {
        mutex_unlock(&buffer_mutex);
        return -EINVAL;
    }

    dma_free_coherent(&pdev->dev, buffers[slot].size, 
                      buffers[slot].vaddr, buffers[slot].dma_addr);

    pr_info("cmem_cma: Freed buffer ID %d, size %zu bytes\n", 
            slot, buffers[slot].size);

    memset(&buffers[slot], 0, sizeof(struct dma_buffer));

    mutex_unlock(&buffer_mutex);

    return 0;
}

/**
 * @brief Retrieve module statistics and NUMA information
 *
 * @param info Pointer to user-space structure to fill with info
 * @return 0 on success
 */
static int cmem_cma_get_info(struct cmem_cma_info *info)
{
    int i, count = 0;
    size_t total = 0;

    mutex_lock(&buffer_mutex);

    for (i = 0; i < MAX_BUFFERS; i++) {
        if (buffers[i].allocated) {
            count++;
            total += buffers[i].size;
        }
    }

    info->num_buffers = count;
    info->total_allocated = total;
    info->numa_nodes_available = nr_node_ids;
    
    mutex_unlock(&buffer_mutex);
    
    return 0;
}

/**
 * @brief Memory mapping support for DMA buffers
 *
 * @param filp pointer to the file structure
 * @param vma pointer to the virtual memory area
 * @return 0 on success, negative error code on failure
 */
static int cmem_cma_mmap(struct file *filp, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

    pr_info("cmem_cma: mmap called, size=%lu, offset=%lu\n", size, offset);

    // Find the buffer by ID (passed as offset)
    int buffer_id = offset / PAGE_SIZE;  // Simple mapping: offset = buffer_id * PAGE_SIZE

    if (buffer_id < 0 || buffer_id >= MAX_BUFFERS) {
        pr_err("cmem_cma: Invalid buffer ID %d for mmap\n", buffer_id);
        return -EINVAL;
    }

    mutex_lock(&buffer_mutex);

    if (!buffers[buffer_id].allocated) {
        pr_err("cmem_cma: Buffer ID %d not allocated\n", buffer_id);
        mutex_unlock(&buffer_mutex);
        return -EINVAL;
    }

    if (size > buffers[buffer_id].size) {
        pr_err("cmem_cma: mmap size %lu exceeds buffer size %zu\n",
               size, buffers[buffer_id].size);
        mutex_unlock(&buffer_mutex);
        return -EINVAL;
    }

    unsigned long pfn = buffers[buffer_id].pfn;

    size_t max_len = buffers[buffer_id].size;
    unsigned long len;
    unsigned long pgoff;

    /*
     * Check the requested size of the region is within range
     */
    len = vma->vm_end - vma->vm_start;
    if (len > max_len)
        return -EINVAL;

    /*
     * We need to temporarily clear vm_pgoff for dma_mmap_coherent()
     */
    pgoff = vma->vm_pgoff;
    vma->vm_pgoff = 0;
    int ret = dma_mmap_coherent(&pdev->dev, vma, buffers[buffer_id].vaddr, buffers[buffer_id].dma_addr, len);
    vma->vm_pgoff = pgoff;
    vma->vm_ops = &cmem_cma_vmops;
    cmem_cma_vmopen(vma);

    if(ret){
        pr_err("cmem_cma: Failed allocating coherent memory! kernel virtual address: %pK - dma address: %llx - size of allocation: %zu", 
       buffers[buffer_id].vaddr,
       (unsigned long long)buffers[buffer_id].dma_addr,
       len);
    }

    mutex_unlock(&buffer_mutex);

    pr_info("cmem_cma: Successfully mapped buffer %d, size %lu, pfn 0x%lx\n",
            buffer_id, size, pfn);

    return 0;
}

/**
 * @brief IOCTL interface for the DMA allocator device
 *
 * @param filp Pointer to the open file
 * @param cmd IOCTL command identifier
 * @param arg User-space pointer to argument structure
 * @return 0 on success, negative error code on failure
 */
static long cmem_cma_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    
    switch (cmd) {
    case CMEM_CMA_ALLOC: {
        struct cmem_cma_alloc_req req;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        
        ret = cmem_cma_alloc_buffer(&req);
        if (ret)
            return ret;
        
        if (copy_to_user((void __user *)arg, &req, sizeof(req)))
            return -EFAULT;
        break;
    }
    
    case CMEM_CMA_FREE: {
        struct cmem_cma_free_req req;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        
        ret = cmem_cma_free_buffer(&req);
        break;
    }
    
    case CMEM_CMA_GET_INFO: {
        struct cmem_cma_info info;
        ret = cmem_cma_get_info(&info);
        if (ret)
            return ret;
        
        if (copy_to_user((void __user *)arg, &info, sizeof(info)))
            return -EFAULT;
        break;
    }
    
    default:
        return -ENOTTY;
    }
    
    return ret;
}

/**
 * @brief Free all allocated buffers
 */
static void cmem_cma_free_all_buffers(void)
{
    mutex_lock(&buffer_mutex);
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (buffers[i].allocated) {
            dma_free_coherent(&pdev->dev, buffers[i].size,
                              buffers[i].vaddr, buffers[i].dma_addr);
            pr_info("cmem_cma: Freed buffer %d \n", i);
        }
    }
    mutex_unlock(&buffer_mutex);
}

/**
 * @brief Called when the device file is opened
 *
 * @param inode Pointer to inode structure
 * @param filp Pointer to file structure
 * @return Always returns 0
 */
static int cmem_cma_open(struct inode *inode, struct file *filp)
{
    pr_info("cmem_cma: Device opened\n");
    return 0;
}

/**
 * @brief Called when the device file is closed
 *
 * @param inode Pointer to inode structure
 * @param filp Pointer to file structure
 * @return Always returns 0
 */
static int cmem_cma_release(struct inode *inode, struct file *filp)
{
    pr_info("cmem_cma: Device closed\n");
    return 0;
}

/**
 * @struct cmem_cma_fops
 *
 * @brief File operations structure for the CMA DMA device
 */
static const struct file_operations cmem_cma_fops = {
    .owner = THIS_MODULE,
    .open = cmem_cma_open,
    .release = cmem_cma_release,
    .unlocked_ioctl = cmem_cma_ioctl,
    .compat_ioctl = cmem_cma_ioctl,
    .mmap = cmem_cma_mmap,
};

/**
 * @brief Module initialization function
 *
 * Registers the platform device, character device, and sets up DMA.
 *
 * @return 0 on success, negative error code on failure
 */
static int __init cmem_cma_init(void)
{
    int ret;

    pr_info("cmem_cma: Initializing CMA DMA allocator module\n");

    // Register platform device
    ret = platform_device_register(&cmem_cma_pdev);
    if (ret) {
        pr_err("cmem_cma: Failed to register platform device\n");
        return ret;
    }
    pdev = &cmem_cma_pdev;

    // Set up DMA configuration
    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    if (ret) {
        pr_warn("cmem_cma: Failed to set 64-bit DMA mask, trying 32-bit\n");
        ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
        if (ret) {
            pr_err("cmem_cma: Failed to set DMA mask\n");
            goto err_platform;
        }
    }

    // Register character device
    major_number = register_chrdev(0, DEVICE_NAME, &cmem_cma_fops);
    if (major_number < 0) {
        pr_err("cmem_cma: Failed to register character device\n");
        ret = major_number;
        goto err_platform;
    }

    // Create device class
    cmem_cma_class = class_create(CLASS_NAME);
    if (IS_ERR(cmem_cma_class)) {
        pr_err("cmem_cma: Failed to create device class\n");
        ret = PTR_ERR(cmem_cma_class);
        goto err_chrdev;
    }

    // Create device
    cmem_cma_device = device_create(cmem_cma_class, NULL, MKDEV(major_number, 0), 
                                   NULL, DEVICE_NAME);
    if (IS_ERR(cmem_cma_device)) {
        pr_err("cmem_cma: Failed to create device\n");
        ret = PTR_ERR(cmem_cma_device);
        goto err_class;
    }

    // Initialize buffer array
    memset(buffers, 0, sizeof(buffers));

    pr_info("cmem_cma: Module loaded successfully, major number %d\n", major_number);
    pr_info("cmem_cma: NUMA nodes available: %d\n", nr_node_ids);

    return 0;

err_class:
    class_destroy(cmem_cma_class);
err_chrdev:
    unregister_chrdev(major_number, DEVICE_NAME);
err_platform:
    platform_device_unregister(&cmem_cma_pdev);
    return ret;
}

/**
 * @brief Module cleanup function
 *
 * Frees any remaining DMA buffers, unregisters devices and class
 */
static void __exit cmem_cma_exit(void)
{
    pr_info("cmem_cma: Cleaning up module\n");

    cmem_cma_free_all_buffers();

    // Cleanup device and class
    device_destroy(cmem_cma_class, MKDEV(major_number, 0));
    class_destroy(cmem_cma_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    platform_device_unregister(&cmem_cma_pdev);

    pr_info("cmem_cma: Module unloaded\n");
}

module_init(cmem_cma_init);
module_exit(cmem_cma_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mario Shehu");
MODULE_DESCRIPTION("CMA DMA Buffer Allocator with NUMA Support");
MODULE_VERSION("1.1");
