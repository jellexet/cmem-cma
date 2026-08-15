#ifndef CMEM_CMA_BUFFER_HPP
#define CMEM_CMA_BUFFER_HPP

#include "cmem_cma.hpp"
#include <cstddef>
#include <cstdint>
#include <cstddef>
#include <memory>

namespace cmem {
    class CmemCmaBuffer
    {
      public:
        CmemCmaBuffer() = default;
        ~CmemCmaBuffer();

        CmemCmaBuffer& operator=(CmemCmaBuffer&) = delete;
        CmemCmaBuffer(CmemCmaBuffer& other) = delete;

        CmemCmaBuffer& operator=(CmemCmaBuffer&&) noexcept;
        CmemCmaBuffer(CmemCmaBuffer&& other) noexcept;
        void* data() noexcept { return addr_m; }

        [[nodiscard]] const uint64_t allocate(uint64_t size, int numa_node) noexcept;
        void deallocate() noexcept;

        [[nodiscard]] const void* data() const noexcept { return addr_m; }
        [[nodiscard]] std::size_t size() const noexcept { return size_m; }
        [[nodiscard]] std::uint64_t get_dma_address() const noexcept { return memory_address_m; }
        [[nodiscard]] int get_numa_node() const noexcept { return numa_node_m; }
        [[nodiscard]] std::uint32_t get_buffer_id() const noexcept { return buffer_id_m; }

        explicit operator bool() const noexcept { return addr_m != nullptr; }

      private:
        int fd_m = -1;
        int buffer_id_m = -1;
        int numa_node_m = -1;
        uint64_t size_m = 0;
        uint64_t memory_address_m = 0;
        void* addr_m = nullptr;
    };
}  // namespace cmem

#endif  // CMEM_CMA_BUFFER_HPP
