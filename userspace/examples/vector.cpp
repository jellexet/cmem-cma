// std::vector with CmemCmaAllocator<T> as custom allocator.

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numeric>
#include <vector>

#include <cmem_cma_allocator.hpp>

using namespace cmem;

int main()
{
    try {
        AllocatorOptions opt{};

        CmemCmaAllocator<std::uint32_t> alloc(opt);

        std::vector<std::uint32_t, CmemCmaAllocator<std::uint32_t>> values(alloc);

        values.resize(1024);
        std::iota(values.begin(), values.end(), 0u);

        const std::uint32_t sum = std::accumulate(values.begin(), values.end(), 0u);
        std::cout << "sum of 0..1023 = " << sum << '\n';

        std::sort(values.begin(), values.end(), std::greater<>{});
        std::cout << "largest after sort: " << values.front() << '\n';

    } catch (const std::exception& e) {
        std::cerr << "cmem error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
