#pragma once

#include <cstdint>
#include <memory>
#include <optional>

namespace bloom::project {

// One Open, Save or Save Copy, and every concurrent one together
// (docs/architecture/project-format.md, "Resource Limits"). A keyframe-dense project charges about
// 300 bytes per JSON value at the peak of its save (1.3 GB for the 4.48-million-value field project
// of issue #737), and an open project keeps its decoded charge in the shared pool while another
// save or a recovery write runs.
inline constexpr std::uint64_t kDefaultProjectIoOperationResidentBytes = 2ULL << 30U;
inline constexpr std::uint64_t kDefaultProjectIoSharedResidentBytes = 4ULL << 30U;
inline constexpr std::uint64_t kDefaultProjectIoSingleAllocationBytes = 256ULL << 20U;

enum class ProjectIoMemoryError : std::uint8_t {
    None,
    InvalidLimit,
    SingleAllocationLimitExceeded,
    OperationLimitExceeded,
    SharedLimitExceeded,
};

struct ProjectIoMemorySnapshot final {
    std::uint64_t currentBytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint64_t limitBytes = 0;

    friend bool operator==(const ProjectIoMemorySnapshot&,
                           const ProjectIoMemorySnapshot&) = default;
};

struct ProjectIoOperationMemorySnapshot final {
    std::uint64_t currentBytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint64_t limitBytes = 0;
    std::uint64_t singleAllocationLimitBytes = 0;

    friend bool operator==(const ProjectIoOperationMemorySnapshot&,
                           const ProjectIoOperationMemorySnapshot&) = default;
};

namespace detail {
struct ProjectIoSharedMemoryState;
struct ProjectIoOperationMemoryState;
} // namespace detail

class ProjectIoMemoryCoordinator;
class ProjectIoOperationMemory;

class ProjectIoMemoryReservation final {
  public:
    ProjectIoMemoryReservation(const ProjectIoMemoryReservation&) = delete;
    ProjectIoMemoryReservation& operator=(const ProjectIoMemoryReservation&) = delete;
    ProjectIoMemoryReservation(ProjectIoMemoryReservation&& other) noexcept;
    ProjectIoMemoryReservation& operator=(ProjectIoMemoryReservation&& other) noexcept;
    ~ProjectIoMemoryReservation();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    void reset() noexcept;

  private:
    friend class ProjectIoOperationMemory;
    friend class ProjectIoMemoryReservationResult;

    ProjectIoMemoryReservation() noexcept = default;
    ProjectIoMemoryReservation(std::shared_ptr<detail::ProjectIoOperationMemoryState> operation,
                               std::uint64_t bytes) noexcept;

    std::shared_ptr<detail::ProjectIoOperationMemoryState> operation_;
    std::uint64_t bytes_ = 0;
};

class [[nodiscard]] ProjectIoMemoryReservationResult final {
  public:
    [[nodiscard]] explicit operator bool() const noexcept {
        return error_ == ProjectIoMemoryError::None;
    }
    [[nodiscard]] ProjectIoMemoryError error() const noexcept { return error_; }
    [[nodiscard]] ProjectIoMemoryReservation takeReservation() && noexcept;

  private:
    friend class ProjectIoOperationMemory;

    explicit ProjectIoMemoryReservationResult(ProjectIoMemoryError error) noexcept;
    explicit ProjectIoMemoryReservationResult(ProjectIoMemoryReservation reservation) noexcept;

    ProjectIoMemoryError error_ = ProjectIoMemoryError::InvalidLimit;
    ProjectIoMemoryReservation reservation_;
};

class ProjectIoOperationMemory final {
  public:
    // Charges one allocation of `bytes`: refused above the single-allocation limit, then against
    // this operation's and the shared budget.
    [[nodiscard]] ProjectIoMemoryReservationResult reserve(std::uint64_t bytes) const;
    // Charges `bytes` as one aggregate: the conservative worst-case charge for the many allocations
    // a dependency makes outside the operation allocator (docs/architecture/project-format.md,
    // "Resource Limits"). It is checked against this operation's and the shared budget exactly as
    // reserve() is, but not against the single-allocation limit, which bounds one allocation, not
    // the sum of many (issue #737: a large project's decoded representation is charged this way).
    [[nodiscard]] ProjectIoMemoryReservationResult reserveAggregate(std::uint64_t bytes) const;
    [[nodiscard]] ProjectIoOperationMemorySnapshot snapshot() const;

  private:
    friend class ProjectIoMemoryCoordinator;

    explicit ProjectIoOperationMemory(
        std::shared_ptr<detail::ProjectIoOperationMemoryState> state) noexcept;

    std::shared_ptr<detail::ProjectIoOperationMemoryState> state_;
};

class ProjectIoMemoryCoordinator final {
  public:
    [[nodiscard]] static std::optional<ProjectIoMemoryCoordinator>
    create(std::uint64_t limitBytes = kDefaultProjectIoSharedResidentBytes) noexcept;

    [[nodiscard]] std::optional<ProjectIoOperationMemory>
    createOperation(std::uint64_t limitBytes = kDefaultProjectIoOperationResidentBytes,
                    std::uint64_t singleAllocationLimitBytes =
                        kDefaultProjectIoSingleAllocationBytes) const noexcept;

    [[nodiscard]] ProjectIoMemorySnapshot snapshot() const;

  private:
    explicit ProjectIoMemoryCoordinator(
        std::shared_ptr<detail::ProjectIoSharedMemoryState> state) noexcept;

    std::shared_ptr<detail::ProjectIoSharedMemoryState> state_;
};

} // namespace bloom::project
