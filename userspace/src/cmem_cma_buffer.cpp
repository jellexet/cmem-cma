#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <limits>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cmem_cma.hpp"
#include "cmem_cma_buffer.hpp"
#include "simple_logger.hpp"

namespace cmem {
    CmemCmaBuffer::CmemCmaBuffer(const std::filesystem::path& device_path)
    {
        m_fd = open(device_path.c_str(), O_RDWR);
        if (m_fd < 0) {
            const int err = errno;
            std::string err_msg =
              std::format("Failed to open device: {}. Error: {}", device_path.string(), strerror(err));
            LOG_FATAL(err_msg);
            throw std::domain_error(err_msg);
        }

        LOG_INFO("cmem_cma device opened successfully. FD: ", m_fd);
        m_is_open.store(true);
    }

    void CmemCmaBuffer::allocate(uint64_t size, int numa_node)
    {
        LOG_DEBUG(std::format("Calling allocate with size: {} and numa node: {}", size, numa_node));

        if (not m_is_open.load()) {
            std::string error_msg = "Character device is not open, cannot perform any operation";
            LOG_ERROR(error_msg);
            throw std::logic_error(error_msg);
        }

        if (size > std::numeric_limits<std::size_t>::max()) {
            LOG_ERROR(
              std::format("Size {} exceeds the maximum allowed of {}", size, std::numeric_limits<std::size_t>::max()));
            throw std::length_error(
              std::format("Size {} exceeds the maximum allowed of {}", size, std::numeric_limits<std::size_t>::max()));
        }

        struct cmem_cma_alloc_req alloc_req{};
        alloc_req.size = size;
        alloc_req.numa_node = numa_node;

        if (ioctl(m_fd, CMEM_CMA_ALLOC, &alloc_req) < 0) {
            const int err = errno;
            LOG_ERROR("Error from ioctl CMEM_RCC_ALLOCATE = ", strerror(err));
            throw std::domain_error(std::format("allocate: ioctl GET failed: {}", strerror(err)));
        }

        if (!alloc_req.dma_addr) {
            LOG_ERROR("Error from ioctl Physical Address = 0)");
            throw std::domain_error("Physical address returned 0");
        }

        LOG_TRACE("Calling mmap. size=", std::hex, alloc_req.size, " offset=", alloc_req.mmap_offset);

        void* mapped_addr = ::mmap(
          nullptr, alloc_req.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, static_cast<long>(alloc_req.dma_addr));

        if (mapped_addr == MAP_FAILED) {
            const int err = errno;
            LOG_ERROR("Error from mmap = ", strerror(err));
            throw std::domain_error(std::format("allocate: mmap failed: {}", strerror(err)));
        }

        m_virt_addr = mapped_addr;
        m_size = alloc_req.size;
        m_offset = alloc_req.mmap_offset;
        m_phys_address = alloc_req.dma_addr;
        m_numa_node = alloc_req.numa_node;
        m_buffer_id = alloc_req.buffer_id;

        LOG_TRACE(std::format("Virtual address = {:X}", (__u64)m_virt_addr));
        LOG_INFO(std::format("Allocation done. Buffer ID: ", m_buffer_id));
    }

    void CmemCmaBuffer::deallocate()
    {
        LOG_DEBUG("Calling deallocate");
        munmap(m_virt_addr, m_size);

        struct cmem_cma_free_req free_req{};
        free_req.buffer_id = m_buffer_id;

        if (ioctl(m_fd, CMEM_CMA_FREE, &free_req) < 0) {
            const int err = errno;
            LOG_ERROR("Error from ioctl CMEM_RCC_FREE = ", strerror(err));
            throw std::domain_error(std::format("allocate: ioctl GET failed: {}", strerror(err)));
        }
        m_virt_addr = nullptr;
        m_size = 0;
        m_offset = 0;
        m_phys_address = 0;
        m_numa_node = -1;
        m_buffer_id = -1;
    };

    CmemCmaBuffer::~CmemCmaBuffer()
    {
        deallocate();
        if (m_fd >= 0) {
            close(m_fd);
            m_fd = -1;
        }
    }
};  // namespace cmem
