#ifndef CMEM_CMA_ALLOCATOR_HPP
#define CMEM_CMA_ALLOCATOR_HPP

#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
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

        explicit CmemCmaAllocator(AllocatorOptions opt) noexcept;
        ~CmemCmaAllocator() noexcept;

        CmemCmaAllocator& operator=(CmemCmaAllocator&) = delete;
        CmemCmaAllocator(CmemCmaAllocator& other) = delete;

        CmemCmaAllocator& operator=(CmemCmaAllocator&&) noexcept;
        CmemCmaAllocator(CmemCmaAllocator&& other) noexcept;

        template<typename U>
        constexpr CmemCmaAllocator(const CmemCmaAllocator<U>&) noexcept
        {}

        [[nodiscard]] T* allocate(std::size_t n)
        {
            if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
                throw std::bad_array_new_length();

            try {
                m_buffer.allocate(n, m_options.numa_node)

            } catch (const std::logic_error& err) {
                throw std::bad_alloc();
            } catch (const std::length_error& err) {
                throw std::bad_alloc();
            } catch (const std::domain_error& err) {
                throw std::bad_alloc();
            }

            LOG_INFO(std::format("Allocated buffer of size {}", n));
            auto p = static_cast<T*>(m_buffer.data());
            return p;
        }

        void deallocate(T* p, std::size_t n) noexcept
        {
            LOG_INFO(std::format("Deallocated buffer of size {}", n));
            m_buffer.deallocate();
        }

        template<class T, class U>
        bool operator==(const CmemCmaAllocator<T>& a, const CmemCmaAllocator<U>& b)
        {
            return a.buffer_m.get_buffer_id() == b.buffer_m.get_buffer_id();
        }

        template<class T, class U>
        bool operator!=(const CmemCmaAllocator<T>& a, const CmemCmaAllocator<U>& b)
        {
            return a.buffer_m.get_buffer_id() != b.buffer_m.get_buffer_id();
        }

      private:
        AllocatorOptions m_options{};
        CmemCmaBuffer m_buffer{};
    };
};  // namespace cmem

#endif  // CMEM_CMA_ALLOCATOR_HPP
