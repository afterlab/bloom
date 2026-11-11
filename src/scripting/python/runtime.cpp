#include "runtime.hpp"
#include "native.hpp"

#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// CPython fixes this symbol spelling for the private _bloom module.
extern "C" PyObject* PyInit__bloom(); // NOLINT(bugprone-reserved-identifier)

namespace bloom::scripting::python {

struct EmbeddedPython::State final {
    PyThreadState* thread = nullptr;
    PyObject* console = nullptr;
    mutable std::mutex mutex;
    std::shared_ptr<NativeHost> host;
    std::shared_ptr<PythonRootRetirements> roots;
};

namespace {

void checkStatus(const PyStatus status) {
    if (PyStatus_Exception(status) != 0) {
        throw std::runtime_error(status.err_msg == nullptr ? "Python initialization failed"
                                                           : status.err_msg);
    }
}

#if defined(__APPLE__)
// Bloom carries its own interpreter on macOS: the standard library is inside the application
// bundle (Contents/Resources/python) and, for a program run from the build tree, in the
// dependency prefix the build names. CPython compiles in the directory it was installed to, which
// does not exist on another machine, so the home is given to it. Empty when neither is there: the
// interpreter then finds its library the way it always has.
std::filesystem::path runningExecutable() {
    std::uint32_t size = 0;
    static_cast<void>(::_NSGetExecutablePath(nullptr, &size));
    std::string buffer(size, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code error;
    auto resolved = std::filesystem::canonical(std::filesystem::path(buffer.c_str()), error);
    return error ? std::filesystem::path{} : resolved;
}

bool holdsStandardLibrary(const std::filesystem::path& home) {
    const auto library =
        home / "lib" /
        ("python" + std::to_string(PY_MAJOR_VERSION) + "." + std::to_string(PY_MINOR_VERSION));
    std::error_code error;
    return std::filesystem::is_regular_file(library / "os.py", error);
}

std::filesystem::path bundleResources() {
    const auto program = runningExecutable();
    return program.empty() ? std::filesystem::path{}
                           : program.parent_path().parent_path() / "Resources";
}

std::filesystem::path carriedPythonHome() {
    if (const auto resources = bundleResources(); !resources.empty()) {
        if (auto home = resources / "python"; holdsStandardLibrary(home)) {
            return home;
        }
    }
#if defined(BLOOM_PYTHON_HOME)
    if (std::filesystem::path home(BLOOM_PYTHON_HOME); holdsStandardLibrary(home)) {
        return home;
    }
#endif
    return {};
}

// The carried interpreter's OpenSSL has no certificate directory of its own, so that it trusts
// nothing by accident. It is pointed at the operating system's certificate file, which keeps
// HTTPS from scripts verified. A choice the user made in the environment is left alone.
void trustSystemCertificates() {
    constexpr auto* systemCertificates = "/etc/ssl/cert.pem";
    std::error_code error;
    if (std::getenv("SSL_CERT_FILE") == nullptr && std::getenv("SSL_CERT_DIR") == nullptr &&
        std::filesystem::is_regular_file(systemCertificates, error)) {
        static_cast<void>(::setenv("SSL_CERT_FILE", systemCertificates, 0));
    }
}
#endif

} // namespace

EmbeddedPython::EmbeddedPython(const std::filesystem::path& executable,
                               std::shared_ptr<runtime::GpuOcioContextResolver> ocioResolver)
    : state_(std::make_unique<State>()) {
    if (Py_IsInitialized() != 0) {
        throw std::runtime_error("Bloom owns one scripting interpreter per process (ADR 0022)");
    }
    setProcessGpuOcioResolver(std::move(ocioResolver));
    if (PyImport_AppendInittab("_bloom", &PyInit__bloom) != 0) {
        throw std::runtime_error("Could not register the Bloom bridge");
    }
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.site_import = 0;
    config.install_signal_handlers = 0;
    const auto program = executable.empty() ? std::string("bloom-cli") : executable.string();
    std::string initializationFailure;
    try {
        checkStatus(PyConfig_SetBytesString(&config, &config.program_name, program.c_str()));
#if defined(__APPLE__)
        if (const auto home = carriedPythonHome(); !home.empty()) {
            checkStatus(PyConfig_SetBytesString(&config, &config.home, home.c_str()));
            trustSystemCertificates();
        }
#endif
        checkStatus(Py_InitializeFromConfig(&config));
    } catch (...) {
        PyConfig_Clear(&config);
        throw;
    }
    PyConfig_Clear(&config);
    // nanobind initializes its native internals when its extension is imported. CPython owns
    // embedding; initialize the built-in bridge before using nanobind's object wrappers.
    PyObject* bridge = PyImport_ImportModule("_bloom");
    if (bridge == nullptr) {
        PyErr_Print();
        Py_FinalizeEx();
        throw std::runtime_error("Could not initialize the built-in Bloom bridge");
    }
    Py_DECREF(bridge);
    try {
        state_->roots = moduleRootRetirements();
        const auto installed = executable.parent_path().parent_path() / "lib/bloom/python";
        auto package = !executable.empty() && std::filesystem::is_directory(installed / "bloom")
                           ? installed
                           : std::filesystem::path(BLOOM_PYTHON_PACKAGE_PATH);
#if defined(__APPLE__)
        // Inside the application bundle the package sits with the other resources.
        if (const auto resources = bundleResources(); !resources.empty()) {
            if (const auto bundled = resources / "bloom-python";
                std::filesystem::is_directory(bundled / "bloom")) {
                package = bundled;
            }
        }
#endif
        auto sys = nb::module_::import_("sys");
        sys.attr("path").attr("insert")(0, package.string());
        auto console = nb::module_::import_("bloom._console").attr("Console")();
        state_->console = console.release().ptr();
    } catch (const std::exception& error) {
        initializationFailure = error.what();
    } catch (...) {
        initializationFailure = "Could not initialize the Bloom Python package";
    }
    if (!initializationFailure.empty()) {
        if (state_->roots)
            finishPythonRootShutdown(state_->roots);
        Py_XDECREF(state_->console);
        state_->console = nullptr;
        Py_FinalizeEx();
        throw std::runtime_error(initializationFailure);
    }
    state_->thread = PyEval_SaveThread();
}

EmbeddedPython::~EmbeddedPython() {
    if (state_->thread != nullptr) {
        PyEval_RestoreThread(state_->thread);
        preparePythonRootShutdown(state_->roots);
        Py_XDECREF(state_->console);
        state_->console = nullptr;
        std::shared_ptr<NativeHost> host;
        {
            const std::lock_guard lock(state_->mutex);
            host = std::move(state_->host);
        }
        host.reset();
        // Includes unbound lazy/default/manual Hosts, while their Python globals still exist.
        // False timeout retains the interpreter and every incomplete provider/owner/scheduler.
        finishPythonRootShutdown(state_->roots);
        Py_FinalizeEx();
    }
}

void EmbeddedPython::bind(std::shared_ptr<NativeHost> host) {
    const nb::gil_scoped_acquire acquire;
    nb::module_::import_("bloom._state").attr("bind")(host);
    const std::lock_guard lock(state_->mutex);
    state_->host = std::move(host);
}

void EmbeddedPython::bindLive(std::shared_ptr<Session> session,
                              std::function<PythonContext()> context,
                              std::function<bool()> cancelled,
                              std::shared_ptr<host::GpuExportProvider> provider) {
    if (!provider) {
        throw std::invalid_argument(
            "Live Python requires the application's shared processing provider");
    }
    const nb::gil_scoped_acquire acquire;
    auto host =
        std::make_shared<NativeHost>(std::move(session), std::move(provider), state_->roots);
    host->contextProvider = std::move(context);
    host->headless = false;
    host->cancelProvider = std::move(cancelled);
    bind(std::move(host));
}

void EmbeddedPython::requestCancellation() {
    const std::lock_guard lock(state_->mutex);
    if (state_->host)
        state_->host->requestCancellation();
}

std::vector<runtime::TaskSnapshot> EmbeddedPython::tasks() const {
    const std::lock_guard lock(state_->mutex);
    return state_->host ? state_->host->taskSnapshots() : std::vector<runtime::TaskSnapshot>{};
}

ExecutionResult EmbeddedPython::execute(const std::string_view source, const bool interactive) {
    const nb::gil_scoped_acquire acquire;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->host)
            state_->host->prepareExecution();
    }
    try {
        const auto result = nb::borrow<nb::object>(state_->console)
                                .attr("execute")(std::string(source), interactive);
        return {.output = nb::cast<std::string>(result[0]),
                .succeeded = nb::cast<bool>(result[1]),
                .incomplete = nb::cast<bool>(result[2])};
    } catch (const nb::python_error& error) {
        return {.output = error.what(), .succeeded = false, .incomplete = false};
    }
}

int EmbeddedPython::runFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() > std::streamoff{1024} * 1024) {
        std::cerr << "Python script is unreadable or exceeds 1 MiB\n";
        return 2;
    }
    input.seekg(0);
    const std::string source(std::istreambuf_iterator<char>(input), {});
    const auto result = execute(source);
    std::cout << result.output;
    return result.succeeded ? 0 : 4;
}

int EmbeddedPython::interactive() {
    std::cout << "Bloom Python — bloom is pre-imported. Ctrl-D exits.\n";
    std::string source;
    std::string line;
    while (std::cout << (source.empty() ? ">>> " : "... "), std::getline(std::cin, line)) {
        source += line + '\n';
        const auto result = execute(source, true);
        std::cout << result.output;
        if (!result.incomplete) {
            source.clear();
        }
    }
    return 0;
}

} // namespace bloom::scripting::python
