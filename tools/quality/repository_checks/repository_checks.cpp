#include "repository_checks.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <ranges>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace bloom::quality {
namespace {

using Path = std::filesystem::path;

constexpr auto kMaximumScannedTextBytes = std::uintmax_t{2} * 1024U * 1024U;

constexpr auto kIgnoredDirectories = std::to_array<std::string_view>(
    {".git", ".cache", ".idea", ".pytest_cache", ".vs", ".vscode", "__pycache__", "build"});
constexpr auto kGeneratedPathParts = std::to_array<std::string_view>(
    {".cache", ".idea", ".pytest_cache", ".vs", ".vscode", "CMakeFiles", "__pycache__", "build"});
constexpr auto kGeneratedFilenames =
    std::to_array<std::string_view>({".DS_Store", "CMakeCache.txt", "Desktop.ini", "Thumbs.db",
                                     "cmake_install.cmake", "compile_commands.json"});
constexpr auto kGeneratedSuffixes = std::to_array<std::string_view>(
    {".a", ".autosave", ".class", ".dll", ".dylib", ".exe", ".ilk", ".lib", ".log", ".o", ".obj",
     ".pdb", ".pyc", ".pyo", ".so", ".suo", ".swp", ".user"});
constexpr auto kSensitiveFilenames =
    std::to_array<std::string_view>({".env", "credentials", "credentials.json", "id_dsa",
                                     "id_ecdsa", "id_ed25519", "id_rsa", ".netrc"});
constexpr auto kSensitiveSuffixes =
    std::to_array<std::string_view>({".key", ".p12", ".pfx", ".pem"});
constexpr auto kTextSuffixes = std::to_array<std::string_view>(
    {".bat", ".c",   ".cc",   ".cfg", ".cmake", ".conf", ".cpp",  ".css", ".cxx", ".h",
     ".hh",  ".hpp", ".hxx",  ".ini", ".inl",   ".json", ".md",   ".ps1", ".py",  ".qrc",
     ".rst", ".sh",  ".toml", ".txt", ".ui",    ".xml",  ".yaml", ".yml"});
constexpr auto kTextFilenames =
    std::to_array<std::string_view>({".clang-format", ".editorconfig", ".gitignore", "AGENTS.md",
                                     "CMakeLists.txt", "LICENSE", "NOTICE"});
constexpr auto kVendoredDirectoryNames =
    std::to_array<std::string_view>({"external", "third-party", "third_party", "vendor"});
constexpr auto kVendoredLicenseFilenames = std::to_array<std::string_view>(
    {"COPYING", "COPYING.md", "COPYING.txt", "LICENSE", "LICENSE.md", "LICENSE.txt", "NOTICE",
     "NOTICE.md", "NOTICE.txt"});
constexpr auto kContentScanExclusions = std::to_array<std::string_view>(
    {"tools/quality/repository_checks/repository_checks.cpp",
     "tools/quality/repository_checks/tests/repository_checks_tests.cpp"});
constexpr auto kPlaceholderUsers = std::to_array<std::string_view>(
    {"$user", "${user}", "%username%", "<user>", "example", "name", "user", "username"});
constexpr auto kPlaceholderSecrets = std::to_array<std::string_view>(
    {"<secret>", "<token>", "changeme", "dummy", "example", "placeholder", "redacted", "test",
     "your-secret", "your-token"});
constexpr auto kCppSuffixes = std::to_array<std::string_view>(
    {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"});
constexpr auto kPythonSourceRoots = std::to_array<std::string_view>(
    {"examples/scripting", "src/scripting/python", "tests/fixtures/scripting"});
// Manual qualification orchestration only; no application/build code generation. The owning
// scripting architecture document names this bounded exception, not a general tools/ Python root.
constexpr auto kPythonQualificationTools = std::to_array<std::string_view>(
    {"tools/ci/configure_gpu_qualification.py",
     // The Google Fonts index generator (docs/architecture/fonts.md): run by hand before a release.
     "tools/fonts/generate_google_catalogue.py", "tools/ci/gpu_qualification.py",
     "tools/quality/qualify_private_share_outage.py",
     // The documented local qualification runner and its own test (local-validation.md).
     "tools/local/sprint_validation.py", "tools/local/test_sprint_validation.py",
     // The documented performance benchmark generators and runner, named file by file
     // (benchmarks.md); tools/bench is not a general Python root.
     "tools/bench/bench_media.py", "tools/bench/bench_spec.py", "tools/bench/blender_scene.py",
     "tools/bench/build_projects.py", "tools/bench/chart.py", "tools/bench/chart_check.py",
     "tools/bench/imageio_min.py", "tools/bench/node_cost.py", "tools/bench/run_bench.py",
     "tools/bench/run_scenarios.py", "tools/bench/scenario_spec.py",
     "tools/bench/test_run_scenarios.py"});

template <std::size_t Size>
[[nodiscard]] constexpr auto contains(const std::array<std::string_view, Size>& values,
                                      const std::string_view candidate) -> bool {
    return std::ranges::find(values, candidate) != values.end();
}

[[nodiscard]] auto asciiLower(std::string value) -> std::string {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] auto normalizedRelative(const Path& path) -> std::string {
    return path.lexically_normal().generic_string();
}

[[nodiscard]] auto pathParts(const Path& path) -> std::vector<std::string> {
    std::vector<std::string> result;
    for (const auto& part : path) {
        result.push_back(part.generic_string());
    }
    return result;
}

template <std::size_t Size>
[[nodiscard]] auto hasAnyPart(const Path& path, const std::array<std::string_view, Size>& names)
    -> bool {
    return std::ranges::any_of(
        path, [&names](const auto& part) { return contains(names, part.generic_string()); });
}

[[nodiscard]] auto isWithin(const Path& path, const Path& root) -> bool {
    const auto mismatch = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
    return mismatch.first == root.end();
}

[[nodiscard]] auto readText(const Path& path, std::string& text) -> bool {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});
    return stream.good() || stream.eof();
}

[[nodiscard]] auto isPlaceholderUser(const std::string_view user) -> bool {
    return contains(kPlaceholderUsers, asciiLower(std::string{user}));
}

[[nodiscard]] auto isPlaceholderSecret(std::string value) -> bool {
    while (!value.empty() && (value.front() == '\'' || value.front() == '"')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '\'' || value.back() == '"')) {
        value.pop_back();
    }
    value = asciiLower(std::move(value));

    const auto startsWith = [&value](const std::string_view prefix) {
        return value.starts_with(prefix);
    };
    const auto allMaskCharacters = std::ranges::all_of(
        value, [](const char character) { return character == 'x' || character == '*'; });
    return contains(kPlaceholderSecrets, value) || startsWith("${") || startsWith("$env{") ||
           (value.starts_with('<') && value.ends_with('>')) || startsWith("your_") ||
           startsWith("your-") || allMaskCharacters;
}

[[nodiscard]] auto isTextFile(const Path& relative) -> bool {
    return contains(kTextFilenames, relative.filename().generic_string()) ||
           contains(kTextSuffixes, asciiLower(relative.extension().generic_string()));
}

[[nodiscard]] auto isAllowedPythonSource(const Path& relative) -> bool {
    const auto normalized = normalizedRelative(relative);
    return contains(kPythonQualificationTools, normalized) ||
           std::ranges::any_of(kPythonSourceRoots, [&normalized](const std::string_view root) {
               return normalized.starts_with(std::string{root} + '/');
           });
}

void appendPathFindings(const Path& root, const Path& relative,
                        std::vector<RepositoryFinding>& findings) {
    const auto filename = relative.filename().generic_string();
    const auto suffix = asciiLower(relative.extension().generic_string());
    if (hasAnyPart(relative, kGeneratedPathParts)) {
        findings.push_back({relative, "generated artifact",
                            "generated/build directory must not be tracked", std::nullopt});
    }
    if (contains(kGeneratedFilenames, filename) || contains(kGeneratedSuffixes, suffix)) {
        findings.push_back({relative, "generated artifact",
                            "generated/build output must not be tracked", std::nullopt});
    }
    if (contains(kSensitiveFilenames, filename) || contains(kSensitiveSuffixes, suffix)) {
        findings.push_back({relative, "sensitive file",
                            "credential or private-key file must not be published", std::nullopt});
    }
    if ((suffix == ".py" || suffix == ".pyi") && !isAllowedPythonSource(relative)) {
        findings.push_back(
            {relative, "python placement",
             "Python sources are restricted to scripting roots and named qualification tools",
             std::nullopt});
    }

    std::error_code error;
    const auto absolute = root / relative;
    if (!std::filesystem::is_symlink(std::filesystem::symlink_status(absolute, error)) || error) {
        return;
    }

    const auto target = std::filesystem::read_symlink(absolute, error);
    if (error) {
        findings.push_back({relative, "read failure", error.message(), std::nullopt});
        return;
    }
    const auto resolved = std::filesystem::weakly_canonical(
        target.is_absolute() ? target : absolute.parent_path() / target, error);
    if (error || !isWithin(resolved, std::filesystem::weakly_canonical(root))) {
        findings.push_back(
            {relative, "unsafe symlink", "symlink resolves outside the repository", std::nullopt});
    }
}

struct CredentialSignature {
    std::string_view label;
    std::string_view hint;
    std::regex matcher;
};

[[nodiscard]] auto credentialSignatures() -> const std::vector<CredentialSignature>& {
    static const auto signatures = std::vector<CredentialSignature>{
        {"private key", "PRIVATE KEY",
         std::regex{R"(-----BEGIN (?:EC |OPENSSH |PGP |RSA )?PRIVATE KEY-----)"}},
        {"AWS access key", "AKIA", std::regex{R"(\b(?:AKIA|ASIA)[A-Z0-9]{16}\b)"}},
        {"AWS access key", "ASIA", std::regex{R"(\b(?:AKIA|ASIA)[A-Z0-9]{16}\b)"}},
        {"GitHub token", "gh", std::regex{R"(\bgh[pousr]_[A-Za-z0-9]{24,}\b)"}},
        {"GitLab token", "glpat-", std::regex{R"(\bglpat-[A-Za-z0-9_-]{20,}\b)"}},
        {"Slack token", "xox", std::regex{R"(\bxox[baprs]-[A-Za-z0-9-]{10,}\b)"}},
        {"credential-bearing URL", "://",
         std::regex{R"(\b[a-z][a-z0-9+.-]*://[^\s/:@]+:[^\s/@]+@)",
                    std::regex::ECMAScript | std::regex::icase}}};
    return signatures;
}

void appendContentFindings(const Path& relative, const std::string& text,
                           std::vector<RepositoryFinding>& findings) {
    static const std::regex posixUserPath{R"bloom(/(?:home|Users)/([^/\s`"'<>]+)/)bloom"};
    static const std::regex windowsUserPath{
        R"bloom(\b[A-Za-z]:[\\/]+Users[\\/]+([^\\/\s`"'<>]+)[\\/])bloom"};
    static const std::regex quotedSecretAssignment{
        R"bloom(\b(?:api[_-]?key|auth[_-]?token|password|passwd|secret|access[_-]?token)\b\s*[:=]\s*['"]([^'"]+)['"])bloom",
        std::regex::ECMAScript | std::regex::icase};
    static const std::regex yamlSecretAssignment{
        R"bloom(^\s*(?:api[_-]?key|auth[_-]?token|password|passwd|secret|access[_-]?token)\s*:\s*([^\s#]+))bloom",
        std::regex::ECMAScript | std::regex::icase};

    std::istringstream lines{text};
    std::string line;
    auto lineNumber = std::size_t{0};
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (line.find("/home/") != std::string::npos || line.find("/Users/") != std::string::npos ||
            line.find("Users\\") != std::string::npos) {
            for (const auto* matcher : {&posixUserPath, &windowsUserPath}) {
                for (auto match = std::sregex_iterator{line.begin(), line.end(), *matcher};
                     match != std::sregex_iterator{}; ++match) {
                    const auto user = (*match)[1].str();
                    if (!isPlaceholderUser(user)) {
                        findings.push_back(
                            {relative, "machine-specific path",
                             "absolute user path contains account name '" + user + "'",
                             lineNumber});
                    }
                }
            }
        }

        for (const auto& signature : credentialSignatures()) {
            if (line.find(signature.hint) != std::string::npos &&
                std::regex_search(line, signature.matcher)) {
                findings.push_back({relative, "credential",
                                    "possible " + std::string{signature.label}, lineNumber});
            }
        }

        const auto lowerLine = asciiLower(line);
        if (lowerLine.find("key") != std::string::npos ||
            lowerLine.find("token") != std::string::npos ||
            lowerLine.find("password") != std::string::npos ||
            lowerLine.find("passwd") != std::string::npos ||
            lowerLine.find("secret") != std::string::npos) {
            for (const auto* matcher : {&quotedSecretAssignment, &yamlSecretAssignment}) {
                std::smatch match;
                if (std::regex_search(line, match, *matcher) &&
                    !isPlaceholderSecret(match[1].str())) {
                    findings.push_back({relative, "credential",
                                        "secret-like assignment contains a non-placeholder value",
                                        lineNumber});
                }
            }
        }
    }
}

void appendRepositoryPolicyFindings(const Path& root, const std::span<const Path> candidates,
                                    std::vector<RepositoryFinding>& findings) {
    std::string licenseText;
    if (!std::filesystem::is_regular_file(root / "LICENSE")) {
        findings.push_back(
            {"LICENSE", "license policy", "root Apache-2.0 license is missing", std::nullopt});
    } else if (!readText(root / "LICENSE", licenseText) ||
               licenseText.find("Apache License") == std::string::npos ||
               licenseText.find("Version 2.0") == std::string::npos) {
        findings.push_back({"LICENSE", "license policy",
                            "root license does not identify the Apache License, Version 2.0",
                            std::nullopt});
    }
    if (!std::filesystem::is_regular_file(root / "NOTICE")) {
        findings.push_back(
            {"NOTICE", "license policy", "root NOTICE file is missing", std::nullopt});
    }

    auto vendoredComponents = std::set<Path>{};
    auto candidateNames = std::set<std::string, std::less<>>{};
    for (const auto& relative : candidates) {
        candidateNames.insert(normalizedRelative(relative));
        const auto parts = pathParts(relative);
        for (std::size_t index = 0; index + 2 < parts.size(); ++index) {
            if (!contains(kVendoredDirectoryNames, asciiLower(parts[index]))) {
                continue;
            }
            auto component = Path{};
            for (std::size_t componentIndex = 0; componentIndex <= index + 1; ++componentIndex) {
                component /= parts[componentIndex];
            }
            vendoredComponents.insert(std::move(component));
            break;
        }
    }

    if (vendoredComponents.empty()) {
        return;
    }
    if (!std::filesystem::is_regular_file(root / "THIRD_PARTY_NOTICES.md")) {
        findings.push_back({"THIRD_PARTY_NOTICES.md", "third-party attribution",
                            "vendored content requires a repository-level attribution inventory",
                            std::nullopt});
    }
    for (const auto& component : vendoredComponents) {
        const auto hasLicense =
            std::ranges::any_of(kVendoredLicenseFilenames, [&](const std::string_view name) {
                return candidateNames.contains(normalizedRelative(component / name));
            });
        if (!hasLicense) {
            findings.push_back({component, "third-party attribution",
                                "vendored component must retain an upstream license or notice file",
                                std::nullopt});
        }
    }
}

[[nodiscard]] auto sortedFiles(std::span<const Path> files) -> std::vector<Path> {
    auto result = std::vector<Path>{files.begin(), files.end()};
    std::ranges::sort(result, {}, [](const Path& path) { return path.generic_string(); });
    return result;
}

constexpr auto kKnownModules = std::to_array<std::string_view>(
    {"core", "platform", "document", "commands", "project", "render", "runtime", "media", "color",
     "output", "host", "scripting", "mcp", "svgartwork"});
constexpr auto kAllowedModuleDependencies =
    std::to_array<std::pair<std::string_view, std::string_view>>({
        {"platform", "core"},      {"document", "core"},      {"commands", "core"},
        {"commands", "document"},  {"project", "core"},       {"project", "document"},
        {"project", "platform"},   {"render", "core"},        {"runtime", "core"},
        {"runtime", "document"},   {"runtime", "render"},     {"media", "core"},
        {"commands", "media"}, /* Asset preparation runs before command publication. */
        {"commands", "color"}, /* LUT import uses bounded off-thread color intake. */
        {"media", "color"},    /* Qualified input conversion belongs to color. */
        {"media", "platform"},     {"media", "runtime"}, /* Only bloom_media_worker_host uses the
                                                            core-only scheduler target. */
        {"media", "render"},       {"color", "core"},         {"color", "platform"},
        {"color", "render"},       {"runtime", "media"}, /* Image evaluation consumes the bounded
                                                            media decoder. */
        {"runtime", "color"},      {"output", "color"},       {"output", "core"},
        {"runtime", "platform"}, /* ADR 0025: identity-verified source digests and revalidation. */
        {"commands", "platform"}, /* ADR 0025: asset import records the source file identity. */
        {"output", "document"},    {"output", "media"},       {"output", "platform"},
        {"output", "render"},      {"output", "runtime"},     {"host", "color"},
        {"host", "commands"},      {"host", "core"},          {"host", "document"},
        {"host", "output"},        {"host", "platform"},      {"host", "project"},
        {"host", "runtime"},       {"scripting", "commands"}, {"scripting", "core"},
        {"scripting", "document"}, {"scripting", "host"},     {"scripting", "output"},
        {"scripting", "platform"}, {"scripting", "runtime"},  {"mcp", "core"},
        {"mcp", "platform"},       {"mcp", "document"},       {"mcp", "commands"},
        {"mcp", "runtime"},        {"mcp", "output"},         {"mcp", "host"},
        {"mcp", "scripting"}, /* ADR 0034: MCP server core and host. */
        {"svgartwork", "core"},    {"svgartwork", "document"}, {"svgartwork", "media"},
        {"svgartwork", "platform"}, {"commands", "svgartwork"},
        {"runtime", "svgartwork"}, /* ADR 0038: the SVG artwork builder and expansion. */
    });

[[nodiscard]] constexpr auto isAllowedDependency(const std::string_view module,
                                                 const std::string_view imported) -> bool {
    return std::ranges::find(kAllowedModuleDependencies, std::pair{module, imported}) !=
           kAllowedModuleDependencies.end();
}

[[nodiscard]] auto sourceModule(const Path& relative) -> std::optional<std::string> {
    const auto parts = pathParts(relative);
    if (parts.size() < 2 || parts[0] != "src") {
        return std::nullopt;
    }
    return parts[1];
}

[[nodiscard]] auto isNonUiSource(const Path& relative) -> bool {
    const auto parts = pathParts(relative);
    return parts.size() >= 2 && parts[0] == "src" && parts[1] != "ui";
}

void appendPublicHeaderPathFinding(const Path& relative, std::vector<RepositoryFinding>& findings) {
    const auto parts = pathParts(relative);
    if (parts.size() < 4 || parts[0] != "src" || parts[2] != "include") {
        return;
    }
    const auto matches = parts.size() >= 5 && parts[3] == "bloom" && parts[4] == parts[1];
    if (!matches) {
        findings.push_back(
            {relative, "architecture",
             "public header must live below src/" + parts[1] + "/include/bloom/" + parts[1], 1});
    }
}

void appendArchitectureContentFindings(const Path& relative, const std::string& text,
                                       std::vector<RepositoryFinding>& findings) {
    static const std::regex qtToken{
        R"bloom((?:#\s*include\s*[<"]Q[A-Z]|\bQt::|\bQ[A-Z][A-Za-z0-9_]*\b))bloom"};
    static const std::regex forbiddenSourceInclude{
        R"bloom(#\s*include\s*[<"](?:\.\.[/\\]|apps[/\\]|tests[/\\]|src[/\\]))bloom"};
    static const std::regex moduleInclude{
        R"bloom(#\s*include\s*[<"]bloom/([a-z][a-z0-9_]*)/)bloom"};

    // A thread the standard library starts takes the platform's default stack, which is 8 MiB on
    // one platform and 512 KiB on another. Production code starts threads through
    // bloom/core/worker_thread.hpp, which states the size once. std::thread::id and
    // std::thread::hardware_concurrency start nothing and stay allowed.
    static const std::regex standardThread{
        R"bloom(\bstd::(?:jthread\b|async\b|thread\b(?!::)))bloom"};
    // The standard at-thread-exit calls work only on threads one standard library started
    // itself, so they fail on Bloom's worker threads there. core::runAtThreadExit works on all.
    static const std::regex standardThreadExit{R"bloom(\b\w+_at_thread_exit\b)bloom"};

    const auto module = sourceModule(relative);
    const auto parts = pathParts(relative);
    const bool startsThreadsThroughCore =
        !parts.empty() && (parts[0] == "src" || parts[0] == "apps") &&
        std::ranges::find(parts, "tests") == parts.end() &&
        !relative.stem().generic_string().ends_with("_tests") && relative.stem() != "worker_thread";
    std::istringstream lines{text};
    std::string line;
    auto lineNumber = std::size_t{0};
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (startsThreadsThroughCore && (line.find("std::") != std::string::npos ||
                                         line.find("_at_thread_exit") != std::string::npos)) {
            const auto comment = line.find("//");
            const auto code = std::string_view(line).substr(0, comment);
            if (std::regex_search(code.begin(), code.end(), standardThread)) {
                findings.push_back(
                    {relative, "architecture",
                     "production code starts threads through bloom/core/worker_thread.hpp, not "
                     "std::thread, std::jthread or std::async",
                     lineNumber});
            }
            if (std::regex_search(code.begin(), code.end(), standardThreadExit)) {
                findings.push_back(
                    {relative, "architecture",
                     "production code registers thread-exit work through core::runAtThreadExit "
                     "(bloom/core/worker_thread.hpp), not the standard at-thread-exit calls",
                     lineNumber});
            }
        }
        if (isNonUiSource(relative) && line.find('Q') != std::string::npos &&
            std::regex_search(line, qtToken)) {
            findings.push_back({relative, "architecture",
                                "Qt types are restricted to src/ui and apps", lineNumber});
        }
        if (!parts.empty() && parts[0] == "src" && line.find("#include") != std::string::npos &&
            std::regex_search(line, forbiddenSourceInclude)) {
            findings.push_back(
                {relative, "architecture",
                 "src modules must not include apps/tests or bypass public include roots",
                 lineNumber});
        }

        std::smatch match;
        if (!module || line.find("bloom/") == std::string::npos ||
            !std::regex_search(line, match, moduleInclude)) {
            continue;
        }
        if (!contains(kKnownModules, *module)) {
            continue;
        }
        const auto imported = match[1].str();
        if (imported != *module && !isAllowedDependency(*module, imported)) {
            findings.push_back({relative, "architecture",
                                "src/" + *module + " may not depend on bloom/" + imported,
                                lineNumber});
        }
    }
}

void appendCmakeArchitectureFindings(const Path& relative, const std::string& text,
                                     std::vector<RepositoryFinding>& findings) {
    const auto parts = pathParts(relative);
    if (relative.filename() != "CMakeLists.txt" || parts.size() < 3 || parts[0] != "src" ||
        parts[1] == "ui") {
        return;
    }
    std::istringstream lines{text};
    std::string line;
    auto lineNumber = std::size_t{0};
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (line.find("Qt6::") != std::string::npos) {
            findings.push_back({relative, "architecture",
                                "non-UI source module links directly to Qt", lineNumber});
        }
    }
}

// A test registered on some platforms only must say why, in a comment directly above the
// condition or on the first line inside it. Without that rule a test written on one system stays
// off the others for a reason nobody recorded, and a platform's coverage erodes unseen.
[[nodiscard]] bool namesPlatform(const std::string_view text) {
    constexpr auto kPredicates = std::to_array<std::string_view>(
        {"APPLE", "WIN32", "UNIX", "CMAKE_SYSTEM_NAME", "BLOOM_MEDIA_FIXTURE_TESTS"});
    const auto isWord = [](const char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_';
    };
    for (const auto predicate : kPredicates) {
        for (auto at = text.find(predicate); at != std::string_view::npos;
             at = text.find(predicate, at + 1)) {
            const auto end = at + predicate.size();
            if ((at == 0 || !isWord(text[at - 1])) && (end == text.size() || !isWord(text[end])))
                return true;
        }
    }
    return false;
}

[[nodiscard]] std::string_view trimmed(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
        line.remove_suffix(1);
    return line;
}

[[nodiscard]] bool startsCommand(const std::string_view line, const std::string_view command) {
    if (!line.starts_with(command))
        return false;
    const auto rest = trimmed(line.substr(command.size()));
    return rest.starts_with("(");
}

void appendPlatformTestReasonFindings(const Path& relative, const std::string& text,
                                      std::vector<RepositoryFinding>& findings) {
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start <= text.size();) {
        const auto end = std::min(text.find('\n', start), text.size());
        lines.push_back(trimmed(std::string_view(text).substr(start, end - start)));
        start = end + 1;
    }
    struct Block {
        std::size_t line = 0;
        bool platform = false;
        bool reason = false;
        bool registersTest = false;
    };
    std::vector<Block> open;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto line = lines[index];
        if (startsCommand(line, "if")) {
            Block block{index + 1, namesPlatform(line), false, false};
            // A condition may continue over the following lines.
            auto last = index;
            auto depth = std::ranges::count(line, '(') - std::ranges::count(line, ')');
            while (depth > 0 && last + 1 < lines.size()) {
                ++last;
                block.platform = block.platform || namesPlatform(lines[last]);
                depth +=
                    std::ranges::count(lines[last], '(') - std::ranges::count(lines[last], ')');
            }
            auto above = index;
            while (above > 0 && lines[above - 1].empty())
                --above;
            block.reason = (above > 0 && lines[above - 1].starts_with("#")) ||
                           (last + 1 < lines.size() && lines[last + 1].starts_with("#"));
            open.push_back(block);
        } else if (startsCommand(line, "endif")) {
            if (open.empty())
                continue;
            const auto block = open.back();
            open.pop_back();
            if (block.platform && block.registersTest && !block.reason) {
                findings.push_back({relative, "test coverage",
                                    "tests registered under a platform condition need a comment "
                                    "saying why, directly above the condition or first inside it",
                                    block.line});
            }
        } else if (startsCommand(line, "bloom_add_test") || startsCommand(line, "add_test")) {
            for (auto& block : open)
                block.registersTest = true;
        }
    }
}

} // namespace

auto RepositoryFinding::render() const -> std::string {
    auto location = path.generic_string();
    if (line) {
        location += ':' + std::to_string(*line);
    }
    return location + ": " + category + ": " + message;
}

auto repositoryFiles(const Path& root) -> std::vector<Path> {
    std::vector<Path> files;
    std::error_code error;
    auto iterator = std::filesystem::recursive_directory_iterator{
        root, std::filesystem::directory_options::skip_permission_denied, error};
    const auto end = std::filesystem::recursive_directory_iterator{};
    while (!error && iterator != end) {
        const auto& entry = *iterator;
        const auto filename = entry.path().filename().generic_string();
        if (entry.is_directory(error) && !error && contains(kIgnoredDirectories, filename)) {
            iterator.disable_recursion_pending();
        } else if (!error && (entry.is_regular_file(error) || entry.is_symlink(error))) {
            if (!error) {
                files.push_back(entry.path().lexically_relative(root));
            }
        }
        iterator.increment(error);
    }
    std::ranges::sort(files, {}, [](const Path& path) { return path.generic_string(); });
    return files;
}

auto repositoryFilesFromNullManifest(const Path& root, const Path& manifest) -> std::vector<Path> {
    std::string bytes;
    if (!readText(manifest, bytes)) {
        throw std::runtime_error("could not read repository file manifest " + manifest.string());
    }
    if (!bytes.empty() && bytes.back() != '\0') {
        throw std::invalid_argument("repository file manifest is not NUL terminated");
    }

    std::vector<Path> files;
    auto offset = std::size_t{0};
    while (offset < bytes.size()) {
        const auto terminator = bytes.find('\0', offset);
        const auto relative = Path{bytes.substr(offset, terminator - offset)}.lexically_normal();
        offset = terminator + 1;
        if (relative.empty() || relative.is_absolute() ||
            std::ranges::find(relative, Path{".."}) != relative.end()) {
            throw std::invalid_argument("repository file manifest contains an unsafe path");
        }

        std::error_code error;
        const auto status = std::filesystem::symlink_status(root / relative, error);
        if (!error && status.type() != std::filesystem::file_type::not_found) {
            files.push_back(relative);
        }
    }
    std::ranges::sort(files, {}, [](const Path& path) { return path.generic_string(); });
    return files;
}

auto scanRepositoryHygiene(const Path& root) -> std::vector<RepositoryFinding> {
    const auto files = repositoryFiles(root);
    return scanRepositoryHygiene(root, files);
}

auto scanRepositoryHygiene(const Path& root, const std::span<const Path> files)
    -> std::vector<RepositoryFinding> {
    const auto candidates = sortedFiles(files);
    std::vector<RepositoryFinding> findings;
    appendRepositoryPolicyFindings(root, candidates, findings);

    for (const auto& relative : candidates) {
        appendPathFindings(root, relative, findings);
        if (contains(kContentScanExclusions, normalizedRelative(relative)) ||
            !isTextFile(relative)) {
            continue;
        }

        std::error_code error;
        const auto absolute = root / relative;
        const auto size = std::filesystem::file_size(absolute, error);
        if (!error && size > kMaximumScannedTextBytes) {
            continue;
        }
        std::string text;
        if (error || !readText(absolute, text)) {
            findings.push_back({relative, "read failure",
                                error ? error.message() : "could not read file", std::nullopt});
            continue;
        }
        appendContentFindings(relative, text, findings);
    }
    return findings;
}

auto scanArchitectureBoundaries(const Path& root) -> std::vector<RepositoryFinding> {
    const auto files = repositoryFiles(root);
    return scanArchitectureBoundaries(root, files);
}

auto scanArchitectureBoundaries(const Path& root, const std::span<const Path> files)
    -> std::vector<RepositoryFinding> {
    const auto candidates = sortedFiles(files);
    std::vector<RepositoryFinding> findings;
    for (const auto& relative : candidates) {
        // Unmodified pinned vendor header: its FAQ's "QUICK NOTES" is not a Qt type. The
        // Bloom-owned adapter remains fully checked and is the only TU allowed to include it.
        if (relative == "src/media/third_party/stb_image/stb_image.h")
            continue;
        const auto suffix = asciiLower(relative.extension().generic_string());
        if (contains(kCppSuffixes, suffix)) {
            appendPublicHeaderPathFinding(relative, findings);
            std::string text;
            if (readText(root / relative, text)) {
                appendArchitectureContentFindings(relative, text, findings);
            }
        }

        if (relative.filename() == "CMakeLists.txt") {
            std::string text;
            if (readText(root / relative, text)) {
                appendCmakeArchitectureFindings(relative, text, findings);
                appendPlatformTestReasonFindings(relative, text, findings);
            }
        }
    }
    return findings;
}

} // namespace bloom::quality
