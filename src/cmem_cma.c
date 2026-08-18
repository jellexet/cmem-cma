/**
 * @file cmem_cma_module.c
 * @brief Kernel module for coherent DMA buffer allocation using CMA with NUMA
 * awareness
 */

#include "cmem_cma.h"
#include "linux/printk.h"
#include <linux/capability.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/nodemask.h>
#include <linux/numa.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/topology.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

/* Minimum size of a single buffer allocation */
#define CMEM_CMA_MIN_BUFFER_SIZE PAGE_SIZE

/* Default value of the max_allocation_size module parameter, in bytes. */
#define CMEM_CMA_DEFAULT_MAX_ALLOC_SIZE (8UL * 1024 * 1024 * 1024) /* 8 GiB */

/* Maximum number of concurrently open buffers */
#define CMEM_CMA_HARD_MAX_BUFFERS 65536

/* Read-only encoding. */
#define READONLY 0444

/**
 * @struct dma_buffer
 * @brief Internal structure used to track a single allocated DMA buffer
 *
 * Instances are heap-allocated and owned by xarray (cmem_buffers).
 */
struct dma_buffer {
    void* vaddr;         /**< Virtual address visible to the kernel */
    dma_addr_t dma_addr; /**< Physical (DMA) address of the buffer */
    size_t size;         /**< Size of the buffer in bytes */
    s32 numa_node;       /**< NUMA node on which memory is allocated*/
    struct device* dev;  /**< Device the allocation was made against; the
                              matching device must be used for freeing */
    struct file* owner;  /**< The open file (fd) that allocated this buffer. */
    atomic_t mmap_count; /**< Counter of references of the buffer. The buffer is
                        not freed if this counter is > 0 */
};

/*
 * @brief Largest single buffer allocation the driver will hand out.
 *
 *   insmod cmem_cma.ko max_allocation_size=536870912   # 512 MiB
 *   modprobe cmem_cma max_allocation_size=1073741824   # 1 GiB
 *
 *   The parameter is read-only after load.
 */
static unsigned long max_allocation_size = CMEM_CMA_DEFAULT_MAX_ALLOC_SIZE;
static u64 total_allocated_bytes = 0;

module_param(max_allocation_size, ulong, READONLY);
MODULE_PARM_DESC(max_allocation_size,
                 "Maximum size in bytes of a single DMA buffer allocation (default 8GiB). "
                 " Read 'CmaTotal' in /proc/meminfo and set max_allocation_size to that value "
                 "at load time if you want the driver to only use CMA memory.");

/* Effective (post-clamp) maximum total RAM the driver will occupy. */
static unsigned long effective_max_alloc_size;
/* Effective (post-clamp) maximum numver of buffer. */
static u32 effective_max_buffers;

static DEFINE_XARRAY_ALLOC(cmem_buffers);
static DEFINE_MUTEX(buffer_mutex);

static int major_number;
static struct class* cmem_cma_class = NULL;
static struct device* cmem_cma_device = NULL;
static struct proc_dir_entry* cmem_cma_proc_entry = NULL;
static const struct file_operations cmem_cma_fops;

/*
 * @brief Per-NUMA-node platform devices
 */
struct cmem_cma_node_device {
    struct platform_device pdev;
    bool registered;
};

static struct cmem_cma_node_device* cmem_cma_node_devs;
static int cmem_cma_num_nodes;

/**
 * @brief Callback for releasing a per-node platform device
 */
static void cmem_cma_pdev_release(struct device* dev)
{
    /* Devices are embedded in cmem_cma_node_devs[], which we kfree()
     * ourselves in cmem_cma_unregister_node_devices(); nothing to do here. */
}

/**
 * @brief Compute the effective maximum size the driver can take in memory and
 *        the effective maximum number of buffers that can be allocated.
 */
static void cmem_cma_compute_limits(void)
{
    effective_max_alloc_size = max_allocation_size;

    effective_max_buffers =
      clamp_t(unsigned long, effective_max_alloc_size / CMEM_CMA_MIN_BUFFER_SIZE, 1, CMEM_CMA_HARD_MAX_BUFFERS);

    pr_debug("cmem_cma: effective max allocation size = %lu bytes, "
             "max concurrent buffers = %u\n",
             effective_max_alloc_size,
             effective_max_buffers);
}

/**
 * @brief Unregister all per-node platform devices
 *
 * MUST be called only after every DMA buffer has already been freed
 */
static void cmem_cma_unregister_node_devices(void)
{
    int node;

    if (!cmem_cma_node_devs)
        return;

    for_each_online_node(node)
    {
        if (cmem_cma_node_devs[node].registered)
            platform_device_unregister(&cmem_cma_node_devs[node].pdev);
    }
    kfree(cmem_cma_node_devs);
    cmem_cma_node_devs = NULL;
}

/**
 * @brief Register one platform device per online NUMA node
 *
 * @return 0 on success, negative errno on failure (nothing left registered)
 */
static int cmem_cma_register_node_devices(void)
{
    int node, ret;

    cmem_cma_num_nodes = nr_node_ids;
    cmem_cma_node_devs = kcalloc(cmem_cma_num_nodes, sizeof(*cmem_cma_node_devs), GFP_KERNEL);
    if (!cmem_cma_node_devs)
        return -ENOMEM;

    for_each_online_node(node)
    {
        struct platform_device* pdev = &cmem_cma_node_devs[node].pdev;

        pdev->name = "cma-dma-device";
        pdev->id = node;
        pdev->dev.release = cmem_cma_pdev_release;
        pdev->dev.coherent_dma_mask = DMA_BIT_MASK(64);
        pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

        set_dev_node(&pdev->dev, node);

        ret = platform_device_register(pdev);
        if (ret) {
            pr_err("cmem_cma: failed to register platform device for node %d (%d)\n", node, ret);
            goto unwind;
        }

        ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
        if (ret) {
            pr_warn("cmem_cma: node %d: 64-bit DMA mask unavailable, trying 32-bit\n", node);
            ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
        }
        if (ret) {
            pr_err("cmem_cma: node %d: failed to set DMA mask (%d)\n", node, ret);
            platform_device_unregister(pdev);
            goto unwind;
        }

        cmem_cma_node_devs[node].registered = true;
        pr_info("cmem_cma: registered DMA device for NUMA node %d\n", node);
    }

    if ((int)first_online_node >= cmem_cma_num_nodes || !cmem_cma_node_devs[first_online_node].registered) {
        pr_err("cmem_cma: no usable NUMA node device was registered\n");
        ret = -ENODEV;
        goto unwind;
    }

    return 0;

unwind:
    cmem_cma_unregister_node_devices();
    return ret;
}

/**
 * @brief Select the device to use for a given NUMA node
 *
 * @param numa_node Requested node, or NUMA_NO_NODE (-1) for "no preference"
 * @return struct platform_device pointer
 *
 * If the argment is NUMA_NO_NODE, resolve to CPU's associated NUMA node.
 */
static struct device* cmem_cma_get_node_device(int numa_node, int* out_node)
{
    int node = numa_node;

    if (node == NUMA_NO_NODE)
        node = numa_node_id();

    if (node < 0 || node >= cmem_cma_num_nodes || !cmem_cma_node_devs[node].registered) {
        pr_warn_ratelimited(
          "cmem_cma: NUMA node %d has no usable device, falling back to node %d\n", numa_node, first_online_node);
        node = first_online_node;
    }

    *out_node = node;
    return &cmem_cma_node_devs[node].pdev.dev;
}

/**
 * @brief Allocate a DMA buffer from the CMA region
 *
 * @param req Pointer to allocation request structure from user space
 * @return 0 on success, negative errno on failure
 */
static int cmem_cma_alloc_buffer(struct file* filp, struct cmem_cma_alloc_req* req)
{
    struct xa_limit limit = {.min = 0, .max = effective_max_buffers - 1};
    struct dma_buffer* buf;
    struct device* dma_dev;
    void* vaddr;
    dma_addr_t dma_addr;
    int resolved_node;
    u32 buffer_id;
    int ret;

    if (req->size == 0) {
        pr_err_ratelimited("cmem_cma: rejected zero-length allocation request\n");
        return -EINVAL;
    }

    if (req->size > effective_max_alloc_size) {
        pr_err_ratelimited(
          "cmem_cma: requested size %u exceeds max_allocation_size (%lu bytes)\n", req->size, effective_max_alloc_size);
        return -EINVAL;
    }

    if (req->numa_node < -1 || req->numa_node >= (int)nr_node_ids) {
        pr_err_ratelimited("cmem_cma: invalid NUMA node %d\n", req->numa_node);
        return -EINVAL;
    }

    mutex_lock(&buffer_mutex);
    if (total_allocated_bytes + req->size > effective_max_alloc_size) {
        mutex_unlock(&buffer_mutex);
        pr_err_ratelimited("cmem_cma: alocating %u bytes would exceed the %lu effective maximum allocation size. "
                           "%llu bytes already in use.\n",
                           req->size,
                           effective_max_alloc_size,
                           total_allocated_bytes);
        return -ENOSPC;
    }
    total_allocated_bytes += req->size;
    mutex_unlock(&buffer_mutex);

    buf = kzalloc(sizeof(*buf), GFP_KERNEL);
    if (!buf) {
        ret = -ENOMEM;
        goto err_mem;
    }

    dma_dev = cmem_cma_get_node_device(req->numa_node, &resolved_node);

    vaddr = dma_alloc_coherent(dma_dev, (size_t)req->size, &dma_addr, GFP_KERNEL);
    if (!vaddr) {
        pr_err("cmem_cma: failed to allocate DMA buffer of size %u\n", req->size);
        ret = -ENOMEM;
        goto err_buf;
    }

    buf->vaddr = vaddr;
    buf->dma_addr = dma_addr;
    buf->size = (size_t)req->size;
    buf->numa_node = resolved_node;
    buf->dev = dma_dev;
    buf->owner = filp;
    atomic_set(&buf->mmap_count, 0);

    mutex_lock(&buffer_mutex);
    ret = xa_alloc(&cmem_buffers, &buffer_id, buf, limit, GFP_KERNEL);
    mutex_unlock(&buffer_mutex);

    if (ret) {
        pr_err("cmem_cma: no free buffer IDs available (%d)\n", ret);
        dma_free_coherent(dma_dev, req->size, vaddr, dma_addr);
        ret = -EBUSY ? -ENOMEM : ret;
        goto err_buf;
    }

    req->dma_addr = dma_addr;
    req->buffer_id = buffer_id;
    req->numa_node = resolved_node;
    req->mmap_offset = (u64)buffer_id * PAGE_SIZE; /* mmap() offset convention */

    pr_debug_ratelimited("cmem_cma: allocated %u bytes at DMA addr %pad, buffer ID %u, "
                         "requested NUMA node %d\n",
                         req->size,
                         &dma_addr,
                         buffer_id,
                         req->numa_node);

    return 0;

err_buf:
    kfree(buf);
err_mem:
    mutex_lock(&buffer_mutex);
    total_allocated_bytes -= req->size;
    mutex_unlock(&buffer_mutex);
    return ret;
}

/**
 * @brief Free a previously allocated DMA buffer
 *
 * @param req Pointer to free request containing the buffer ID
 * @return 0 on success, -EINVAL if invalid ID or already free
 */
static int cmem_cma_free_buffer(struct file* filp, struct cmem_cma_free_req* req)
{
    struct dma_buffer* buf;

    if (req->buffer_id < 0)
        return -EINVAL;

    mutex_lock(&buffer_mutex);

    buf = xa_load(&cmem_buffers, req->buffer_id);
    if (!buf) {
        mutex_unlock(&buffer_mutex);
        return -EINVAL;
    }

    if (buf->owner != filp) {
        pr_warn_ratelimited("cmem_cma: Non owning fd tried CMEM_CMA_FREE of buffer %d.\n", req->buffer_id);
        mutex_unlock(&buffer_mutex);
        return -EPERM;
    }

    if (atomic_read(&buf->mmap_count) > 0) {
        pr_warn_ratelimited(
          "cmem_cma: Buffer %d still in use, %d active.\n", req->buffer_id, atomic_read(&buf->mmap_count));
        mutex_unlock(&buffer_mutex);
        return -EBUSY;
    }

    buf = xa_erase(&cmem_buffers, req->buffer_id);
    total_allocated_bytes -= buf->size;
    mutex_unlock(&buffer_mutex);

    dma_free_coherent(buf->dev, buf->size, buf->vaddr, buf->dma_addr);

    pr_debug_ratelimited("cmem_cma: freed buffer ID %d, size %lu bytes\n", req->buffer_id, buf->size);

    kfree(buf);
    return 0;
}

/**
 * @brief Retrieve module statistics and NUMA information
 *
 * @param info Pointer to user-space structure to fill with info
 * @return 0 on success
 */
static int cmem_cma_get_info(struct cmem_cma_info* info)
{
    struct dma_buffer* buf;
    unsigned long index;
    int n_buffers = 0;

    mutex_lock(&buffer_mutex);
    xa_for_each(&cmem_buffers, index, buf)
    {
        n_buffers++;
    }
    mutex_unlock(&buffer_mutex);

    info->num_buffers = n_buffers;
    info->total_allocated = total_allocated_bytes;
    info->numa_nodes_available = (int)nr_node_ids;
    info->max_allocation_size = effective_max_alloc_size;
    info->max_buffers = effective_max_buffers;
    info->reserved0 = 0;

    return 0;
}

/**
 * @brief Print internal driver information to /proc/cmem_cma
 */
static int cmem_cma_proc_show(struct seq_file* m, void* v)
{
    struct dma_buffer* buf;
    unsigned long index;
    int count = 0;
    size_t total = 0;
    const unsigned long max_allocation_MB = effective_max_alloc_size / (1024 * 1024);

    seq_puts(m, "cmem_cma driver status\n");
    seq_puts(m, "-----------------------\n");
    seq_printf(m, "max_allocation_size:   %lu MB\n", max_allocation_MB);
    seq_printf(m, "max_buffers:           %u\n", effective_max_buffers);
    seq_printf(m, "numa_nodes_available:  %u\n", nr_node_ids);
    seq_printf(m, "numa_nodes_registered: %d\n", cmem_cma_num_nodes);
    seq_puts(m, "\n");
    seq_printf(m, "%-8s %-14s %-6s %-18s\n", "ID", "SIZE(bytes)", "NODE", "DMA_ADDR");

    mutex_lock(&buffer_mutex);
    xa_for_each(&cmem_buffers, index, buf)
    {
        seq_printf(
          m, "%-8lu %-14lu %-6d 0x%016llx\n", index, buf->size, buf->numa_node, (unsigned long long)buf->dma_addr);
        count++;
        total += buf->size;
    }
    mutex_unlock(&buffer_mutex);

    const size_t total_MB = total / (1024 * 1024);

    seq_puts(m, "\n");
    seq_printf(m, "total buffers:   %d\n", count);
    seq_printf(m, "total allocated: %zu MB\n", total_MB);

    return 0;
}

static int cmem_cma_proc_open(struct inode* inode, struct file* file)
{
    if (!capable(CAP_SYS_RAWIO))
        return -EPERM;

    return single_open(file, cmem_cma_proc_show, NULL);
}

static const struct proc_ops cmem_cma_proc_ops = {
  .proc_open = cmem_cma_proc_open,
  .proc_read = seq_read,
  .proc_lseek = seq_lseek,
  .proc_release = single_release,
};

/**
 * @brief vm_area_struct ->open callback: called for every additional
 *        reference taken on a mapping created by cmem_cma_mmap().
 *        NOT called for the very first mapping.
 */
static void cmem_cma_vma_open(struct vm_area_struct* vma)
{
    struct dma_buffer* buf = vma->vm_private_data;

    atomic_inc(&buf->mmap_count);
}

/**
 * @brief vm_area_struct ->close callback: called for every reference
 *        of the buffers taken down.
 */
static void cmem_cma_vma_close(struct vm_area_struct* vma)
{
    struct dma_buffer* buf = vma->vm_private_data;

    atomic_dec(&buf->mmap_count);
}

static const struct vm_operations_struct cmem_cma_vm_ops = {
  .open = cmem_cma_vma_open,
  .close = cmem_cma_vma_close,
};

/**
 * @brief Memory mapping support for DMA buffers
 *
 * @param filp pointer to the file structure
 * @param vma pointer to the virtual memory area
 * @return 0 on success, negative error code on failure
 */
static int cmem_cma_mmap(struct file* filp, struct vm_area_struct* vma)
{
    unsigned long len = vma->vm_end - vma->vm_start;
    unsigned long saved_pgoff = vma->vm_pgoff;
    struct dma_buffer* buf;
    int buffer_id;
    int ret;

    if (saved_pgoff >= effective_max_buffers)
        return -EINVAL;
    buffer_id = (int)saved_pgoff;

    pr_info("cmem_cma: mmap called for buffer %d, len=%lu\n", buffer_id, len);

    mutex_lock(&buffer_mutex);

    buf = xa_load(&cmem_buffers, buffer_id);
    if (!buf) {
        pr_err("cmem_cma: buffer ID %d not allocated\n", buffer_id);
        mutex_unlock(&buffer_mutex);
        return -EINVAL;
    }

    if (buf->owner != filp) {
        pr_warn_ratelimited("cmem_cma: non owning fd tried mmap of buffer %d. Refused.", buffer_id);
        mutex_unlock(&buffer_mutex);
        return -EPERM;
    }

    if (len > buf->size) {
        pr_err("cmem_cma: mmap size %lu exceeds buffer size %zu\n", len, buf->size);
        mutex_unlock(&buffer_mutex);
        return -EINVAL;
    }

    vma->vm_pgoff = 0;
    ret = dma_mmap_coherent(buf->dev, vma, buf->vaddr, buf->dma_addr, len);
    vma->vm_pgoff = saved_pgoff;

    if (ret == 0) {
        vma->vm_ops = &cmem_cma_vm_ops;
        vma->vm_private_data = buf;
        atomic_inc(&buf->mmap_count);
    }

    mutex_unlock(&buffer_mutex);

    if (ret) {
        pr_err("cmem_cma: dma_mmap_coherent failed for buffer %d (vaddr=%pK, "
               "dma_addr=%pad, len=%lu): %d\n",
               buffer_id,
               buf->vaddr,
               &buf->dma_addr,
               len,
               ret);
        return ret;
    }

    pr_debug("cmem_cma: successfully mapped buffer %d, size %lu\n", buffer_id, len);
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
static long cmem_cma_ioctl(struct file* filp, unsigned int cmd, unsigned long arg)
{
    int ret = 0;

    switch (cmd) {
    case CMEM_CMA_ALLOC: {
        struct cmem_cma_alloc_req req;

        if (!capable(CAP_SYS_RAWIO))
            return -EPERM;

        if (copy_from_user(&req, (void __user*)arg, sizeof(req)))
            return -EFAULT;

        ret = cmem_cma_alloc_buffer(filp, &req);
        if (ret)
            return ret;

        if (copy_to_user((void __user*)arg, &req, sizeof(req))) {
            struct cmem_cma_free_req free_req = {.buffer_id = req.buffer_id};

            cmem_cma_free_buffer(filp, &free_req);
            return -EFAULT;
        }
        break;
    }

    case CMEM_CMA_FREE: {
        struct cmem_cma_free_req req;
        if (copy_from_user(&req, (void __user*)arg, sizeof(req)))
            return -EFAULT;

        ret = cmem_cma_free_buffer(filp, &req);
        break;
    }

    case CMEM_CMA_GET_INFO: {
        struct cmem_cma_info info;
        ret = cmem_cma_get_info(&info);
        if (ret)
            return ret;

        if (copy_to_user((void __user*)arg, &info, sizeof(info)))
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
    struct dma_buffer* buf;
    unsigned long idx;

    mutex_lock(&buffer_mutex);
    xa_for_each(&cmem_buffers, idx, buf)
    {
        dma_free_coherent(buf->dev, buf->size, buf->vaddr, buf->dma_addr);
        total_allocated_bytes -= buf->size;
        pr_debug("cmem_cma: freed buffer %lu, total_allocated_bytes %llu\n", idx, total_allocated_bytes);
        kfree(buf);
    }
    xa_destroy(&cmem_buffers);
    mutex_unlock(&buffer_mutex);
}

/**
 * @brief Free buffers owned by a single file
 *
 * On release free all the buffers owned by a specific file.
 * Avoids memory leak when a process crashes.
 */
static void cmem_cma_release_owned_buffers(struct file* filp)
{
    struct dma_buffer* buf;
    unsigned long idx;
    int freed = 0;

    mutex_lock(&buffer_mutex);
    xa_for_each(&cmem_buffers, idx, buf)
    {
        if (buf->owner != filp)
            continue;

        xa_erase(&cmem_buffers, idx);
        total_allocated_bytes -= buf->size;
        dma_free_coherent(buf->dev, buf->size, buf->vaddr, buf->dma_addr);

        kfree(buf);
        freed++;
    }
    mutex_unlock(&buffer_mutex);
    pr_debug("cmem_cma: released %d buffers held by a closing fd\n", freed);
}

/**
 * @brief Called when the device file is opened
 */
static int cmem_cma_open(struct inode* inode, struct file* filp)
{
    pr_info("cmem_cma: Device opened\n");
    return 0;
}

/**
 * @brief Called when the device file is closed
 */
static int cmem_cma_release(struct inode* inode, struct file* filp)
{
    pr_info("cmem_cma: Device closed\n");
    cmem_cma_release_owned_buffers(filp);
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
  .mmap = cmem_cma_mmap,
};

/**
 * @brief Module initialization
 *
 * Registers the per-node platform devices, character device, and class.
 *
 * @return 0 on success, negative error code on failure
 */
static int __init cmem_cma_init(void)
{
    int ret;

    pr_info("cmem_cma: Initializing CMA DMA allocator module\n");

    cmem_cma_compute_limits();

    ret = cmem_cma_register_node_devices();
    if (ret)
        return ret;

    major_number = register_chrdev(0, DEVICE_NAME, &cmem_cma_fops);
    if (major_number < 0) {
        pr_err("cmem_cma: Failed to register character device\n");
        ret = major_number;
        goto err_nodes;
    }

    cmem_cma_class = class_create(CLASS_NAME);
    if (IS_ERR(cmem_cma_class)) {
        pr_err("cmem_cma: Failed to create device class\n");
        ret = PTR_ERR(cmem_cma_class);
        goto err_chrdev;
    }

    cmem_cma_device = device_create(cmem_cma_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(cmem_cma_device)) {
        pr_err("cmem_cma: Failed to create device\n");
        ret = PTR_ERR(cmem_cma_device);
        goto err_class;
    }

    cmem_cma_proc_entry = proc_create(DEVICE_NAME, 0400, NULL, &cmem_cma_proc_ops);
    if (!cmem_cma_proc_entry)
        pr_warn("cmem_cma: failed to create /proc/%s entry (continuing without it)\n", DEVICE_NAME);

    pr_info("cmem_cma: Module loaded successfully, major number %d, "
            "%d NUMA node device(s) registered\n",
            major_number,
            cmem_cma_num_nodes);

    return 0;

err_class:
    class_destroy(cmem_cma_class);
err_chrdev:
    unregister_chrdev(major_number, DEVICE_NAME);
err_nodes:
    cmem_cma_unregister_node_devices();
    return ret;
}

/**
 * @brief Module cleanup
 */
static void __exit cmem_cma_exit(void)
{
    pr_info("cmem_cma: Cleaning up module\n");

    if (cmem_cma_proc_entry)
        proc_remove(cmem_cma_proc_entry);

    device_destroy(cmem_cma_class, MKDEV(major_number, 0));
    class_destroy(cmem_cma_class);
    unregister_chrdev(major_number, DEVICE_NAME);

    cmem_cma_free_all_buffers();
    cmem_cma_unregister_node_devices();

    pr_info("cmem_cma: Module unloaded\n");
}

module_init(cmem_cma_init);
module_exit(cmem_cma_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mario Shehu");
MODULE_DESCRIPTION("CMA DMA Buffer Allocator with NUMA Support");
MODULE_VERSION("2.1");
