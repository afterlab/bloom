#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>

namespace bloom::update::detail {

// A file's bytes as read-only memory, without a copy: the signature of an update is defined
// over the whole file at once, and a package is too large to hold a second time. The file must
// not be changed while it is mapped; an update is checked in Bloom's own update folder.
class MappedFile final {
  public:
    [[nodiscard]] static std::optional<MappedFile> open(const std::filesystem::path& path) noexcept;

    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile();

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(address_), size_};
    }

  private:
    MappedFile(void* address, std::size_t size) noexcept : address_(address), size_(size) {}
    void release() noexcept;

    void* address_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace bloom::update::detail
