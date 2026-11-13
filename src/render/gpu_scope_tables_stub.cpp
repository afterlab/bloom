// CPU-unavailable stub for the GPU scope measure (#724). It defines the same public symbols as the
// Vulkan implementation so a build without Vulkan headers/VMA links unchanged; every begin() fails
// closed with a typed DeviceUnavailable diagnostic and the caller measures with the CPU reference.

#include <bloom/render/gpu_scope_tables.hpp>

#include <memory>

namespace bloom::render {

struct GpuScopeTables::Impl final {
    GpuScopeTablesState state = GpuScopeTablesState::Idle;
    GpuScopeTablesDiagnostic diagnostic;
};

GpuScopeTables::GpuScopeTables() : impl_(std::make_unique<Impl>()) {}

GpuScopeTables::~GpuScopeTables() = default;

GpuScopeTablesState GpuScopeTables::state() const noexcept {
    return impl_ == nullptr ? GpuScopeTablesState::Idle : impl_->state;
}

const GpuScopeTablesDiagnostic& GpuScopeTables::diagnostic() const noexcept {
    static const GpuScopeTablesDiagnostic empty{};
    return impl_ == nullptr ? empty : impl_->diagnostic;
}

bool GpuScopeTables::begin(const GpuScopeTablesRequest&, const std::uint64_t) noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    impl_->state = GpuScopeTablesState::Failure;
    try {
        impl_->diagnostic = {GpuScopeTablesCode::DeviceUnavailable,
                             "the scope measure was built without Vulkan"};
    } catch (...) {
        impl_->diagnostic.code = GpuScopeTablesCode::DeviceUnavailable;
    }
    return false;
}

GpuScopeTablesState GpuScopeTables::poll() noexcept {
    return impl_ == nullptr ? GpuScopeTablesState::Idle : impl_->state;
}

GpuScopeTablesResult GpuScopeTables::take() noexcept { return {}; }

void GpuScopeTables::cancel() noexcept {}

bool GpuScopeTables::hasUnretiredSubmission() noexcept { return false; }

std::uint64_t GpuScopeTables::nativeDispatchCount() noexcept { return 0; }

} // namespace bloom::render
