// The lookup of the carried interpreter (issue #677): the package layout of this platform, the
// build tree's fallback, and no interpreter at all.
#include <interpreter_home.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
void touch(const std::filesystem::path& file) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "#\n";
}
} // namespace

int main() {
    using namespace bloom::scripting::python;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "bloom-interpreter-home-test";
    fs::remove_all(root);

    // A package: the program where this platform's packages keep it.
#if defined(_WIN32)
    const auto program = root / "package" / "bloom-cli.exe";
    const auto expectedHome = root / "package" / "python";
    const auto expectedPackage = root / "package" / "bloom-python";
#elif defined(__APPLE__)
    const auto program = root / "package" / "Bloom.app" / "Contents" / "MacOS" / "bloom-cli";
    const auto expectedHome = root / "package" / "Bloom.app" / "Contents" / "Resources" / "python";
    const auto expectedPackage =
        root / "package" / "Bloom.app" / "Contents" / "Resources" / "bloom-python";
#else
    const auto program = root / "package" / "bin" / "bloom-cli";
    const auto expectedHome = root / "package" / "share" / "bloom" / "python";
    const auto expectedPackage = root / "package" / "share" / "bloom" / "bloom-python";
#endif
    touch(program);
    const auto layout = carriedPythonLayout(program);
    expect(layout.home == expectedHome && layout.package == expectedPackage,
           "the layout is this platform's: " + layout.home.string());

    const auto buildHome = root / "prefix" / "python";
    // Nothing anywhere: no carried interpreter.
    auto found = locateCarriedPython(program, buildHome, 3, 14);
    expect(found.home.empty() && found.package.empty(), "no standard library, no carried home");

    // Only the dependency prefix has one: a program run from the build tree.
    touch(standardLibrary(buildHome, 3, 14) / "os.py");
    found = locateCarriedPython(program, buildHome, 3, 14);
    expect(found.home == buildHome && found.package.empty(),
           "the build tree's interpreter is used when the package has none");
    // Another minor version's library is not this interpreter's (the name carries the version
    // everywhere but on Windows).
#if !defined(_WIN32)
    expect(locateCarriedPython(program, buildHome, 3, 13).home.empty(),
           "a standard library of another version is not taken");
#endif

    // The package has one: it wins, with and without Bloom's own package beside it.
    touch(standardLibrary(expectedHome, 3, 14) / "os.py");
    found = locateCarriedPython(program, buildHome, 3, 14);
    expect(found.home == expectedHome && found.package.empty(),
           "the package's interpreter wins over the build tree's");
    touch(expectedPackage / "bloom" / "__init__.py");
    found = locateCarriedPython(program, buildHome, 3, 14);
    expect(found.home == expectedHome && found.package == expectedPackage,
           "Bloom's package is found beside the package's interpreter");
    expect(sitePackages(expectedHome, 3, 14).parent_path() == standardLibrary(expectedHome, 3, 14),
           "the bundled packages sit inside the standard library directory");

    // No program path: only the build tree's can be found.
    expect(locateCarriedPython({}, buildHome, 3, 14).home == buildHome,
           "without a program path the build tree's interpreter is used");

    // This process: its own path resolves to an existing file.
    const auto self = runningProgram();
    expect(!self.empty() && fs::is_regular_file(self), "the running program is found");

    // Certificates: a file that exists, or none (Windows reads the system store itself).
    const auto certificates = systemCertificateFile();
#if defined(_WIN32)
    expect(!certificates.has_value(), "Windows names no certificate file");
#else
    expect(!certificates || fs::is_regular_file(*certificates),
           "the certificate file named exists");
    if (certificates)
        std::cout << "certificates: " << certificates->string() << '\n';
    else
        std::cout << "certificates: none of the known files exists on this computer\n";
#endif

    fs::remove_all(root);
    if (failures == 0)
        std::cout << "interpreter home: ok\n";
    return failures == 0 ? 0 : 1;
}
