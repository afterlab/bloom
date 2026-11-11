#include "ocio_config_worker.hpp"

#include "lut_worker_ipc.hpp"
#include "ocio_config_protocol.hpp"
#include "ocio_gpu_program_extract.hpp"
#include "ocio_gpu_program_serialize.hpp"
#include "ocio_internal.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bloom::color::detail {

#ifdef __linux__
namespace {

[[nodiscard]] std::string compilerId() {
#if defined(__clang__)
    return "Clang";
#elif defined(__GNUC__)
    return "GCC";
#elif defined(_MSC_VER)
    return "MSVC";
#else
    return "Unknown";
#endif
}

[[nodiscard]] std::string compilerVersion() {
#if defined(__clang__)
    return __clang_version__;
#elif defined(__GNUC__)
    return std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
           std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return std::to_string(_MSC_VER);
#else
    return {};
#endif
}

// Reads exactly `bytes.size()` bytes at `offset`, without moving the descriptor's position.
[[nodiscard]] bool readAt(const int fd, std::span<std::byte> bytes, std::uint64_t offset) {
    while (!bytes.empty()) {
        const auto count = ::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        if (count <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
    return true;
}

[[nodiscard]] std::optional<std::vector<std::byte>> readAll(const int fd,
                                                            const std::uint64_t bytes) {
    std::vector<std::byte> value(static_cast<std::size_t>(bytes));
    if (!readAt(fd, value, 0))
        return std::nullopt;
    return value;
}

// Reads the index of a sealed resource table in place: keys and payload positions only.
[[nodiscard]] std::optional<std::vector<ConfigResourceSlice>>
readResourceIndex(const int fd, const std::uint64_t bytes) {
    std::uint64_t at = 0;
    const auto u32 = [&](std::uint32_t& value) {
        if (bytes - at < sizeof(value) ||
            !readAt(fd, std::as_writable_bytes(std::span(&value, 1)), at))
            return false;
        at += sizeof(value);
        return true;
    };
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    if (!u32(version) || version != kConfigRecordVersion || !u32(count) || count == 0 ||
        count > kOcioLooseMaximumEntryCount)
        return std::nullopt;
    std::vector<ConfigResourceSlice> index;
    index.reserve(count);
    for (std::uint32_t entry = 0; entry < count; ++entry) {
        std::uint32_t keyBytes = 0;
        if (!u32(keyBytes) || keyBytes == 0 || keyBytes > kOcioLooseMaximumKeyBytes ||
            bytes - at < keyBytes)
            return std::nullopt;
        ConfigResourceSlice slice;
        slice.key.resize(keyBytes);
        if (!readAt(fd, std::as_writable_bytes(std::span(slice.key)), at))
            return std::nullopt;
        at += keyBytes;
        if (bytes - at < sizeof(slice.size) ||
            !readAt(fd, std::as_writable_bytes(std::span(&slice.size, 1)), at))
            return std::nullopt;
        at += sizeof(slice.size);
        if (slice.size > kOcioLooseMaximumResourceBytes || bytes - at < slice.size)
            return std::nullopt;
        slice.offset = at;
        at += slice.size;
        if (canonicalConfigResourceKey(slice.key) != slice.key ||
            (!index.empty() && !(index.back().key < slice.key)))
            return std::nullopt;
        index.push_back(std::move(slice));
    }
    if (at != bytes)
        return std::nullopt;
    return index;
}

// A loose config served from the sealed resource table. OpenColorIO asks for the config text and
// then for each file by the path it resolved; a path that is not a supplied key does not exist.
// Every key that was found is recorded: together they are the consumed resource set.
class TableProxy final : public OCIO::ConfigIOProxy {
  public:
    TableProxy(const int fd, std::vector<ConfigResourceSlice> index)
        : fd_(fd), index_(std::move(index)) {}

    std::vector<std::uint8_t> getLutData(const char* const path) const override {
        const auto* const slice = find(path);
        if (slice == nullptr)
            return {};
        std::vector<std::uint8_t> data(static_cast<std::size_t>(slice->size));
        if (!readAt(fd_, std::as_writable_bytes(std::span(data)), slice->offset))
            return {};
        return data;
    }
    std::string getConfigData() const override {
        const auto data = getLutData("config.ocio");
        return {reinterpret_cast<const char*>(data.data()), data.size()};
    }
    std::string getFastLutFileHash(const char* const path) const override {
        const auto* const slice = find(path);
        // The table is immutable for the helper's life, so the key identifies the content.
        return slice == nullptr ? std::string{} : "bloom-resource:" + slice->key;
    }
    [[nodiscard]] bool has(const std::string_view key) const {
        return std::ranges::any_of(index_, [&](const auto& slice) { return slice.key == key; });
    }
    [[nodiscard]] std::vector<std::string> consumed() const {
        return {consumed_.begin(), consumed_.end()};
    }
    // After the config was opened, a key outside the consumed set is no longer served: the
    // revision the host computed covers exactly that set.
    void close() { closed_ = true; }

  private:
    [[nodiscard]] const ConfigResourceSlice* find(const char* const path) const {
        if (path == nullptr)
            return nullptr;
        const auto key = canonicalConfigResourceKey(path);
        if (key.empty())
            return nullptr;
        const auto found =
            std::ranges::find_if(index_, [&](const auto& slice) { return slice.key == key; });
        if (found == index_.end())
            return nullptr;
        if (closed_)
            return consumed_.contains(key) ? &*found : nullptr;
        consumed_.insert(key);
        return &*found;
    }
    int fd_;
    std::vector<ConfigResourceSlice> index_;
    mutable std::set<std::string> consumed_;
    bool closed_ = false;
};

void collectFiles(const OCIO::ConstTransformRcPtr& transform, std::set<std::string>& files) {
    if (!transform)
        return;
    if (const auto group = OCIO_DYNAMIC_POINTER_CAST<const OCIO::GroupTransform>(transform)) {
        for (int index = 0; index < group->getNumTransforms(); ++index)
            collectFiles(group->getTransform(index), files);
        return;
    }
    if (const auto file = OCIO_DYNAMIC_POINTER_CAST<const OCIO::FileTransform>(transform)) {
        if (file->getSrc() != nullptr && *file->getSrc() != '\0')
            files.insert(file->getSrc());
    }
}

// Every file the config's own transforms name: color spaces, looks, view transforms and named
// transforms, active or not.
[[nodiscard]] std::set<std::string> referencedFiles(const OCIO::ConstConfigRcPtr& config) {
    std::set<std::string> files;
    const int spaces =
        config->getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ALL);
    for (int index = 0; index < spaces; ++index) {
        const char* const name = config->getColorSpaceNameByIndex(OCIO::SEARCH_REFERENCE_SPACE_ALL,
                                                                  OCIO::COLORSPACE_ALL, index);
        const auto space =
            name == nullptr ? OCIO::ConstColorSpaceRcPtr{} : config->getColorSpace(name);
        if (!space)
            continue;
        collectFiles(space->getTransform(OCIO::COLORSPACE_DIR_TO_REFERENCE), files);
        collectFiles(space->getTransform(OCIO::COLORSPACE_DIR_FROM_REFERENCE), files);
    }
    for (int index = 0; index < config->getNumLooks(); ++index) {
        const auto look = config->getLook(config->getLookNameByIndex(index));
        if (!look)
            continue;
        collectFiles(look->getTransform(), files);
        collectFiles(look->getInverseTransform(), files);
    }
    for (int index = 0; index < config->getNumViewTransforms(); ++index) {
        const auto view = config->getViewTransform(config->getViewTransformNameByIndex(index));
        if (!view)
            continue;
        for (const auto direction :
             {OCIO::VIEWTRANSFORM_DIR_TO_REFERENCE, OCIO::VIEWTRANSFORM_DIR_FROM_REFERENCE})
            collectFiles(view->getTransform(direction), files);
    }
    const int named = config->getNumNamedTransforms(OCIO::NAMEDTRANSFORM_ALL);
    for (int index = 0; index < named; ++index) {
        const auto transform = config->getNamedTransform(
            config->getNamedTransformNameByIndex(OCIO::NAMEDTRANSFORM_ALL, index));
        if (!transform)
            continue;
        for (const auto direction : {OCIO::TRANSFORM_DIR_FORWARD, OCIO::TRANSFORM_DIR_INVERSE})
            collectFiles(transform->getTransform(direction), files);
    }
    return files;
}

class Mapping final {
  public:
    Mapping(const int descriptor, const std::size_t size, const int protection)
        : data_(::mmap(nullptr, size, protection, MAP_SHARED, descriptor, 0)), size_(size) {}
    ~Mapping() {
        if (data_ != MAP_FAILED)
            ::munmap(data_, size_);
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    [[nodiscard]] bool valid() const { return data_ != MAP_FAILED; }
    [[nodiscard]] std::span<float> floats() const {
        return {static_cast<float*>(data_), size_ / sizeof(float)};
    }

  private:
    void* data_;
    std::size_t size_;
};

[[nodiscard]] bool writeReply(const int fd, const std::uint64_t extent,
                              const std::span<const std::byte> bytes) {
    return bytes.size() <= extent && ::lseek(fd, 0, SEEK_SET) == 0 && writeFd(fd, bytes);
}

} // namespace

struct ConfigWorker::State final {
    std::optional<ConfigParameters> parameters;
    int resource = -1;
    std::shared_ptr<TableProxy> table;
    std::optional<ResolvedBloomNeutralConfig> resolved;
    OCIO::ConstContextRcPtr context;
    std::vector<OCIO::ConstCPUProcessorRcPtr> processors;

    State() = default;
    ~State() {
        if (resource >= 0)
            ::close(resource);
    }
    State(const State&) = delete;
    State& operator=(const State&) = delete;

    // The context every processor of this config is built with: the config's search path and
    // resources, its declared environment defaults and then the project's own variables. Nothing
    // comes from this process's environment.
    void makeContext(const OCIO::ConstConfigRcPtr& config) {
        auto editable = config->getCurrentContext()->createEditableCopy();
        editable->clearStringVars();
        for (int index = 0; index < config->getNumEnvironmentVars(); ++index) {
            const char* const name = config->getEnvironmentVarNameByIndex(index);
            if (name != nullptr)
                editable->setStringVar(name, config->getEnvironmentVarDefault(name));
        }
        for (const auto& variable : parameters->context)
            editable->setStringVar(variable.name.c_str(), variable.value.c_str());
        context = editable;
    }

    [[nodiscard]] OCIO::ConstProcessorRcPtr processor(const ConfigProcessorRecipe& recipe,
                                                      ConfigProcessorError& error) const {
        const auto& config = resolved->impl().config();
        const std::string working(resolved->processColorSpaceId());
        switch (recipe.kind) {
        case ConfigProcessorKind::DisplayForward:
        case ConfigProcessorKind::DisplayInverse: {
            auto transform = OCIO::DisplayViewTransform::Create();
            transform->setSrc(working.c_str());
            transform->setDisplay(recipe.a.c_str());
            transform->setView(recipe.b.c_str());
            return config->getProcessor(context, transform,
                                        recipe.kind == ConfigProcessorKind::DisplayForward
                                            ? OCIO::TRANSFORM_DIR_FORWARD
                                            : OCIO::TRANSFORM_DIR_INVERSE);
        }
        case ConfigProcessorKind::ColorSpace: {
            const auto source = config->getColorSpace(recipe.a.c_str());
            if (!source) {
                error = ConfigProcessorError::MissingSource;
                return {};
            }
            if (source->isData()) {
                error = ConfigProcessorError::SourceIsData;
                return {};
            }
            const auto destination = config->getColorSpace(recipe.b.c_str());
            if (!destination) {
                error = ConfigProcessorError::MissingDestination;
                return {};
            }
            if (destination->isData()) {
                error = ConfigProcessorError::DestinationIsData;
                return {};
            }
            return config->getProcessor(context, recipe.a.c_str(), recipe.b.c_str());
        }
        case ConfigProcessorKind::InputInverseDisplay:
            return config->getProcessor(
                context, working.c_str(), std::string(resolved->displayName()).c_str(),
                std::string(resolved->viewName()).c_str(), OCIO::TRANSFORM_DIR_INVERSE);
        }
        error = ConfigProcessorError::UnknownKind;
        return {};
    }

    [[nodiscard]] LutError begin(const LutPacket& message, const int socket, bool (*confine)()) {
        if (parameters || !message[4] || message[4] > kConfigMaximumParameterBytes)
            return LutError::HelperProtocolViolation;
        const auto descriptors = receiveDescriptors(socket, 1);
        const LutFd file(descriptors[0]);
        if (file.get() < 0 || !validSharedFile(file.get(), message[7], message[4], true))
            return LutError::HelperProtocolViolation;
        if (!confine())
            return LutError::HelperUnavailable;
        const auto bytes = readAll(file.get(), message[4]);
        if (!bytes)
            return LutError::HelperProtocolViolation;
        parameters = decodeConfigParameters(*bytes);
        return parameters ? LutError::None : LutError::HelperProtocolViolation;
    }

    [[nodiscard]] LutError open(const LutPacket& message, const int socket,
                                std::uint64_t& payload) {
        const bool loose =
            parameters && parameters->kind == OcioConfigLocatorKind::ExternalLooseConfig;
        if (!parameters || resolved || resource >= 0 || !message[4] ||
            message[4] > (loose ? kOcioLooseMaximumAggregateBytes + kConfigMaximumReplyBytes
                                : kConfigMaximumArchiveBytes) ||
            !message[8] || message[8] > kConfigMaximumReplyBytes)
            return LutError::HelperProtocolViolation;
        const auto descriptors = receiveDescriptors(socket, 2);
        LutFd input(descriptors[0]);
        const LutFd output(descriptors[1]);
        if (input.get() < 0 || output.get() < 0 ||
            !validSharedFile(input.get(), message[7], message[4], true) ||
            !validSharedFile(output.get(), message[9], message[8], false))
            return LutError::HelperProtocolViolation;
        // The resource stays open for the helper's life: files are read when a processor needs
        // them.
        resource = ::dup(input.get());
        if (resource < 0)
            return LutError::HelperProtocolViolation;

        ConfigOpenReply reply;
        const auto invalid = [&](const OcioBuiltInInvalidReason reason) {
            reply.outcome = ConfigOpenOutcome::Invalid;
            reply.invalidReason = reason;
        };
        OCIO::ConstConfigRcPtr config;
        try {
            if (loose) {
                auto index = readResourceIndex(resource, message[4]);
                if (!index)
                    return LutError::HelperProtocolViolation;
                table = std::make_shared<TableProxy>(resource, std::move(*index));
                if (!table->has("config.ocio"))
                    return LutError::HelperProtocolViolation;
                config = OCIO::Config::CreateFromConfigIOProxy(table);
            } else {
                std::array<std::byte, 4> head{};
                if (!readAt(resource, head, 0))
                    return LutError::HelperProtocolViolation;
                if (!looksLikeConfigArchive(head))
                    throw OCIO::Exception("not an archive");
                // OpenColorIO reads an archive by path; the sealed descriptor has one under /proc.
                const auto path = "/proc/self/fd/" + std::to_string(resource);
                config = OCIO::Config::CreateFromFile(path.c_str());
            }
        } catch (const std::bad_alloc&) {
            return LutError::HelperMemoryLimit;
        } catch (const std::exception&) {
            invalid(OcioBuiltInInvalidReason::ParseFailed);
        }
        if (config) {
            try {
                config->validate();
            } catch (const std::bad_alloc&) {
                return LutError::HelperMemoryLimit;
            } catch (const std::exception&) {
                invalid(OcioBuiltInInvalidReason::ValidateFailed);
                config.reset();
            }
        }
        if (config) {
            try {
                makeContext(config);
                for (const auto& file : referencedFiles(config)) {
                    try {
                        // Only a supplied resource exists. An absolute name is refused before
                        // OpenColorIO can ask the file system whether it is there.
                        const std::string named = context->resolveStringVar(file.c_str());
                        const std::string location =
                            named.empty() || named.front() == '/' ||
                                    named.find(':') != std::string::npos
                                ? std::string{}
                                : std::string(context->resolveFileLocation(file.c_str()));
                        if (canonicalConfigResourceKey(location).empty())
                            reply.missing.push_back(file);
                    } catch (const std::bad_alloc&) {
                        throw;
                    } catch (const std::exception&) {
                        reply.missing.push_back(file);
                    }
                }
                if (!reply.missing.empty()) {
                    reply.outcome = ConfigOpenOutcome::MissingResource;
                } else {
                    auto described =
                        OcioConfigAccess::describe(config, false, {}, core::Sha256Digest{},
                                                   parameters->workingColorSpaceId, true);
                    if (!described.ready()) {
                        invalid(described.invalidReason());
                    } else {
                        resolved = std::move(described).takeResolved();
                        // Bloom's synthesized input spaces are in the described config.
                        makeContext(resolved->impl().config());
                        reply.outcome = ConfigOpenOutcome::Ready;
                        auto& d = reply.description;
                        d.processColorSpaceId = resolved->processColorSpaceId();
                        d.outputColorSpaceId = resolved->outputColorSpaceId();
                        d.displayName = resolved->displayName();
                        d.viewName = resolved->viewName();
                        d.configName = resolved->configName();
                        d.colorSpaces = resolved->colorSpaces();
                        d.displays.assign(resolved->displays().begin(), resolved->displays().end());
                        d.sRgbTextureColorSpaceId = resolved->sRgbTextureColorSpaceId();
                        d.rec709VideoColorSpaceId = resolved->rec709VideoColorSpaceId();
                        d.rec2100PqColorSpaceId = resolved->rec2100PqColorSpaceId();
                        d.rec2100HlgColorSpaceId = resolved->rec2100HlgColorSpaceId();
                        d.ocioVersion = OCIO::GetVersion();
                        d.compilerId = compilerId();
                        d.compilerVersion = compilerVersion();
                        d.targetTriple = BLOOM_COLOR_OCIO_TARGET_TRIPLE;
                    }
                }
            } catch (const std::bad_alloc&) {
                return LutError::HelperMemoryLimit;
            } catch (const std::exception&) {
                invalid(OcioBuiltInInvalidReason::ValidateFailed);
                resolved.reset();
            }
        }
        if (table) {
            reply.consumedKeys = table->consumed();
            table->close();
        }
        const auto bytes = encodeConfigOpenReply(reply);
        if (!writeReply(output.get(), message[8], bytes))
            return LutError::HelperProtocolViolation;
        payload = bytes.size();
        return LutError::None;
    }

    // Receives a sealed recipe and the reply file of a build or extraction command. The reply
    // descriptor is the caller's to close.
    [[nodiscard]] std::optional<std::vector<std::byte>> recipeBytes(const LutPacket& message,
                                                                    const int socket, int& output) {
        output = -1;
        if (!resolved || !message[4] || message[4] > kConfigMaximumParameterBytes || !message[8])
            return std::nullopt;
        const auto descriptors = receiveDescriptors(socket, 2);
        const LutFd input(descriptors[0]);
        output = descriptors[1];
        if (input.get() < 0 || output < 0 ||
            !validSharedFile(input.get(), message[7], message[4], true) ||
            !validSharedFile(output, message[9], message[8], false))
            return std::nullopt;
        return readAll(input.get(), message[4]);
    }

    [[nodiscard]] LutError build(const LutPacket& message, const int socket,
                                 std::uint64_t& payload) {
        if (message[8] > kConfigMaximumReplyBytes)
            return LutError::HelperProtocolViolation;
        int outputDescriptor = -1;
        const auto bytes = recipeBytes(message, socket, outputDescriptor);
        const LutFd output(outputDescriptor);
        const auto recipe = bytes ? decodeConfigProcessorRecipe(*bytes) : std::nullopt;
        if (!recipe)
            return LutError::HelperProtocolViolation;
        ConfigProcessorReply reply;
        try {
            if (recipe->kind == ConfigProcessorKind::ColorSpace && recipe->a == recipe->b) {
                // The checks of an identity pair still apply; no token is spent on it.
                (void)processor({ConfigProcessorKind::ColorSpace, recipe->a, recipe->b},
                                reply.error);
                reply.identity = reply.error == ConfigProcessorError::None;
            } else if (processors.size() >= kConfigMaximumProcessors) {
                reply.error = ConfigProcessorError::TooManyProcessors;
            } else if (const auto built = processor(*recipe, reply.error); built) {
                const auto cpu = recipe->kind == ConfigProcessorKind::DisplayInverse
                                     ? built->getOptimizedCPUProcessor(OCIO::OPTIMIZATION_LOSSLESS)
                                     : built->getDefaultCPUProcessor();
                if (!cpu) {
                    reply.error = ConfigProcessorError::CpuUnavailable;
                } else {
                    processors.push_back(cpu);
                    reply.token = processors.size();
                    reply.cacheId = built->getCacheID();
                }
            } else if (reply.error == ConfigProcessorError::None) {
                reply.error = ConfigProcessorError::BuildFailed;
            }
        } catch (const std::bad_alloc&) {
            return LutError::HelperMemoryLimit;
        } catch (const std::exception&) {
            reply.error = ConfigProcessorError::BuildFailed;
        }
        const auto encoded = encodeConfigProcessorReply(reply);
        if (!writeReply(output.get(), message[8], encoded))
            return LutError::HelperProtocolViolation;
        payload = encoded.size();
        return LutError::None;
    }

    // A non-finite input or result is answered with payload 1 and leaves the helper serving: one
    // bad frame must not invalidate every token of the config.
    [[nodiscard]] LutError apply(const LutPacket& message, const int socket,
                                 std::uint64_t& payload) {
        if (!resolved || !message[4] || message[4] > kLutSlabBytes || message[4] % 16 != 0 ||
            message[10] == 0 || message[10] > processors.size() ||
            message[11] > static_cast<std::uint64_t>(ConfigApplyMode::Packed))
            return LutError::HelperProtocolViolation;
        const auto descriptors = receiveDescriptors(socket, 2);
        const LutFd input(descriptors[0]);
        const LutFd output(descriptors[1]);
        if (input.get() < 0 || output.get() < 0 ||
            !validSharedFile(input.get(), message[7], message[4], true) ||
            !validSharedFile(output.get(), message[9], message[4], false))
            return LutError::HelperProtocolViolation;
        const Mapping in(input.get(), message[4], PROT_READ);
        const Mapping out(output.get(), message[4], PROT_READ | PROT_WRITE);
        if (!in.valid() || !out.valid())
            return LutError::HelperMemoryLimit;
        const auto& cpu = processors[static_cast<std::size_t>(message[10] - 1)];
        try {
            const auto source = in.floats();
            const auto pixels = out.floats();
            std::ranges::copy(source, pixels.begin());
            const std::size_t count = pixels.size() / 4;
            const auto finite = [&] {
                for (std::size_t index = 0; index < count; ++index)
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        if (!std::isfinite(pixels[index * 4 + channel]))
                            return false;
                return true;
            };
            if (!finite()) {
                payload = 1;
                return LutError::None;
            }
            if (message[11] == static_cast<std::uint64_t>(ConfigApplyMode::Packed)) {
                constexpr auto floatBytes = static_cast<std::ptrdiff_t>(sizeof(float));
                OCIO::PackedImageDesc description(pixels.data(), static_cast<long>(count), 1L, 3L,
                                                  OCIO::BIT_DEPTH_F32, floatBytes, floatBytes * 4,
                                                  floatBytes * 4 *
                                                      static_cast<std::ptrdiff_t>(count));
                cpu->apply(description);
            } else {
                for (std::size_t index = 0; index < count; ++index)
                    cpu->applyRGB(pixels.data() + index * 4);
            }
            if (!finite()) {
                payload = 1;
                return LutError::None;
            }
        } catch (const std::bad_alloc&) {
            return LutError::HelperMemoryLimit;
        } catch (const std::exception&) {
            return LutError::TransformBuildFailed;
        }
        return LutError::None;
    }

    // The reply file holds a four-byte OcioGpuProgramError and then, on success, the serialized
    // program.
    [[nodiscard]] LutError extract(const LutPacket& message, const int socket,
                                   std::uint64_t& payload) {
        if (message[8] > kMaximumGpuProgramBytes || message[8] <= sizeof(std::uint32_t))
            return LutError::HelperProtocolViolation;
        int outputDescriptor = -1;
        const auto bytes = recipeBytes(message, socket, outputDescriptor);
        const LutFd output(outputDescriptor);
        const auto recipe = bytes ? decodeConfigGpuProgramRecipe(*bytes) : std::nullopt;
        if (!recipe)
            return LutError::HelperProtocolViolation;
        auto error = render::OcioGpuProgramError::TransformBuildFailed;
        std::vector<std::byte> program;
        try {
            auto processorError = ConfigProcessorError::None;
            const auto built = processor(recipe->processor, processorError);
            if (!built) {
                error = processorError == ConfigProcessorError::None
                            ? render::OcioGpuProgramError::TransformBuildFailed
                            : render::OcioGpuProgramError::UnsupportedColorSpace;
            } else {
                auto extracted = extractOcioGpuProgram(built, recipe->stage, recipe->semanticsId,
                                                       recipe->limits);
                if (!extracted.succeeded() || extracted.reflected() == nullptr) {
                    error = extracted.error == render::OcioGpuProgramError::None
                                ? render::OcioGpuProgramError::ShaderExtractionFailed
                                : extracted.error;
                } else if (auto serialized = serializeOcioGpuProgram(
                               *extracted.reflected(), message[8] - sizeof(std::uint32_t))) {
                    program = std::move(*serialized);
                    error = render::OcioGpuProgramError::None;
                } else {
                    error = render::OcioGpuProgramError::ResourceLimitExceeded;
                }
            }
        } catch (const std::bad_alloc&) {
            return LutError::HelperMemoryLimit;
        } catch (const std::exception&) {
            error = render::OcioGpuProgramError::TransformBuildFailed;
        }
        ConfigWriter writer;
        writer.u32(static_cast<std::uint32_t>(error));
        auto reply = std::move(writer).take();
        reply.insert(reply.end(), program.begin(), program.end());
        if (!writeReply(output.get(), message[8], reply))
            return LutError::HelperProtocolViolation;
        payload = reply.size();
        return LutError::None;
    }
};

ConfigWorker::ConfigWorker() : state_(std::make_unique<State>()) {}
ConfigWorker::~ConfigWorker() = default;

LutError ConfigWorker::handle(const LutPacket& message, const int descriptorSocket,
                              bool (*const confine)(), std::uint64_t& payload) {
    payload = 0;
    try {
        switch (message[2]) {
        case kConfigCommandBegin:
            return state_->begin(message, descriptorSocket, confine);
        case kConfigCommandOpen:
            return state_->open(message, descriptorSocket, payload);
        case kConfigCommandBuildProcessor:
            return state_->build(message, descriptorSocket, payload);
        case kConfigCommandApplySlab:
            return state_->apply(message, descriptorSocket, payload);
        case kConfigCommandExtractGpuProgram:
            return state_->extract(message, descriptorSocket, payload);
        default:
            return LutError::HelperProtocolViolation;
        }
    } catch (const std::bad_alloc&) {
        return LutError::HelperMemoryLimit;
    }
}

#else

struct ConfigWorker::State final {};
ConfigWorker::ConfigWorker() = default;
ConfigWorker::~ConfigWorker() = default;
LutError ConfigWorker::handle(const LutPacket&, int, bool (*)(), std::uint64_t& payload) {
    // No confinement is qualified for the config group on this target.
    payload = 0;
    return LutError::HelperUnavailable;
}

#endif

} // namespace bloom::color::detail
