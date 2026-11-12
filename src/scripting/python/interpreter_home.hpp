#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace bloom::scripting::python {

// Bloom carries its own CPython in every package (issue #677), so scripting starts on a computer
// that has no Python. CPython compiles in the directory it was installed to, which does not exist
// on another computer, so Bloom gives the interpreter its home. Where the home is inside a
// package is the only thing that differs between platforms:
//
//             interpreter home                Bloom's Python package
//   macOS     Contents/Resources/python       Contents/Resources/bloom-python
//   Linux     share/bloom/python              share/bloom/bloom-python
//   Windows   python\ beside the program      bloom-python\ beside the program
//
// The macOS and Linux paths are relative to the directory above the program's own (the bundle's
// Contents, the package's root).
struct CarriedPythonLayout final {
    std::filesystem::path home;
    std::filesystem::path package;
};
[[nodiscard]] CarriedPythonLayout carriedPythonLayout(const std::filesystem::path& program);

// The standard library of interpreter version `major`.`minor` under `home`
// (lib/pythonX.Y; Lib on Windows), and its third-party directory.
[[nodiscard]] std::filesystem::path standardLibrary(const std::filesystem::path& home, int major,
                                                    int minor);
[[nodiscard]] std::filesystem::path sitePackages(const std::filesystem::path& home, int major,
                                                 int minor);

struct CarriedPython final {
    // Empty when no carried interpreter is found: the interpreter then finds its own library the
    // way it always has (a developer build against the system's Python).
    std::filesystem::path home;
    // Bloom's own package when the package layout holds it; empty otherwise.
    std::filesystem::path package;
};
// The carried interpreter for the running `program`: the package's when the package layout holds
// a standard library, else `buildHome` (the dependency prefix's interpreter, which the build names
// for programs run from the build tree) when that holds one.
[[nodiscard]] CarriedPython locateCarriedPython(const std::filesystem::path& program,
                                                const std::filesystem::path& buildHome, int major,
                                                int minor);

// The running program's own path with links resolved; empty when the system does not say.
[[nodiscard]] std::filesystem::path runningProgram();

// The carried interpreter's OpenSSL has no certificate directory of its own, so that it trusts
// nothing by accident. This is the operating system's certificate file to point it at: the one
// macOS keeps, or the first of the bundles the Linux distributions keep that exists. Empty on
// Windows, where CPython reads the system's certificate store itself, and when no file is there.
[[nodiscard]] std::optional<std::filesystem::path> systemCertificateFile();

// Sets SSL_CERT_FILE to systemCertificateFile() unless SSL_CERT_FILE or SSL_CERT_DIR is already
// set: a choice the user made in the environment is left alone.
void trustSystemCertificates();

} // namespace bloom::scripting::python
