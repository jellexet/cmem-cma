#ifndef CMEM_CMA_ALLOCATOR_HPP
#define CMEM_CMA_ALLOCATOR_HPP

#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "cmem_cma_buffer.hpp"
#include "simple_logger.hpp"

namespace cmem {

    struct AllocatorOptions {
        int numa_node = -1;
    };

    template<typename T>
    class CmemCmaAllocator
    {
      public:
        using value_type = T;
        using size_type = std::size_t;
        using propagate_on_container_move_assignment = std::false_type;
        using propagate_on_container_copy_assignment = std::false_type;
        using propagate_on_container_swap = std::false_type;
        using is_always_equal = std::false_type;

        explicit CmemCmaAllocator(AllocatorOptions opt) noexcept : m_options(opt) {}
        ~CmemCmaAllocator() noexcept = default;

        CmemCmaAllocator& operator=(const CmemCmaAllocator&) noexcept = default;
        CmemCmaAllocator(const CmemCmaAllocator&) noexcept = default;
        CmemCmaAllocator& operator=(CmemCmaAllocator&&) noexcept = default;
        CmemCmaAllocator(CmemCmaAllocator&&) noexcept = default;

        template<typename U>
        constexpr CmemCmaAllocator(const CmemCmaAllocator<U>& other) noexcept : m_options(other.options())
        {}

        [[nodiscard]] const AllocatorOptions& options() const noexcept { return m_options; }

        [[nodiscard]] T* allocate(std::size_t n)
        {
            if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
                throw std::bad_array_new_length();

            const std::size_t bytes = n * sizeof(T);

            std::unique_ptr<CmemCmaBuffer> buf;
            try {
                buf = std::make_unique<CmemCmaBuffer>();
                buf->allocate(bytes, m_options.numa_node);
            } catch (const std::exception&) {
                throw std::bad_alloc();
            }

            void* p = buf->data();

            {
                std::lock_guard<std::mutex> lock(registry_mutex());
                registry().emplace(p, std::move(buf));
            }

            LOG_INFO(std::format("Allocated buffer of {} element(s), {} byte(s), at {}", n, bytes, p));
            return static_cast<T*>(p);
        }

        void deallocate(T* p, std::size_t n) noexcept
        {
            std::unique_ptr<CmemCmaBuffer> buf;
            {
                std::lock_guard<std::mutex> lock(registry_mutex());
                auto it = registry().find(static_cast<void*>(p));
                if (it == registry().end()) {
                    LOG_ERROR(std::format("deallocate() called with a pointer this allocator did not hand out: {}",
                                          static_cast<void*>(p)));
                    return;
                }
                buf = std::move(it->second);
                registry().erase(it);
            }

            LOG_INFO(std::format("Deallocating buffer of {} element(s), {} byte(s)", n, n * sizeof(T)));
        }

      private:
        AllocatorOptions m_options{};

        static std::mutex& registry_mutex()
        {
            static std::mutex m;
            return m;
        }

        static std::unordered_map<void*, std::unique_ptr<CmemCmaBuffer>>& registry()
        {
            static std::unordered_map<void*, std::unique_ptr<CmemCmaBuffer>> r;
            return r;
        }

        template<typename U>
        friend class CmemCmaAllocator;
    };

    template<typename T, typename U>
    bool operator==(const CmemCmaAllocator<T>& a, const CmemCmaAllocator<U>& b) noexcept
    {
        return a.options().numa_node == b.options().numa_node;
    }

    template<typename T, typename U>
    bool operator!=(const CmemCmaAllocator<T>& a, const CmemCmaAllocator<U>& b) noexcept
    {
        return !(a == b);
    }

};  // namespace cmem

#endif  // CMEM_CMA_ALLOCATOR_HPP
