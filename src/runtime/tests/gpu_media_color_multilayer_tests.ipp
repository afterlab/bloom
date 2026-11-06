// ADR 0026 Q1 (#402) on the native route: a multilayer EXR whose selected layer has no color
// channels publishes nothing, so the frame runs on the device (no CPU fallback) and matches the
// CPU reference. Selecting `Composite.Combined` runs the raw upload plus the GPU OCIO command.
// Included by gpu_media_color_tests.cpp inside its anonymous namespace.

void testMultilayerNoColourLayerNative(Expectations& expectations,
                                       bloom::render::GpuDevice& device,
                                       const GpuSceneOcioContext& ocioContext) {
    const auto directory = std::filesystem::temp_directory_path() / "bloom_gpu_media_multilayer";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "multilayer.exr";
    writeMultilayerExr(path, 3, 2, signedHdrPixels(), acesAp0Chromaticities());
    const auto asset = imageAsset(path, "multilayer", 7300);
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(directory);
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(),
                        "multilayer: cache and executor host");
    if (!cache || !executor) {
        std::filesystem::remove_all(directory);
        return;
    }
    const auto context = deterministicMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);

    const auto root =
        mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}}, 7300, 0, 0, "Depth");
    const auto request = acesRequest(*root, kAcesWorking);
    const auto prepared = builder.build(root, request);
    expectations.expect(prepared.hasValue(),
                        "multilayer: a data-only selection prepares on the GPU route" +
                            (prepared ? std::string{} : " (" + prepared.diagnostic.message + ")"));
    if (prepared) {
        expectations.expect(prepared.scene->mediaStatistics().imageNoColourLayerSources == 1 &&
                                firstUpload(*prepared.scene) == nullptr &&
                                firstOcio(*prepared.scene) == nullptr,
                            "multilayer: the data-only source publishes no upload or color pass");
        const auto before = executor.executor->counters();
        const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
        expectations.expect(run.ready && run.image != nullptr,
                            "multilayer: the executor reaches Ready on the device");
        expectations.expect(executor.executor->counters().dispatches >
                                before.dispatches &&
                                executor.executor->counters().uploads == before.uploads,
                            "multilayer: the frame is native dispatches, not a CPU frame");
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto frame = evaluator.evaluate(root, oracleRequest, {});
        expectations.expect(frame.frame() != nullptr, "multilayer: the CPU oracle evaluates");
        if (run.ready && run.image != nullptr && frame.frame() != nullptr) {
            expectations.expect(prepared.scene->processIdentity() == frame.frame()->identity() &&
                                    descriptorMatchesScene(*run.image, *prepared.scene),
                                "multilayer: identity and descriptor match the CPU frame");
            const auto readback =
                bloom::render::readbackResidentImage(*run.image, kReadbackBudget);
            const auto& cpu = frame.frame()->processImage();
            expectations.expect(readback.hasValue() && readback.pixels.size() ==
                                                           cpu.pixels().size() &&
                                    pixelsClose(readback.pixels, cpu.pixels()),
                                "multilayer: the native frame matches the CPU reference");
        }
    }

    const auto colour = mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}}, 7400,
                                  0, 0, "Composite.Combined");
    expectations.expect(expectMediaParity(expectations, *executor.executor, evaluator, builder,
                                          colour, acesRequest(*colour, kAcesWorking), 3, 2,
                                          "multilayer Composite.Combined"),
                        "multilayer: the selected color layer runs natively with parity");
    std::filesystem::remove_all(directory);
}

// #386: footage used by several layers is prepared once per frame. Two layers of one ACES AP0
// still and a third layer of another still build one raw upload and one GPU OCIO conversion per
// distinct source, and both layers of the shared source consume that one converted image; under a
// fractional proxy the point gather is shared the same way. On the device the cold frame runs one
// upload and one colour conversion per source and matches the CPU reference, which still evaluates
// every layer's source on its own.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
sharedFootagePlan(const CompositionFormat compositionFormat, const document::AssetRecord& shared,
                  const document::AssetRecord& other, const std::uint64_t idBase) {
    const auto parameter = [](const std::uint64_t raw) {
        return bloom::document::ParameterId::fromRaw(raw);
    };
    const std::array<const document::AssetRecord*, 3> assets{&shared, &shared, &other};
    const std::array<LayerValues, 3> values{
        LayerValues{.position = {4.3, 3.1}, .opacity = 0.9},
        LayerValues{.position = {1.0, 2.0}, .scale = {1.5, 1.25}, .rotation = 12.0},
        LayerValues{.position = {6.0, 1.0}, .opacity = 0.75}};
    std::vector<CompiledOperation> operations;
    std::vector<CompiledMergeInput> inputs;
    for (std::uint64_t layer = 0; layer < assets.size(); ++layer) {
        const auto base = idBase + layer * 20;
        const LayerIds ids{parameter(base + 0), parameter(base + 1), parameter(base + 2),
                           parameter(base + 3), parameter(base + 4), parameter(base + 5)};
        const auto source = OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size()));
        operations.emplace_back(bloom::runtime::CompiledImageSource{
            bloom::document::NodeId::fromRaw(base + 10), *assets[layer], 0, 0, 0, std::string{},
            false});
        const auto output = OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size()));
        operations.emplace_back(layerOutput(bloom::document::NodeId::fromRaw(base + 11),
                                            bloom::document::LayerId::fromRaw(base + 12), source,
                                            ids, values[layer]));
        inputs.push_back(CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(base + 13),
                                            bloom::document::LayerId::fromRaw(base + 12), output});
    }
    const auto merge = OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size()));
    operations.emplace_back(
        CompiledMerge{bloom::document::NodeId::fromRaw(idBase + 90), std::move(inputs)});
    const auto output = OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size()));
    operations.emplace_back(
        CompiledCompositionOutput{bloom::document::NodeId::fromRaw(idBase + 91), merge});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), output});
}

template <typename Command>
[[nodiscard]] std::vector<const Command*> commandsOf(const PreparedGpuScene& scene) {
    std::vector<const Command*> found;
    for (const auto& command : scene.commands()) {
        if (const auto* typed = std::get_if<Command>(&command))
            found.push_back(typed);
    }
    return found;
}

// Every command of one kind has its own key: none is an equal copy of another.
template <typename Command>
[[nodiscard]] bool keysDistinct(const std::vector<const Command*>& commands) {
    std::set<std::string> keys;
    for (const auto* command : commands)
        keys.insert(command->semanticKey);
    return keys.size() == commands.size();
}

void testSharedFootagePreparedOnce(Expectations& expectations, bloom::render::GpuDevice* device,
                                   const CpuCompositionEvaluator& evaluator, const Fixture& fixture,
                                   const GpuSceneOcioContext& ocioContext) {
    auto otherPixels = signedHdrPixels();
    std::reverse(otherPixels.begin(), otherPixels.end());
    const auto otherPath = fixture.directory / "aces_ap0_other.exr";
    writeExrRgbaWithChromaticities(otherPath, 3, 2, otherPixels, acesAp0Chromaticities());
    const auto other = imageAsset(otherPath, "aces_ap0_other", 7600);
    const auto context = deterministicMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan = sharedFootagePlan(format(9, 6), fixture.asset, other, 7700);

    const auto full = acesRequest(*plan, kAcesWorking);
    const auto prepared = builder.build(plan, full);
    expectations.expect(prepared.hasValue(),
                        "shared footage: prepares" +
                            (prepared ? std::string{} : " (" + prepared.diagnostic.message + ")"));
    if (prepared) {
        const auto uploads = commandsOf<GpuSceneUploadCommand>(*prepared.scene);
        const auto conversions = commandsOf<GpuSceneOcioEffectCommand>(*prepared.scene);
        expectations.expect(uploads.size() == 2 && keysDistinct(uploads),
                            "shared footage: one upload per distinct source");
        expectations.expect(conversions.size() == 2 && keysDistinct(conversions),
                            "shared footage: one colour conversion per distinct source");
        // The layer placement commands that read each conversion.
        std::map<bloom::runtime::GpuSceneCommandIndex, int> consumers;
        for (const auto& command : prepared.scene->commands()) {
            if (const auto* affine = std::get_if<bloom::runtime::GpuSceneAffineCommand>(&command))
                ++consumers[affine->input];
            if (const auto* translation =
                    std::get_if<bloom::runtime::GpuSceneTranslationCommand>(&command))
                ++consumers[translation->input];
        }
        const int first = conversions.size() == 2 ? consumers[conversions[0]->index] : 0;
        const int second = conversions.size() == 2 ? consumers[conversions[1]->index] : 0;
        const bool sharedTwice = first + second == 3 && (first == 2 || second == 2);
        expectations.expect(sharedTwice,
                            "shared footage: both layers of one source consume one conversion");
    }

    const auto extent = bloom::render::ImageExtent::create(6, 4);
    auto proxy = full;
    if (extent)
        proxy.resolution = ProxyResolution{*extent.value()};
    const auto proxied = builder.build(plan, proxy);
    expectations.expect(proxied.hasValue(), "shared footage: the proxy prepares");
    if (proxied) {
        const auto uploads = commandsOf<GpuSceneUploadCommand>(*proxied.scene);
        const auto gathers =
            commandsOf<bloom::runtime::GpuScenePointResampleCommand>(*proxied.scene);
        const auto conversions = commandsOf<GpuSceneOcioEffectCommand>(*proxied.scene);
        expectations.expect(uploads.size() == 2 && gathers.size() == 2 &&
                                conversions.size() == 2 && keysDistinct(gathers) &&
                                keysDistinct(conversions),
                            "shared footage: the proxy gathers and converts each source once");
    }

    if (device == nullptr)
        return;
    const std::array<const bloom::runtime::EvaluationRequest*, 2> requests{&full, &proxy};
    for (const auto* request : requests) {
        const std::string label =
            request == &full ? "shared footage full" : "shared footage proxy";
        auto cache = GpuSceneCache::create(*device, GpuSceneCacheBudgets{kCacheBudget});
        auto executor = GpuSceneExecutor::create(*device, *cache.cache, GpuSceneExecutorBudgets{});
        expectations.expect(cache.hasValue() && executor.hasValue(),
                            label + ": cache and executor host");
        if (!cache || !executor)
            return;
        const auto scene = builder.build(plan, *request);
        if (!scene)
            continue;
        const auto run = runScene(*executor.executor, scene.scene, kSceneBudget);
        const auto counters = executor.executor->counters();
        expectations.expect(run.ready && run.image != nullptr,
                            label + ": the executor reaches Ready on the device");
        expectations.expect(counters.uploads == 2 && counters.ocioEffectDispatches == 2,
                            label + ": the cold frame uploads and converts each source once");
        auto oracleRequest = *request;
        oracleRequest.bypassOperationCache = true;
        const auto frame = evaluator.evaluate(plan, oracleRequest, {});
        expectations.expect(frame.frame() != nullptr, label + ": the CPU oracle evaluates");
        if (!run.ready || run.image == nullptr || frame.frame() == nullptr)
            continue;
        expectations.expect(scene.scene->processIdentity() == frame.frame()->identity() &&
                                descriptorMatchesScene(*run.image, *scene.scene),
                            label + ": identity and descriptor match the CPU frame");
        const auto readback = bloom::render::readbackResidentImage(*run.image, kReadbackBudget);
        const auto& cpu = frame.frame()->processImage();
        expectations.expect(readback.hasValue() && readback.pixels.size() == cpu.pixels().size() &&
                                pixelsClose(readback.pixels, cpu.pixels()),
                            label + ": the native frame matches the CPU reference");
    }
}

// #386: an all-HALF OpenEXR sequence member's native samples are decoded by the read-ahead before
// the frame that uses them. That frame neither reads nor decodes its member, and uploads exactly
// the samples (same backing identity, same bytes) a frame decoding its own member uploads.
void writeHalfExrMember(const std::filesystem::path& path, const float shift) {
    auto pixels = signedHdrPixels();
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(2, 1));
    Imf::Header header(window, window);
    header.insert("chromaticities", Imf::ChromaticitiesAttribute(Imf::Chromaticities()));
    header.insert("alphaAssociation", Imf::StringAttribute("premultiplied"));
    std::array<std::vector<Imath::half>, 4> lanes;
    Imf::FrameBuffer frameBuffer;
    constexpr std::array<const char*, 4> kNames{"R", "G", "B", "A"};
    for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
        header.channels().insert(kNames[lane], Imf::Channel(Imf::HALF));
        for (const auto& pixel : pixels)
            lanes[lane].emplace_back(lane == 0   ? pixel.red + shift
                                     : lane == 1 ? pixel.green
                                     : lane == 2 ? pixel.blue
                                                 : pixel.alpha);
        frameBuffer.insert(kNames[lane],
                           Imf::Slice(Imf::HALF, reinterpret_cast<char*>(lanes[lane].data()),
                                      sizeof(Imath::half), 3 * sizeof(Imath::half)));
    }
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(2);
}

void testSequenceDecodeAhead(Expectations& expectations, const Fixture& fixture,
                             const GpuSceneOcioContext& ocioContext) {
    std::vector<document::AssetSequenceMember> members;
    for (int frame = 0; frame < 4; ++frame) {
        const auto path = fixture.directory / ("decode_ahead." + std::to_string(frame) + ".exr");
        writeHalfExrMember(path, static_cast<float>(frame));
        members.push_back(sequenceMember(path, frame, static_cast<std::uint64_t>(frame) + 40));
    }
    document::AssetRecord asset = fixture.asset;
    asset.id = bloom::document::AssetId::fromRaw(931);
    asset.kind = document::AssetKind::Sequence;
    asset.manifest.pattern = "decode_ahead.#.exr";
    asset.manifest.padding = 1;
    asset.manifest.first = 0;
    asset.manifest.last = 3;
    asset.manifest.members = members;
    const auto plan = mediaPlan(format(8, 8), asset, LayerValues{}, 4600);
    const auto frameRequest = [&](const int frame) {
        auto request = acesRequest(*plan, kAcesWorking);
        request.time = rationalTime(frame, 24);
        request.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Revalidate;
        request.requireVerifiedMediaSources = true;
        return request;
    };
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.directory);
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto first = builder.build(plan, frameRequest(0));
    expectations.expect(first.hasValue() && first.scene->mediaStatistics().imageConversions == 1 &&
                            first.scene->mediaStatistics().imageDecodedAhead == 0,
                        "decode ahead: frame 0 decodes its own member");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (context.sequenceReadAhead->statistics().decoded +
                   context.sequenceReadAhead->statistics().failed <
               3 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto statistics = context.sequenceReadAhead->statistics();
    expectations.expect(statistics.captured == 3 && statistics.decoded == 3 &&
                            statistics.decodesSkipped == 0,
                        "decode ahead: the three following members are captured and decoded");
    // The oracle: an evaluator without read-ahead, whose frames decode their own members.
    CpuCompositionEvaluator reference;
    reference.setAssetBaseDirectory(fixture.directory);
    const CpuGpuSceneBuilder referenceBuilder(nullptr, deterministicMediaContext(reference),
                                              ocioContext);
    for (int frame = 1; frame < 4; ++frame) {
        const auto label = "decode ahead frame " + std::to_string(frame);
        const auto built = builder.build(plan, frameRequest(frame));
        const auto oracle = referenceBuilder.build(plan, frameRequest(frame));
        expectations.expect(built.hasValue() && oracle.hasValue(), label + ": both prepare");
        if (!built || !oracle)
            continue;
        const auto& media = built.scene->mediaStatistics();
        expectations.expect(media.imageDecodedAhead == 1 && media.imageConversions == 0 &&
                                media.imageDecodeNanoseconds == 0 && media.sourceBytesRead == 0,
                            label + ": the member was read and decoded ahead of the frame");
        const auto* upload = firstUpload(*built.scene);
        const auto* oracleUpload = firstUpload(*oracle.scene);
        const bool samples = upload != nullptr && oracleUpload != nullptr && upload->samples &&
                             oracleUpload->samples;
        expectations.expect(samples && upload->semanticKey == oracleUpload->semanticKey,
                            label + ": the upload keeps the identity of a frame's own decode");
        if (!samples)
            continue;
        const auto planes = upload->samples->planes();
        const auto oraclePlanes = oracleUpload->samples->planes();
        bool equal = planes.size() == oraclePlanes.size();
        for (std::size_t index = 0; equal && index < planes.size(); ++index)
            equal = std::ranges::equal(planes[index].bytes, oraclePlanes[index].bytes);
        expectations.expect(equal, label + ": the decoded-ahead samples are the frame's samples");
    }
    // The transport stands at frame 10: a member for frame 9 (passed) or frame 19 (beyond the
    // read-ahead window after a jump) is captured but not decoded; frame 11's member is decoded.
    using bloom::runtime::ImageSequenceReadAhead;
    using bloom::runtime::SourceUse;
    using bloom::runtime::SourceWorkingSet;
    const auto cache = std::make_shared<bloom::runtime::OperationCache>(std::size_t{1} << 30);
    ImageSequenceReadAhead readAhead(1);
    std::vector<ImageSequenceReadAhead::Request> requests;
    const std::array<std::int64_t, 3> useFrames{9, 19, 11};
    bloom::media::ImageInterpretation raw;
    raw.colorSpace = bloom::media::ImageColorSpace::Raw;
    raw.alphaAssociation = bloom::media::ImageAlphaAssociation::Premultiplied;
    for (std::size_t index = 0; index < useFrames.size(); ++index) {
        const auto& member = members[index + 1];
        requests.push_back({.path = fixture.directory / member.locator.path,
                            .digest = bloom::document::sequenceMemberIdentity(member),
                            .stamp = std::nullopt,
                            .recordedDigest = member.contentDigest,
                            .revisionKey = "transport-" + std::to_string(index),
                            .byteBudget = std::size_t{1} << 30,
                            .use = {.composition = 1, .frame = useFrames[index], .tracked = true},
                            .decode = raw,
                            .channels = {}});
    }
    {
        const SourceWorkingSet::UseScope transport(
            SourceUse{.composition = 1, .frame = 10, .tracked = true});
        readAhead.request(cache, std::move(requests));
    }
    const auto transportDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (readAhead.statistics().captured + readAhead.statistics().failed < 3 &&
           std::chrono::steady_clock::now() < transportDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto transport = readAhead.statistics();
    expectations.expect(transport.captured == 3 && transport.decoded == 1 &&
                            transport.decodesSkipped == 2,
                        "decode ahead: frames the transport passed or jumped away from are not "
                        "decoded");
}

// #549: a frame that reads an OpenEXR part's colour layer and other channels of it (a Shuffle's
// depth channel here, a Cryptomatte's ranks in a render) reads the part once. The colour decode
// reads the channels beside its layer whichever leaf the plan prepares first, a later build of the
// frame takes them from the operation cache, and the read-ahead decodes them with each later
// member's samples. The gathered channels are the ones a build without the shared read gathers.
void writeHalfExrMemberWithDepth(const std::filesystem::path& path, const float shift) {
    auto pixels = signedHdrPixels();
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(2, 1));
    Imf::Header header(window, window);
    header.insert("chromaticities", Imf::ChromaticitiesAttribute(Imf::Chromaticities()));
    header.insert("alphaAssociation", Imf::StringAttribute("premultiplied"));
    std::array<std::vector<Imath::half>, 4> lanes;
    std::vector<float> depth;
    Imf::FrameBuffer frameBuffer;
    constexpr std::array<const char*, 4> kNames{"R", "G", "B", "A"};
    for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
        header.channels().insert(kNames[lane], Imf::Channel(Imf::HALF));
        for (const auto& pixel : pixels)
            lanes[lane].emplace_back(lane == 0   ? pixel.red
                                     : lane == 1 ? pixel.green
                                     : lane == 2 ? pixel.blue
                                                 : pixel.alpha);
        frameBuffer.insert(kNames[lane],
                           Imf::Slice(Imf::HALF, reinterpret_cast<char*>(lanes[lane].data()),
                                      sizeof(Imath::half), 3 * sizeof(Imath::half)));
    }
    for (std::size_t index = 0; index < pixels.size(); ++index)
        depth.push_back(shift + 0.5F * static_cast<float>(index));
    header.channels().insert("depth.Z", Imf::Channel(Imf::FLOAT));
    frameBuffer.insert("depth.Z", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(depth.data()),
                                             sizeof(float), 3 * sizeof(float)));
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(2);
}

// The Image and a Shuffle of its depth channel, each on its own layer; `shuffleFirst` puts the
// Shuffle first in the plan, so it is prepared before the Image.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
imageAndDepthPlan(const document::AssetRecord& asset, const std::uint64_t idBase,
                  const bool shuffleFirst) {
    const auto parameter = [](const std::uint64_t raw) {
        return bloom::document::ParameterId::fromRaw(raw);
    };
    const auto node = bloom::document::NodeId::fromRaw(idBase + 1);
    bloom::runtime::CompiledChannelShuffle shuffle;
    shuffle.sourceNodeId = bloom::document::NodeId::fromRaw(idBase + 2);
    shuffle.channels.sources.push_back(
        bloom::runtime::CompiledChannelSource{node, asset, 0, 0, 0, {}, {}});
    bloom::runtime::CompiledChannelStep step;
    step.kind = bloom::runtime::CompiledChannelStepKind::Source;
    step.nodeId = node;
    shuffle.channels.steps.push_back(step);
    shuffle.mapping = {"depth.Z", "depth.Z", "depth.Z", "1"};
    shuffle.mode = document::kChannelShuffleRawMode;
    shuffle.associated = false;
    std::vector<CompiledOperation> operations;
    std::vector<CompiledMergeInput> inputs;
    for (std::uint64_t layer = 0; layer < 2; ++layer) {
        const bool depthLayer = (layer == 0) == shuffleFirst;
        const auto base = idBase + 10 + layer * 20;
        const LayerIds ids{parameter(base + 0), parameter(base + 1), parameter(base + 2),
                           parameter(base + 3), parameter(base + 4), parameter(base + 5)};
        const auto source = OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size()));
        if (depthLayer)
            operations.emplace_back(shuffle);
        else
            operations.emplace_back(
                bloom::runtime::CompiledImageSource{node, asset, 0, 0, 0, std::string{}, false});
        const auto output = OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size()));
        operations.emplace_back(layerOutput(bloom::document::NodeId::fromRaw(base + 11),
                                            bloom::document::LayerId::fromRaw(base + 12), source,
                                            ids, LayerValues{}));
        inputs.push_back(CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(base + 13),
                                            bloom::document::LayerId::fromRaw(base + 12), output});
    }
    const auto merge = OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size()));
    operations.emplace_back(
        CompiledMerge{bloom::document::NodeId::fromRaw(idBase + 90), std::move(inputs)});
    const auto output = OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size()));
    operations.emplace_back(
        CompiledCompositionOutput{bloom::document::NodeId::fromRaw(idBase + 91), merge});
    return publish(CompiledCompositionPlanDefinition{bloom::document::Revision::fromRaw(7),
                                                     kProjectId, kCompositionId, format(8, 8),
                                                     std::move(operations), output});
}

// The Shuffle's gathered RGBA upload (the Image uploads native samples).
[[nodiscard]] const GpuSceneUploadCommand* shuffleUpload(const PreparedGpuScene& scene) {
    for (const auto* upload : commandsOf<GpuSceneUploadCommand>(scene))
        if (upload->image != nullptr)
            return upload;
    return nullptr;
}

void testChannelReadsShareColourDecode(Expectations& expectations, const Fixture& fixture,
                                       const GpuSceneOcioContext& ocioContext) {
    std::vector<document::AssetSequenceMember> members;
    for (int frame = 0; frame < 4; ++frame) {
        const auto path = fixture.directory / ("depth_read." + std::to_string(frame) + ".exr");
        writeHalfExrMemberWithDepth(path, static_cast<float>(frame));
        members.push_back(sequenceMember(path, frame, static_cast<std::uint64_t>(frame) + 60));
    }
    document::AssetRecord asset = fixture.asset;
    asset.id = bloom::document::AssetId::fromRaw(941);
    asset.kind = document::AssetKind::Sequence;
    asset.manifest.pattern = "depth_read.#.exr";
    asset.manifest.padding = 1;
    asset.manifest.first = 0;
    asset.manifest.last = 3;
    asset.manifest.members = members;
    CpuCompositionEvaluator reference;
    reference.setAssetBaseDirectory(fixture.directory);
    const CpuGpuSceneBuilder referenceBuilder(nullptr, deterministicMediaContext(reference),
                                              ocioContext);
    for (const bool shuffleFirst : {false, true}) {
        const std::string order = shuffleFirst ? "channels first" : "image first";
        const auto plan = imageAndDepthPlan(asset, shuffleFirst ? 4900 : 4800, shuffleFirst);
        const auto frameRequest = [&](const int frame) {
            auto request = acesRequest(*plan, kAcesWorking);
            request.time = rationalTime(frame, 24);
            return request;
        };
        // The oracle gathers the depth channel on its own; the scenes must upload the same bytes.
        const auto sameGather = [&](const PreparedGpuScene& scene, const int frame) {
            const auto oracle = referenceBuilder.build(plan, frameRequest(frame));
            const auto* upload = shuffleUpload(scene);
            const auto* oracleUpload = oracle ? shuffleUpload(*oracle.scene) : nullptr;
            return upload != nullptr && oracleUpload != nullptr &&
                   std::ranges::equal(upload->image->pixels(), oracleUpload->image->pixels());
        };
        CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(fixture.directory);
        const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
        const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
        const auto first = builder.build(plan, frameRequest(0));
        expectations.expect(first.hasValue(), "shared channel read, " + order + ": prepares" +
                                                  (first ? std::string{}
                                                         : " (" + first.diagnostic.message + ")"));
        if (!first)
            continue;
        const auto& cold = first.scene->mediaStatistics();
        expectations.expect(cold.channelSetsReadWithColour == 1 && cold.channelSetDecodes == 0,
                            "shared channel read, " + order +
                                ": the depth channel is read in the colour layer's decode");
        expectations.expect(first.scene->mediaStatistics().imageSources == 2 &&
                                commandsOf<GpuSceneUploadCommand>(*first.scene).size() == 2 &&
                                sameGather(*first.scene, 0),
                            "shared channel read, " + order +
                                ": both leaves upload, the gather equal to its own decode's");
        const auto again = builder.build(plan, frameRequest(0));
        expectations.expect(again.hasValue() &&
                                again.scene->mediaStatistics().channelSetsReused == 1 &&
                                again.scene->mediaStatistics().channelSetDecodes == 0 &&
                                again.scene->mediaStatistics().channelSetsReadWithColour == 0,
                            "shared channel read, " + order +
                                ": a later build takes the channels from the operation cache");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (context.sequenceReadAhead->statistics().channelsDecoded < 3 &&
               context.sequenceReadAhead->statistics().failed == 0 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        expectations.expect(context.sequenceReadAhead->statistics().channelsDecoded == 3 &&
                                context.sequenceReadAhead->statistics().decoded == 3,
                            "shared channel read, " + order +
                                ": the read-ahead decodes each later member's channels");
        for (int frame = 1; frame < 4; ++frame) {
            const auto label = "shared channel read, " + order + ", frame " + std::to_string(frame);
            const auto built = builder.build(plan, frameRequest(frame));
            expectations.expect(built.hasValue(), label + ": prepares");
            if (!built)
                continue;
            const auto& media = built.scene->mediaStatistics();
            expectations.expect(media.imageDecodedAhead == 1 && media.channelSetsReused == 1 &&
                                    media.channelSetDecodes == 0 &&
                                    media.channelSetsReadWithColour == 0 &&
                                    media.imageDecodeNanoseconds == 0,
                                label + ": the samples and channels were decoded ahead");
            expectations.expect(sameGather(*built.scene, frame),
                                label + ": the gather equals its own decode's");
        }
    }
}
