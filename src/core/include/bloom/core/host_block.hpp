#pragma once

// Backing memory for Bloom's pixel and frame buffers.
//
// A cache that evicts frames has to give their memory back to the operating system, or the
// eviction relieves nothing. On Linux the process allocator does that for every block of 1 MiB
// and more (runtime::configureHostMemoryReturn). The macOS allocator has no such setting: it
// keeps the most recently freed large blocks, up to about a gigabyte of them, charged to the
// process. So on macOS a block of kMappedHostBlockBytes or more is mapped from the system
// directly and unmapped when freed; smaller blocks, and every block on other platforms, come
// from operator new.
//
// The same holds for a block that never calls these functions: on macOS this library replaces
// operator new and operator delete, so a block of kMappedHostBlockBytes or more that any C++
// code in the process allocates (a vector of pixels, a decoded plane, cached samples) is mapped
// and unmapped the same way (host_block_operator_new.cpp; issue #673). Memory a C library takes from malloc
// is not covered.
//
// The pointer is aligned for any scalar type, as operator new's is. A mapped block arrives
// zeroed; no caller may rely on that. Which of the two a block is follows from its size alone, so
// the block carries nothing of its own and is freed with the size it was asked for.

#include <cstddef>

namespace bloom::core {

inline constexpr std::size_t kMappedHostBlockBytes = std::size_t{1024} * 1024;

// Throws std::bad_alloc when the memory cannot be had.
[[nodiscard]] void* allocateHostBlock(std::size_t bytes);
// `block` is a pointer allocateHostBlock(bytes) returned, or null.
void freeHostBlock(void* block, std::size_t bytes) noexcept;

} // namespace bloom::core
