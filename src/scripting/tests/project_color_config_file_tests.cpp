// Issue #680: an external or project-relative OCIO configuration can be selected, read back and
// cleared through the scripting Session (the surface Python and MCP share), as one undoable
// history entry, and a configuration that cannot be read is refused in plain words. One loose
// copy of the ACES CG config and an archive of it serve as the files.

#include <bloom/core/file_uri.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/host/project_color_choice.hpp>
#include <bloom/scripting/session.hpp>

#include <OpenColorIO/OpenColorIO.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {

using namespace bloom;
namespace OCIO = OCIO_NAMESPACE;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
}

std::string pathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

host::ProjectColorConfigFile fileAt(const std::filesystem::path& path,
                                    const bool projectRelative = false) {
    host::ProjectColorConfigFile file;
    file.path = projectRelative ? path.generic_string() : pathText(path);
    file.projectRelative = projectRelative;
    return file;
}

bool builtIn(const std::optional<document::ColorSettings>& settings) {
    return settings && std::holds_alternative<document::BuiltInOcioConfigLocator>(
                           settings->ocioConfig.locator);
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("bloom-color-file-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto project = root / "project";
    std::filesystem::create_directories(project);

    // The files: the ACES CG config as a loose folder, and as an archive beside the project file.
    {
        const auto builtInConfig = OCIO::Config::CreateFromBuiltinConfig(
            "cg-config-v1.0.0_aces-v1.3_ocio-v2.1");
        std::ostringstream text;
        builtInConfig->serialize(text);
        write(root / "aces" / "config.ocio", text.str());
        const auto config = OCIO::Config::CreateFromFile((root / "aces" / "config.ocio").string().c_str());
        std::ofstream archive(project / "shot.ocioz", std::ios::binary);
        config->archive(archive);
        write(root / "garbage" / "config.ocio", "this: is: not: [a config\n");
        write(root / "needs-lut" / "config.ocio", [&] {
            std::ostringstream lut;
            const auto edited = OCIO::Config::CreateFromFile(
                (root / "aces" / "config.ocio").string().c_str())->createEditableCopy();
            const auto space = OCIO::ColorSpace::Create();
            space->setName("needs lut");
            const auto transform = OCIO::FileTransform::Create();
            transform->setSrc("missing.spi1d");
            space->setTransform(transform, OCIO::COLORSPACE_DIR_TO_REFERENCE);
            edited->addColorSpace(space);
            edited->serialize(lut);
            return lut.str();
        }());
    }

    auto created = scripting::Session::createNew("Color files", "Main");
    expect(static_cast<bool>(created), "the session opens");
    if (!created)
        return 1;
    auto session = std::move(created).takeSession();
    expect(session->saveAs(project / "project.bloom").succeeded(),
           "the project is saved, so it has a folder");
    const auto original = session->renderSourceSnapshot().colorSettings;
    expect(builtIn(original), "a new project uses a built-in configuration");

    // Show: "current" reads the configuration in force without changing anything.
    const auto shown = session->setColorSettings("current", std::nullopt);
    expect(shown.code == "no-change" && shown.settings &&
               host::describeProjectColorConfig(*shown.settings).kind == "neutral",
           "current shows the configuration in force");

    // Select an external loose configuration; one undoable step.
    const auto before = session->snapshot().revision();
    auto chosen = session->setColorSettings("file", std::nullopt, fileAt(root / "aces" / "config.ocio"));
    expect(chosen.code == "updated" && chosen.settings, "a loose config.ocio is selected: " +
                                                            chosen.message);
    if (!chosen.settings)
        return 1;
    const auto description = host::describeProjectColorConfig(*chosen.settings);
    expect(std::holds_alternative<document::ExternalOcioConfigLocator>(
               chosen.settings->ocioConfig.locator) &&
               description.kind == "external-config" &&
               description.location == pathText(root / "aces" / "config.ocio") &&
               description.revision.size() == 64 &&
               chosen.settings->processColorSpaceId == "ACEScg" &&
               chosen.settings->ocioConfig.portability == document::OcioConfigPortability::External,
           "the stored reference names the file, its revision and its own scene-linear space");
    expect(session->snapshot().revision() != before, "choosing a file is a history entry");
    expect(session->setColorSettings("file", std::nullopt, fileAt(root / "aces" / "config.ocio"))
                   .code == "no-change",
           "choosing the same file again changes nothing");

    // Undo and redo restore the configuration.
    expect(session->undo().succeeded() && builtIn(session->renderSourceSnapshot().colorSettings) &&
               *session->renderSourceSnapshot().colorSettings == *original,
           "undo restores the previous configuration and working space");
    expect(session->redo().succeeded() &&
               session->renderSourceSnapshot().colorSettings == chosen.settings,
           "redo selects the file again");

    // Clear: back to a built-in, as one more step.
    const auto cleared = session->setColorSettings("clear", std::nullopt);
    expect(cleared.code == "updated" && builtIn(cleared.settings) &&
               cleared.settings->processColorSpaceId == "lin_rec709_scene",
           "clear returns to Bloom Neutral");
    expect(session->undo().succeeded() &&
               session->renderSourceSnapshot().colorSettings == chosen.settings,
           "undoing the clear brings the file back");

    // An external archive, a project-relative archive and context variables.
    auto archive = fileAt(project / "shot.ocioz");
    const auto external = session->setColorSettings("file", std::nullopt, archive);
    expect(external.code == "updated" && external.settings &&
               host::describeProjectColorConfig(*external.settings).kind == "external-archive",
           "an external .ocioz archive is selected: " + external.message);
    auto relative = fileAt("shot.ocioz", true);
    relative.contextVariables = {{"SHOW", "sample"}};
    const auto inProject = session->setColorSettings("file", std::nullopt, relative);
    const auto relativeDescription = inProject.settings
                                         ? host::describeProjectColorConfig(*inProject.settings)
                                         : host::ProjectColorConfigDescription{};
    expect(inProject.code == "updated" && relativeDescription.kind == "project-relative-archive" &&
               relativeDescription.location == "shot.ocioz" &&
               relativeDescription.contextVariables.size() == 1 &&
               inProject.settings->ocioConfig.portability ==
                   document::OcioConfigPortability::ProjectRelative,
           "a project-relative archive is selected with its context variables: " +
               inProject.message);

    // Refusals, each in plain words and none changing the project.
    const auto steady = session->renderSourceSnapshot().colorSettings;
    const auto refused = [&](const host::ProjectColorConfigFile& file, const std::string& phrase,
                             const std::string& label) {
        const auto result = session->setColorSettings("file", std::nullopt, file);
        expect(result.code == "invalid-settings" &&
                   result.message.find(phrase) != std::string::npos,
               label + " (" + result.code + ": " + result.message + ")");
        expect(session->renderSourceSnapshot().colorSettings == steady,
               label + ": the project is unchanged");
    };
    refused(fileAt(root / "absent" / "config.ocio"), "can't find", "a missing file");
    refused(fileAt(root / "garbage" / "config.ocio"), "couldn't read", "an unparseable config");
    refused(fileAt(root / "needs-lut" / "config.ocio"), "not a valid configuration",
            "a config whose LUT is not beside it");
    refused(fileAt(root / "aces" / "colors.txt"), "config.ocio", "a file that is not a config");
    refused(fileAt("aces/config.ocio"), "full path", "a relative path that is not project-relative");
    refused(fileAt("../outside.ocioz", true), "below the project", "a project-relative path that leaves");
    expect(session->setColorSettings("file", std::nullopt).code == "invalid-config",
           "config file without a file is refused");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    if (failures == 0)
        std::cout << "project color config file tests passed\n";
    return failures == 0 ? 0 : 1;
}
