#include <bloom/core/host_block.hpp>

#include <new>

#if defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace bloom::core {

void* allocateHostBlock(const std::size_t bytes) {
#if defined(__APPLE__)
    if (bytes >= kMappedHostBlockBytes) {
        void* const block =
            ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (block == MAP_FAILED)
            throw std::bad_alloc();
        return block;
    }
#endif
    return ::operator new(bytes);
}

void freeHostBlock(void* const block, [[maybe_unused]] const std::size_t bytes) noexcept {
    if (!block)
        return;
#if defined(__APPLE__)
    if (bytes >= kMappedHostBlockBytes) {
        static_cast<void>(::munmap(block, bytes));
        return;
    }
#endif
    ::operator delete(block);
}

} // namespace bloom::core
