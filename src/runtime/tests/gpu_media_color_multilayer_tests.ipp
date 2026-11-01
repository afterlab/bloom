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
