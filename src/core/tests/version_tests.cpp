#include <bloom/core/version.hpp>

#include <iostream>
#include <source_location>
#include <string>
#include <string_view>

// BLOOM_EXPECTED_PACKAGE_VERSION is the label the build was configured with
// (BLOOM_PACKAGE_VERSION); BLOOM_EXPECTED_BUILD_VERSION is what cmake/BloomVersion.cmake makes of
// it.
#ifndef BLOOM_EXPECTED_PACKAGE_VERSION
#error "BLOOM_EXPECTED_PACKAGE_VERSION must name the configured package version"
#endif
#ifndef BLOOM_EXPECTED_BUILD_VERSION
#error "BLOOM_EXPECTED_BUILD_VERSION must name the configured build version"
#endif

namespace {

class ExpectationContext final {
  public:
    bool expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return true;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
        return false;
    }

    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    int failures_ = 0;
};

} // namespace

int main() {
    ExpectationContext expectations;
    namespace core = bloom::core;

    expectations.expect(core::version() == BLOOM_EXPECTED_PACKAGE_VERSION,
                        "the version is the configured package version");
    expectations.expect(core::buildVersion() == core::buildVersionOf(core::version()),
                        "the build version is the compact form of the version");
    expectations.expect(core::buildVersion() == BLOOM_EXPECTED_BUILD_VERSION,
                        "the build version is what bloom_build_version makes of the label");

    struct Case {
        std::string_view label;
        std::string_view compact;
    };
    static constexpr Case kCases[] = {
        {"0.1.0-alpha.1", "0.1.0a1"},
        {"0.1.0-alpha.12", "0.1.0a12"},
        {"1.20.3-beta.4", "1.20.3b4"},
        {"1.2.3-rc.5", "1.2.3rc5"},
        {"0.1.0", "0.1.0"},
        {"0.1.0-alpha", "0.1.0-alpha"},
        {"0.1.0-alpha.", "0.1.0-alpha."},
        {"0.1.0-alpha.x", "0.1.0-alpha.x"},
        {"0.1.0-alpha.1.2", "0.1.0-alpha.1.2"},
        {"0.1.0-alpha.1+build", "0.1.0-alpha.1+build"},
        {"0.1.0-dev.3", "0.1.0-dev.3"},
        {"0.1.0+build", "0.1.0+build"},
        {"0.1-alpha.1", "0.1-alpha.1"},
        {"", ""},
    };
    for (const Case& item : kCases) {
        const std::string made = core::buildVersionOf(item.label);
        expectations.expect(made == item.compact,
                            "the compact form of '" + std::string(item.label) + "' is '" +
                                std::string(item.compact) + "', not '" + made + "'");
    }
    return expectations.ok() ? 0 : 1;
}
