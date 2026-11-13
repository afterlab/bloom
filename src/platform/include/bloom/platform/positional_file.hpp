#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

// A regular file read and written at stated offsets (#718: the local mirror's byte ranges of one
// large source). Every member is safe to call from several threads at once: no call uses or moves
// a shared file position. Blocking filesystem work: never on the UI thread.
namespace bloom::platform {

class PositionalFile final {
  public:
    // Opens `path` for reading and writing. `create` makes it when it is absent (and its parent
    // folder); a new file is marked sparse where the file system has sparse files, so a write far
    // into it does not make the system write everything before it. Null when it cannot be opened.
    [[nodiscard]] static std::unique_ptr<PositionalFile> open(const std::filesystem::path& path,
                                                              bool create);
    ~PositionalFile();
    PositionalFile(const PositionalFile&) = delete;
    PositionalFile& operator=(const PositionalFile&) = delete;
    // Exactly bytes.size() bytes at `offset`; false when the file is shorter or the read failed.
    [[nodiscard]] bool readAt(std::uint64_t offset, std::span<std::byte> bytes) const;
    // Writes all of `bytes` at `offset`, extending the file when it has to.
    [[nodiscard]] bool writeAt(std::uint64_t offset, std::span<const std::byte> bytes) const;
    [[nodiscard]] std::optional<std::uint64_t> size() const;

  private:
#if defined(_WIN32)
    explicit PositionalFile(void* handle) noexcept : handle_(handle) {}
    void* handle_ = nullptr;
#else
    explicit PositionalFile(int descriptor) noexcept : descriptor_(descriptor) {}
    int descriptor_ = -1;
#endif
};

} // namespace bloom::platform
