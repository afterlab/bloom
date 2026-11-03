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
        mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}}, 7300, 0, 0, "");
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
