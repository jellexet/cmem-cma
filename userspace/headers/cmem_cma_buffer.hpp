#ifndef CMEM_CMA_BUFFER_HPP
#define CMEM_CMA_BUFFER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace cmem {
    class CmemCmaBuffer
    {
      public:
        CmemCmaBuffer(const std::filesystem::path& device_path = "/dev/cmem_cma");
        ~CmemCmaBuffer();

        CmemCmaBuffer& operator=(CmemCmaBuffer&) = delete;
        CmemCmaBuffer(CmemCmaBuffer& other) = delete;
        CmemCmaBuffer& operator=(CmemCmaBuffer&&) = delete;

        CmemCmaBuffer(CmemCmaBuffer&& other) noexcept;
        void* data() noexcept { return m_virt_addr; }

        void allocate(uint64_t size, int numa_node);
        void deallocate();

        [[nodiscard]] const void* data() const noexcept { return m_virt_addr; }
        [[nodiscard]] std::size_t size() const noexcept { return m_size; }
        [[nodiscard]] std::uint64_t get_dma_address() const noexcept { return m_phys_address; }
        [[nodiscard]] int get_numa_node() const noexcept { return m_numa_node; }
        [[nodiscard]] std::uint32_t get_buffer_id() const noexcept { return m_buffer_id; }

        explicit operator bool() const noexcept { return m_virt_addr != nullptr; }

      private:
        int m_fd{-1};  ///< The file descriptor for the /dev/cmem_rcc device.
        int m_buffer_id{-1};
        int m_numa_node{-1};
        uint64_t m_offset{0};
        uint64_t m_size{0};
        uint64_t m_phys_address{0};
        void* m_virt_addr{nullptr};
        std::atomic<bool> m_is_open{false};  ///< Flag: true if a buffer has been created.
    };
}  // namespace cmem

#endif  // CMEM_CMA_BUFFER_HPP
