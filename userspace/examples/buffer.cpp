#include "cmem_cma_buffer.hpp"
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>

using namespace cmem;

void put_data_into_buffer(CmemCmaBuffer& b, uint32_t data, uint32_t n, size_t* end_buf)
{
    const std::size_t bytes_needed = (*end_buf + n) * sizeof(uint32_t);
    if (bytes_needed > b.size()) {
        throw std::out_of_range("put_data_into_buffer: write would exceed buffer size");
    }

    uint32_t* base = static_cast<uint32_t*>(b.data());
    for (uint32_t i = 0; i < n; i++) {
        base[*end_buf] = data;
        *end_buf += 1;
    }
}

int main()
{
    size_t end_buf = 0;

    try {
        CmemCmaBuffer buf{};

        buf.allocate(1024, -1);
        put_data_into_buffer(buf, 0xDEADBEEF, 4, &end_buf);
        std::cout << "wrote " << end_buf << " word(s) into the buffer\n";
    } catch (const std::exception& e) {
        std::cout << e.what() << std::endl;
        return 1;
    }

    return 0;
}
