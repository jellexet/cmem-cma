#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <limits>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

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

    CmemCmaBuffer::CmemCmaBuffer(CmemCmaBuffer&& other) noexcept :
      m_fd(other.m_fd),
      m_buffer_id(other.m_buffer_id),
      m_numa_node(other.m_numa_node),
      m_offset(other.m_offset),
      m_size(other.m_size),
      m_phys_address(other.m_phys_address),
      m_virt_addr(other.m_virt_addr)
    {
        m_is_open.store(other.m_is_open.load());

        other.m_fd = -1;
        other.m_buffer_id = -1;
        other.m_numa_node = -1;
        other.m_offset = 0;
        other.m_size = 0;
        other.m_phys_address = 0;
        other.m_virt_addr = nullptr;
        other.m_is_open.store(false);
    }

    void CmemCmaBuffer::allocate(uint64_t size, int numa_node)
    {
        LOG_DEBUG(std::format("Calling allocate with size: {} and numa node: {}", size, numa_node));

        if (not m_is_open.load()) {
            std::string error_msg = "Character device is not open, cannot perform any operation";
            LOG_ERROR(error_msg);
            throw std::logic_error(error_msg);
        }

        if (m_virt_addr != nullptr || m_buffer_id >= 0) {
            std::string error_msg = "CmemCmaBuffer already holds an allocated buffer; call deallocate() first";
            LOG_ERROR(error_msg);
            throw std::logic_error(error_msg);
        }

        if (size == 0) {
            std::string error_msg = "cannot allocate a zero-sized buffer";
            LOG_ERROR(error_msg);
            throw std::invalid_argument(error_msg);
        }

        using wire_size_t = decltype(std::declval<cmem_cma_alloc_req>().size);
        if (size > std::numeric_limits<wire_size_t>::max()) {
            std::string error_msg = std::format("Size {} exceeds the maximum single allocation of {} bytes",
                                                size,
                                                std::numeric_limits<wire_size_t>::max());
            LOG_ERROR(error_msg);
            throw std::length_error(error_msg);
        }

        struct cmem_cma_alloc_req alloc_req{};
        alloc_req.size = static_cast<wire_size_t>(size);
        alloc_req.numa_node = numa_node;

        if (ioctl(m_fd, CMEM_CMA_ALLOC, &alloc_req) < 0) {
            const int err = errno;
            LOG_ERROR(std::format("Error from ioctl CMEM_CMA_ALLOC: {}", strerror(err)));
            throw std::domain_error(std::format("allocate: ioctl CMEM_CMA_ALLOC failed: {}", strerror(err)));
        }

        if (!alloc_req.dma_addr) {
            LOG_ERROR("Kernel returned a zero DMA address for a successful allocation");
            struct cmem_cma_free_req free_req{};
            free_req.buffer_id = static_cast<int32_t>(alloc_req.buffer_id);
            if (ioctl(m_fd, CMEM_CMA_FREE, &free_req) < 0) {
                LOG_ERROR(std::format("Failed to roll back orphaned buffer {} after invalid DMA address: {}",
                                      alloc_req.buffer_id,
                                      strerror(errno)));
            }
            throw std::domain_error("allocate: kernel returned a zero DMA address");
        }

        LOG_TRACE("Calling mmap. size=", std::hex, alloc_req.size, " offset=", alloc_req.mmap_offset);

        void* mapped_addr = ::mmap(
          nullptr, alloc_req.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, static_cast<off_t>(alloc_req.mmap_offset));

        if (mapped_addr == MAP_FAILED) {
            const int err = errno;
            LOG_ERROR(std::format("Error from mmap: {}", strerror(err)));

            struct cmem_cma_free_req free_req{};
            free_req.buffer_id = static_cast<int32_t>(alloc_req.buffer_id);
            if (ioctl(m_fd, CMEM_CMA_FREE, &free_req) < 0) {
                LOG_ERROR(std::format("Failed to roll back orphaned buffer {} after mmap failure: {}",
                                      alloc_req.buffer_id,
                                      strerror(errno)));
            }

            throw std::domain_error(std::format("allocate: mmap failed: {}", strerror(err)));
        }

        m_virt_addr = mapped_addr;
        m_size = alloc_req.size;
        m_offset = alloc_req.mmap_offset;
        m_phys_address = alloc_req.dma_addr;
        m_numa_node = alloc_req.numa_node;
        m_buffer_id = alloc_req.buffer_id;

        LOG_TRACE(std::format("Virtual address = {:#x}", reinterpret_cast<std::uintptr_t>(m_virt_addr)));
        LOG_INFO(std::format("Allocation done. Buffer ID: {}", m_buffer_id));
    }

    void CmemCmaBuffer::deallocate()
    {
        LOG_DEBUG("Calling deallocate");

        if (m_virt_addr == nullptr && m_buffer_id < 0) {
            return;
        }

        if (m_virt_addr != nullptr) {
            if (munmap(m_virt_addr, m_size) != 0) {
                const int err = errno;
                LOG_ERROR(std::format("munmap failed for buffer {}: {}", m_buffer_id, strerror(err)));
            }
            m_virt_addr = nullptr;
            m_size = 0;
        }

        if (m_buffer_id >= 0) {
            struct cmem_cma_free_req free_req{};
            free_req.buffer_id = m_buffer_id;

            if (ioctl(m_fd, CMEM_CMA_FREE, &free_req) < 0) {
                const int err = errno;
                LOG_ERROR(std::format("Error from ioctl CMEM_CMA_FREE: {}", strerror(err)));
                throw std::domain_error(std::format("deallocate: ioctl CMEM_CMA_FREE failed: {}", strerror(err)));
            }
            m_buffer_id = -1;
        }

        m_offset = 0;
        m_phys_address = 0;
        m_numa_node = -1;
    }

    CmemCmaBuffer::~CmemCmaBuffer()
    {
        try {
            deallocate();
        } catch (const std::exception& e) {
            LOG_ERROR(std::format("Exception while releasing buffer {} in destructor: {}", m_buffer_id, e.what()));
        } catch (...) {
            LOG_ERROR("Unknown exception while releasing buffer in destructor");
        }

        if (m_fd >= 0) {
            close(m_fd);
            m_fd = -1;
        }
    }
};  // namespace cmem
