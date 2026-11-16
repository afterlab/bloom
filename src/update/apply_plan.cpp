#include <bloom/update/apply.hpp>

#include <charconv>
#include <fstream>
#include <iterator>

namespace bloom::update {
namespace {

[[nodiscard]] std::string text(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {encoded.begin(), encoded.end()};
}

[[nodiscard]] std::filesystem::path pathOf(const std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

[[nodiscard]] bool singleLine(const std::string_view value) noexcept {
    return value.find_first_of("\r\n") == std::string_view::npos;
}

template <typename Number>
[[nodiscard]] std::optional<Number> numberOf(const std::string_view value) noexcept {
    Number number{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return number;
}

[[nodiscard]] std::optional<std::string> readText(const std::filesystem::path& path,
                                                  const std::size_t limit) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string content;
    char character = 0;
    while (content.size() <= limit && file.get(character)) {
        content.push_back(character);
    }
    if (content.size() > limit) {
        return std::nullopt;
    }
    return content;
}

[[nodiscard]] std::string trimmedLine(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
        value.pop_back();
    }
    return value;
}

// Writes a small file whole: under another name first, then renamed over the real one.
[[nodiscard]] bool writeWhole(const std::filesystem::path& path, const std::string_view content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto partial = path;
    partial += ".partial";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        if (!file) {
            return false;
        }
    }
    std::filesystem::rename(partial, path, error);
    return !error;
}

} // namespace

bool writeApplyPlan(const std::filesystem::path& path, const ApplyPlan& plan) {
    std::string content;
    const auto line = [&content](const std::string_view name, const std::string_view value) {
        content += name;
        content += '=';
        content += value;
        content += '\n';
        return singleLine(value);
    };
    bool valid = line("mode", plan.mode == ApplyMode::Swap ? "swap" : "watch");
    valid = line("process", std::to_string(plan.processId)) && valid;
    valid = line("installed", text(plan.installed)) && valid;
    valid = line("staged", text(plan.staged)) && valid;
    valid = line("previous", text(plan.previous)) && valid;
    valid = line("program", text(plan.program)) && valid;
    for (const auto& argument : plan.arguments) {
        valid = line("argument", argument) && valid;
    }
    valid = line("confirmation", text(plan.confirmation)) && valid;
    valid = line("version", plan.version) && valid;
    valid = line("notice", text(plan.notice)) && valid;
    valid = line("exit-seconds", std::to_string(plan.exitTimeout.count())) && valid;
    valid = line("confirm-seconds", std::to_string(plan.confirmTimeout.count())) && valid;
    return valid && writeWhole(path, content);
}

std::optional<ApplyPlan> readApplyPlan(const std::filesystem::path& path) {
    const auto content = readText(path, 1024 * 1024);
    if (!content.has_value()) {
        return std::nullopt;
    }
    ApplyPlan plan;
    bool hasMode = false;
    std::string_view rest = *content;
    while (!rest.empty()) {
        const auto end = rest.find('\n');
        auto entry = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (!entry.empty() && entry.back() == '\r') {
            entry.remove_suffix(1);
        }
        const auto equals = entry.find('=');
        if (entry.empty() || equals == std::string_view::npos) {
            continue;
        }
        const auto name = entry.substr(0, equals);
        const auto value = entry.substr(equals + 1);
        if (name == "mode") {
            if (value != "swap" && value != "watch") {
                return std::nullopt;
            }
            plan.mode = value == "swap" ? ApplyMode::Swap : ApplyMode::Watch;
            hasMode = true;
        } else if (name == "process") {
            const auto number = numberOf<std::uint64_t>(value);
            if (!number.has_value()) {
                return std::nullopt;
            }
            plan.processId = *number;
        } else if (name == "installed") {
            plan.installed = pathOf(value);
        } else if (name == "staged") {
            plan.staged = pathOf(value);
        } else if (name == "previous") {
            plan.previous = pathOf(value);
        } else if (name == "program") {
            plan.program = pathOf(value);
        } else if (name == "argument") {
            plan.arguments.emplace_back(value);
        } else if (name == "confirmation") {
            plan.confirmation = pathOf(value);
        } else if (name == "version") {
            plan.version = std::string(value);
        } else if (name == "notice") {
            plan.notice = pathOf(value);
        } else if (name == "exit-seconds" || name == "confirm-seconds") {
            const auto number = numberOf<std::int64_t>(value);
            if (!number.has_value() || *number < 0 || *number > 24 * 60 * 60) {
                return std::nullopt;
            }
            (name == "exit-seconds" ? plan.exitTimeout : plan.confirmTimeout) =
                std::chrono::seconds(*number);
        }
    }
    // Every path the steps touch is absolute, so nothing depends on where the helper was started.
    const bool pathsAbsolute =
        plan.installed.is_absolute() && plan.previous.is_absolute() &&
        plan.program.is_absolute() && plan.confirmation.is_absolute() &&
        plan.notice.is_absolute() && (plan.mode == ApplyMode::Watch || plan.staged.is_absolute());
    if (!hasMode || !pathsAbsolute || plan.version.empty() || plan.installed == plan.previous ||
        (plan.mode == ApplyMode::Swap &&
         (plan.staged == plan.installed || plan.staged == plan.previous))) {
        return std::nullopt;
    }
    return plan;
}

bool writeStartConfirmation(const std::filesystem::path& path, const std::string_view version) {
    return singleLine(version) && writeWhole(path, std::string(version) + "\n");
}

std::optional<std::string> readStartConfirmation(const std::filesystem::path& path) {
    auto content = readText(path, 256);
    if (!content.has_value()) {
        return std::nullopt;
    }
    return trimmedLine(std::move(*content));
}

std::optional<std::string> takeRestoreNotice(const std::filesystem::path& path) {
    auto content = readText(path, 256);
    if (!content.has_value()) {
        return std::nullopt;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    return trimmedLine(std::move(*content));
}

} // namespace bloom::update
