#include <bloom/core/host_block.hpp>

#include <cstddef>
#include <new>

#if defined(__APPLE__)
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <sys/mman.h>
#include <unordered_map>
#include <utility>
#endif

// macOS: every large block any C++ code in the process asks operator new for is a host block too
// (issue #673). Bloom's frame-sized memory is not only the pixel buffers that call
// allocateHostBlock themselves: it is also the vectors behind evaluated and display images,
// cached half-float frames, decoded planes, audio and readbacks, in a great many places, and the
// macOS allocator keeps each of those charged to the process after it is freed. Routing the
// allocation function itself covers all of them, and whatever is written next, in one place.
//
// The replacement is a member of the core library of its own, so the linker takes it for every
// program that does not define these functions itself (a test that counts allocations keeps its
// own). A block of kMappedHostBlockBytes or more is mapped and recorded; operator delete, which is
// not told the size, recognizes such a block by its address. Smaller blocks come from malloc as
// before. The aligned forms are left to the standard library, and a build with the address
// sanitizer keeps the sanitizer's own allocation functions.
#if defined(__APPLE__) && !defined(BLOOM_HOST_BLOCK_NO_OPERATOR_NEW)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BLOOM_HOST_BLOCK_NO_OPERATOR_NEW
#endif
#endif
#endif

#if defined(__APPLE__) && !defined(BLOOM_HOST_BLOCK_NO_OPERATOR_NEW)
namespace {

// The record's own nodes come from malloc, never from the operator new it serves.
template <class T> struct MallocAllocator {
    using value_type = T;
    MallocAllocator() = default;
    template <class U> constexpr MallocAllocator(const MallocAllocator<U>&) noexcept {}
    [[nodiscard]] T* allocate(const std::size_t count) {
        if (void* const block = std::malloc(count * sizeof(T)))
            return static_cast<T*>(block);
        throw std::bad_alloc();
    }
    void deallocate(T* const block, std::size_t) noexcept { std::free(block); }
    template <class U> constexpr bool operator==(const MallocAllocator<U>&) const noexcept {
        return true;
    }
};

class MappedBlocks final {
  public:
    // Made on first use and never destroyed: operator delete runs until the process ends.
    static MappedBlocks& instance() noexcept {
        static MappedBlocks* const blocks = [] {
            void* const storage = std::malloc(sizeof(MappedBlocks));
            return storage != nullptr ? ::new (storage) MappedBlocks : nullptr;
        }();
        return *blocks;
    }
    [[nodiscard]] bool record(void* const block, const std::size_t bytes) noexcept {
        try {
            const std::lock_guard lock(mutex_);
            blocks_.emplace(block, bytes);
            return true;
        } catch (...) {
            return false;
        }
    }
    // The size of a recorded block, which is then forgotten; zero for any other address.
    [[nodiscard]] std::size_t take(void* const block) noexcept {
        const std::lock_guard lock(mutex_);
        const auto found = blocks_.find(block);
        if (found == blocks_.end())
            return 0;
        const auto bytes = found->second;
        blocks_.erase(found);
        return bytes;
    }

  private:
    std::mutex mutex_;
    std::unordered_map<void*, std::size_t, std::hash<void*>, std::equal_to<void*>,
                       MallocAllocator<std::pair<void* const, std::size_t>>>
        blocks_;
};

// A mapping starts on a page boundary, and no page is smaller than this.
constexpr std::uintptr_t kSmallestPageMask = 4096 - 1;

} // namespace

// NOLINTBEGIN(misc-new-delete-overloads,cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
void* operator new(const std::size_t bytes) {
    if (bytes >= bloom::core::kMappedHostBlockBytes) {
        void* const block =
            ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (block != MAP_FAILED) {
            if (MappedBlocks::instance().record(block, bytes))
                return block;
            static_cast<void>(::munmap(block, bytes));
        }
        throw std::bad_alloc();
    }
    for (;;) {
        if (void* const block = std::malloc(bytes != 0 ? bytes : 1))
            return block;
        const auto handler = std::get_new_handler();
        if (handler == nullptr)
            throw std::bad_alloc();
        handler();
    }
}

void operator delete(void* const block) noexcept {
    if (block == nullptr)
        return;
    if ((reinterpret_cast<std::uintptr_t>(block) & kSmallestPageMask) == 0) {
        if (const auto bytes = MappedBlocks::instance().take(block); bytes != 0) {
            static_cast<void>(::munmap(block, bytes));
            return;
        }
    }
    std::free(block);
}

void operator delete(void* const block, std::size_t) noexcept { ::operator delete(block); }
// NOLINTEND(misc-new-delete-overloads,cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
#endif
