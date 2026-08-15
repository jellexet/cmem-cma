#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <vector>

namespace cmem {

    template<typename T>
    class CmemCmaAllocator
    {
        using value_type = T;
        using size_type = std::size_t;
        using propagate_on_container_move_assignment = std::true_type;
        using is_always_equal = std::true_type;

        CmemCmaAllocator() = default;

        template<typename U>
        constexpr CmemCmaAllocator(const CmemCmaAllocator<U>&) noexcept
        {}

        [[nodiscard]] T* allocate(std::size_t n)
        {
            if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
                throw std::bad_array_new_length();

            if (auto p = static_cast<T*>(std::malloc(n * sizeof(T)))) {
                report(p, n);
                return p;
            }

            throw std::bad_alloc();
        }

        void deallocate(T* p, std::size_t n) noexcept
        {
            report(p, n, 0);
            std::free(p);
        }

      private:
        void report(T* p, std::size_t n, bool alloc = true) const
        {
            std::cout << (alloc ? "Alloc: " : "Dealloc: ") << sizeof(T) * n << " bytes at " << std::hex << std::showbase
                      << reinterpret_cast<void*>(p) << std::dec << '\n';
        }
    };

    template<class T, class U>
    bool operator==(const CmemCmaAllocator<T>&, const CmemCmaAllocator<U>&)
    {
        return true;
    }

    template<class T, class U>
    bool operator!=(const CmemCmaAllocator<T>&, const CmemCmaAllocator<U>&)
    {
        return false;
    }
};  // namespace cmem
