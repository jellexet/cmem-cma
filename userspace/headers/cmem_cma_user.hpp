#include "cmem-cma.h"
#include <cstddef>
#include <memory>

namespace cmem {
    template<typename T>
    class CmemBufferAllocator
    {
        using value_type = T;

      public:
        T* allocate() {}

        void deallocate(T* p, std::size_t) noexcept {}
        explicit CmemBuffer(size_t size);
        ~CmemBuffer();
        CmemBuffer(CmemBuffer& other) = delete;
        CmemBuffer(CmemBuffer&& other) = delete;

      private:
        int buffer_id;
    };
}  // namespace cmem
