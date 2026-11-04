// Cache-identity group: warm rebuilds (staging is scratch, ADR 0037), changed frame/bypass, the
// source-key identity contract, per-request statistics, and the gesture cache never touching disk.
// Included by gpu_media_scene_preparation_tests.cpp inside its anonymous namespace.

void testWarmReuseAndChangedSource(Expectations& expectations,
                                   const CpuCompositionEvaluator& evaluator,
                                   const MediaFixture& fixture) {
    auto context = deterministicMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto planA = mediaPlan(format(8, 8), fixture.asset,
                                 LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 1500);
    const auto planB = mediaPlan(format(8, 8), fixture.asset,
                                 LayerValues{.position = {2.7, 2.4}, .opacity = 1.0}, 1600);
    const auto a = builder.build(planA, requestFor(*planA));
    const auto b = builder.build(planB, requestFor(*planB));
    expectations.expect(a.hasValue() && b.hasValue(), "both media builds prepare");
    if (!a || !b) {
        return;
    }
    const auto* uploadA = firstUpload(*a.scene);
    const auto* uploadB = firstUpload(*b.scene);
    const auto* translationA = firstTranslation(*a.scene);
    const auto* translationB = firstTranslation(*b.scene);
    expectations.expect(uploadA != nullptr && uploadB != nullptr && translationA != nullptr &&
                            translationB != nullptr,
                        "both builds emit an upload and a translation command");
    if (uploadA == nullptr || uploadB == nullptr || translationA == nullptr ||
        translationB == nullptr) {
        return;
    }
    expectations.expect(uploadA->semanticKey == uploadB->semanticKey,
                        "an unchanged source keeps one upload semantic key across transforms");
    expectations.expect(translationA->semanticKey != translationB->semanticKey,
                        "a changed transform has a different translation pixel key");
    expectations.expect(b.scene->mediaStatistics().imageConversions == 1,
                        "the warm build converts again: no host copy of an upload is kept");
}

void testChangedFrameAndBypass(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                               const MediaFixture& fixture) {
    auto secondPixels = signedHdrPixels();
    secondPixels.front().green += 1.0F;
    const auto secondPath = fixture.directory / "bypass_second.exr";
    writeExrRgba(secondPath, 3, 2, secondPixels);
    document::AssetRecord asset = fixture.asset;
    asset.id = bloom::document::AssetId::fromRaw(902);
    asset.kind = document::AssetKind::Sequence;
    asset.manifest.pattern = "bypass_sequence.####.exr";
    asset.manifest.padding = 4;
    asset.manifest.first = 0;
    asset.manifest.last = 1;
    asset.manifest.members = {sequenceMember(fixture.path, 0, 0), sequenceMember(secondPath, 1, 1)};
    const auto plan = mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}}, 1700);

    auto context = deterministicMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto frameZero = builder.build(plan, requestFor(*plan, RationalTime::fromInteger(0)));
    const auto frameOne = builder.build(plan, requestFor(*plan, rationalTime(1, 24)));
    expectations.expect(frameZero.hasValue() && frameOne.hasValue(),
                        "both sequence frames prepare");
    if (!frameZero || !frameOne) {
        return;
    }
    const auto* uploadZero = firstUpload(*frameZero.scene);
    const auto* uploadOne = firstUpload(*frameOne.scene);
    expectations.expect(uploadZero != nullptr && uploadOne != nullptr &&
                            uploadZero->semanticKey != uploadOne->semanticKey,
                        "a changed selected frame changes the upload semantic key");
    expectations.expect(uploadZero->image.get() != uploadOne->image.get(),
                        "a changed selected frame converts a distinct source image");

    // Explicit bypass recalculates the source.
    auto bypassRequest = requestFor(*plan, RationalTime::fromInteger(0));
    bypassRequest.bypassOperationCache = true;
    const auto bypassed = builder.build(plan, bypassRequest);
    expectations.expect(bypassed.hasValue(), "the bypass build prepares");
    if (!bypassed) {
        return;
    }
    const auto* uploadBypass = firstUpload(*bypassed.scene);
    expectations.expect(bypassed.scene->mediaStatistics().imageConversions == 1,
                        "an explicit bypass reconverts");
    expectations.expect(uploadBypass != nullptr && uploadZero != nullptr &&
                            uploadBypass->image.get() != uploadZero->image.get(),
                        "an explicit bypass recalculates a distinct source image");
}

// The source key is an identity of the resolved pixels, not of the plan: node/layer/parameter ids
// and the document revision must not enter it, while the proxy/composition descriptor must.
void testSourceKeyExcludesIdsAndRevision(Expectations& expectations,
                                         const CpuCompositionEvaluator& evaluator,
                                         const MediaFixture& fixture) {
    auto context = deterministicMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto a = mediaPlan(format(8, 8), fixture.asset,
                             LayerValues{.position = {4.3, 3.1}, .opacity = 0.8}, 2700);
    const auto b = mediaPlan(format(8, 8), fixture.asset,
                             LayerValues{.position = {4.3, 3.1}, .opacity = 0.8}, 2800);
    auto definition = a->copyDefinition();
    definition.sourceRevision = bloom::document::Revision::fromRaw(999);
    const auto revised = publish(std::move(definition));

    const auto builtA = builder.build(a, requestFor(*a));
    const auto builtB = builder.build(b, requestFor(*b));
    const auto builtRevised = builder.build(revised, requestFor(*revised));
    expectations.expect(builtA.hasValue() && builtB.hasValue() && builtRevised.hasValue(),
                        "the identity-key builds prepare");
    if (!builtA || !builtB || !builtRevised) {
        return;
    }
    const auto* uploadA = firstUpload(*builtA.scene);
    const auto* uploadB = firstUpload(*builtB.scene);
    const auto* uploadRevised = firstUpload(*builtRevised.scene);
    const auto* translationA = firstTranslation(*builtA.scene);
    const auto* translationB = firstTranslation(*builtB.scene);
    const auto* translationRevised = firstTranslation(*builtRevised.scene);
    expectations.expect(uploadA != nullptr && uploadB != nullptr && uploadRevised != nullptr &&
                            translationA != nullptr && translationB != nullptr &&
                            translationRevised != nullptr,
                        "the identity-key builds emit uploads and translations");
    if (uploadA == nullptr || uploadB == nullptr || uploadRevised == nullptr ||
        translationA == nullptr || translationB == nullptr || translationRevised == nullptr) {
        return;
    }
    expectations.expect(uploadA->semanticKey == uploadB->semanticKey &&
                            uploadA->semanticKey == uploadRevised->semanticKey,
                        "node/layer/parameter ids and the revision never enter the source key");
    expectations.expect(translationA->semanticKey == translationB->semanticKey &&
                            translationA->semanticKey == translationRevised->semanticKey,
                        "node/layer/parameter ids and the revision never enter a derived key");

    // The proxy scale IS part of the converted source identity.
    const auto proxyPlan = mediaPlan(format(8, 8), fixture.asset,
                                     LayerValues{.position = {4.3, 3.1}, .opacity = 0.8}, 2900);
    const auto extent = bloom::render::ImageExtent::create(5, 4);
    expectations.expect(static_cast<bool>(extent), "the proxy extent builds");
    if (!extent) {
        return;
    }
    auto proxyRequest = requestFor(*proxyPlan);
    proxyRequest.resolution = ProxyResolution{*extent.value()};
    const auto proxied = builder.build(proxyPlan, proxyRequest);
    expectations.expect(proxied.hasValue(), "the proxy build prepares");
    if (!proxied) {
        return;
    }
    const auto* uploadProxy = firstUpload(*proxied.scene);
    expectations.expect(uploadProxy != nullptr &&
                            uploadProxy->semanticKey != uploadA->semanticKey &&
                            proxied.scene->mediaStatistics().imageConversions == 1,
                        "a changed proxy scale changes the source key and converts");
}

// Per-build CPU work is reported on the result, never in a shared mutable aggregate.
void testPerRequestStatisticsAreLocal(Expectations& expectations,
                                      const CpuCompositionEvaluator& evaluator,
                                      const MediaFixture& fixture) {
    auto context = deterministicMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 3000);
    const auto first = builder.build(plan, requestFor(*plan));
    const auto second = builder.build(plan, requestFor(*plan));
    expectations.expect(first.hasValue() && second.hasValue(), "both per-request builds prepare");
    if (!first || !second) {
        return;
    }
    expectations.expect(first.scene->mediaStatistics().imageSources == 1 &&
                            first.scene->mediaStatistics().imageConversions == 1,
                        "the first result reports only its own work");
    expectations.expect(second.scene->mediaStatistics().imageSources == 1 &&
                            second.scene->mediaStatistics().imageConversions == 1,
                        "the second result reports only its own work, not a running total");
}

// Explicit bypass and the interactive (overridden) plan bypass must not touch the real disk cache.
void testGestureCacheNeverTouchesDisk(Expectations& expectations,
                                      const CpuCompositionEvaluator& evaluator,
                                      const MediaFixture& fixture) {
    const auto diskRoot = fixture.directory / "gesture_disk_cache";
    auto disk = std::make_shared<bloom::media::cache::MediaDiskCache>(
        bloom::media::cache::MediaDiskCacheConfig{.rootDirectory = diskRoot,
                                                  .byteBudget = std::size_t{1} << 24U});
    evaluator.setMediaDiskCache(disk);
    auto context = deterministicMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 3100);
    const auto warm = builder.build(plan, requestFor(*plan));
    expectations.expect(warm.hasValue(), "the disk-cache warm build prepares");
    disk->flush();
    const auto before = disk->statistics();

    auto interactiveDefinition = plan->copyDefinition();
    interactiveDefinition.bypassOperationCache = true;
    const auto interactive = publish(std::move(interactiveDefinition));
    const auto gestureHit = builder.build(interactive, requestFor(*interactive));
    disk->flush();
    const auto afterHit = disk->statistics();
    expectations.expect(gestureHit.hasValue() &&
                            gestureHit.scene->mediaStatistics().imageConversions == 1,
                        "a gesture build of a decoded source converts directly");
    expectations.expect(afterHit.hits == before.hits && afterHit.misses == before.misses &&
                            afterHit.entryCount == before.entryCount &&
                            afterHit.storedBytes == before.storedBytes,
                        "a gesture hit never reads or writes the disk cache");

    auto secondPixels = signedHdrPixels();
    secondPixels.front().red += 0.75F;
    const auto secondPath = fixture.directory / "gesture_second.exr";
    writeExrRgba(secondPath, 3, 2, secondPixels);
    const auto secondAsset = imageAsset(secondPath, "gesture_second", 904);
    const auto missPlan =
        mediaPlan(format(8, 8), secondAsset, LayerValues{.position = {4.3, 3.1}}, 3200);
    auto missDefinition = missPlan->copyDefinition();
    missDefinition.bypassOperationCache = true;
    const auto missInteractive = publish(std::move(missDefinition));
    const auto beforeMiss = disk->statistics();
    const auto gestureMiss = builder.build(missInteractive, requestFor(*missInteractive));
    disk->flush();
    const auto afterMiss = disk->statistics();
    expectations.expect(gestureMiss.hasValue() &&
                            gestureMiss.scene->mediaStatistics().imageConversions == 1,
                        "a gesture miss converts directly and is not inserted");
    expectations.expect(afterMiss.hits == beforeMiss.hits &&
                            afterMiss.misses == beforeMiss.misses &&
                            afterMiss.entryCount == beforeMiss.entryCount &&
                            afterMiss.storedBytes == beforeMiss.storedBytes,
                        "a gesture miss never reads or writes the disk cache");

    auto explicitRequest = requestFor(*plan);
    explicitRequest.bypassOperationCache = true;
    const auto beforeExplicit = disk->statistics();
    const auto explicitBuild = builder.build(plan, explicitRequest);
    disk->flush();
    const auto afterExplicit = disk->statistics();
    expectations.expect(explicitBuild.hasValue() &&
                            explicitBuild.scene->mediaStatistics().imageConversions == 1,
                        "an explicit bypass reconverts");
    expectations.expect(afterExplicit.hits == beforeExplicit.hits &&
                            afterExplicit.misses == beforeExplicit.misses &&
                            afterExplicit.entryCount == beforeExplicit.entryCount &&
                            afterExplicit.storedBytes == beforeExplicit.storedBytes,
                        "an explicit bypass never reads or writes the disk cache either");
}

void testImmutableSourceRevisionReuse(Expectations& expectations, const MediaFixture& fixture) {
    using bloom::runtime::MediaSourceRevisionPolicy;
    using bloom::runtime::OperationCacheEntryKind;
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.directory);
    auto context = deterministicMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto planA =
        mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {4.3, 3.1}}, 3900);
    const auto planB =
        mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {2.7, 2.4}}, 4000);
    auto requestA = requestFor(*planA);
    auto requestB = requestFor(*planB);
    requestA.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Retained;
    requestB.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Retained;
    const auto cold = builder.build(planA, requestA);
    // ADR 0025 (amended 2026-11-04): an unchanged identity admits the bytes without a hash.
    expectations.expect(cold && cold.scene->mediaStatistics().sourceRevisionCaptures == 1 &&
                            cold.scene->mediaStatistics().sourceRevisionBytesHashed == 0 &&
                            cold.scene->mediaStatistics().sourceBytesRead > 0 &&
                            cold.scene->mediaStatistics().sourceIoNanoseconds > 0 &&
                            cold.scene->mediaStatistics().sourceHashNanoseconds == 0 &&
                            cold.scene->mediaStatistics().sourceProbeNanoseconds > 0 &&
                            cold.scene->mediaStatistics().imageDecodeNanoseconds > 0,
                        "cold interactive source captures one immutable revision without a hash");
    const auto charged =
        evaluator.operationCache()->retainedBytes(OperationCacheEntryKind::DecodedMedia);
    expectations.expect(charged >= std::filesystem::file_size(fixture.path),
                        "same source pool charges encoded source bytes and metadata");
    // Remove the path while a captured revision is retained. Warm source selection must have no
    // filesystem dependency: the immutable captured bytes, rather than metadata, are authority.
    const auto heldPath = fixture.directory / "held_revision.exr";
    std::filesystem::rename(fixture.path, heldPath);
    const auto warm = builder.build(planB, requestB);
    if (!warm) {
        std::cerr << "warm revision preparation: " << static_cast<int>(warm.diagnostic.code) << ' '
                  << warm.diagnostic.message << '\n';
    } else {
        const auto& work = warm.scene->mediaStatistics();
        if (work.sourceRevisionCacheHits != 1 || work.sourceRevisionCaptures != 0 ||
            work.imageConversions != 1) {
            std::cerr << "warm revision work: hits=" << work.sourceRevisionCacheHits
                      << " captures=" << work.sourceRevisionCaptures
                      << " decode-ns=" << work.imageDecodeNanoseconds
                      << " conversions=" << work.imageConversions << '\n';
        }
    }
    expectations.expect(warm && warm.scene->mediaStatistics().sourceRevisionCacheHits == 1 &&
                            warm.scene->mediaStatistics().sourceRevisionCaptures == 0 &&
                            warm.scene->mediaStatistics().sourceRevisionBytesHashed == 0 &&
                            warm.scene->mediaStatistics().sourceBytesRead == 0 &&
                            warm.scene->mediaStatistics().sourceIoNanoseconds == 0 &&
                            warm.scene->mediaStatistics().sourceHashNanoseconds == 0 &&
                            warm.scene->mediaStatistics().sourceProbeNanoseconds == 0 &&
                            warm.scene->mediaStatistics().imageConversions == 1,
                        "warm transform edit decodes the retained revision with zero probe/hash");
    auto finalCpuRequest = requestB;
    finalCpuRequest.requireVerifiedMediaSources = true;
    const auto cpuFinal = evaluator.evaluate(planB, finalCpuRequest, {});
    expectations.expect(
        cpuFinal.status() == bloom::runtime::EvaluationStatus::Failed && !cpuFinal.frame(),
        "CPU final boundary rejects missing still despite explicit Retained request");
    auto finalRequest = requestB;
    finalRequest.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Revalidate;
    expectations.expect(
        !builder.build(planB, finalRequest),
        "default final revalidation refuses a disappeared source despite warm upload");
    bloom::runtime::GpuProcessFrameEvaluatorOptions finalOptions;
    finalOptions.mediaContext = context;
    const auto forcedFinal =
        bloom::runtime::detail::prepareExportScene(finalOptions, planB, requestB, {}, false);
    expectations.expect(
        !forcedFinal.scene,
        "production final preparation overrides an explicitly Retained preview request");
    auto bypass = requestB;
    bypass.bypassOperationCache = true;
    expectations.expect(!builder.build(planB, bypass),
                        "explicit bypass revalidates instead of trusting retained revision");
    auto relinked = fixture.asset;
    relinked.locator.path = "absent_relink.exr";
    relinked.locator.relinkHint = "file:///absent_relink.exr";
    const auto relinkPlan = mediaPlan(format(8, 8), relinked, LayerValues{}, 4100);
    auto relinkRequest = requestFor(*relinkPlan);
    relinkRequest.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Retained;
    expectations.expect(!builder.build(relinkPlan, relinkRequest),
                        "changed locator cannot adopt an unrelated retained ticket");
    evaluator.operationCache()->invalidateSourceRevisions();
    expectations.expect(!builder.build(planB, requestB),
                        "New/Open invalidates source authority even while prepared pixels remain");
    evaluator.operationCache()->clearDecodedMedia();
    expectations.expect(
        !builder.build(planB, requestB),
        "Refresh/purge drops tickets and rechecks missing bytes before upload hits");
    std::filesystem::rename(heldPath, fixture.path);
    const auto reconnected = builder.build(planB, requestB);
    expectations.expect(reconnected &&
                            reconnected.scene->mediaStatistics().sourceRevisionCaptures == 1,
                        "reconnect after refresh captures fresh bytes");
    const auto captured =
        bloom::media::captureImageSourceRevision(fixture.path, bloom::media::kMaxImageStorageBytes);
    const auto generation = evaluator.operationCache()->decodedMediaGeneration();
    evaluator.operationCache()->clearDecodedMedia();
    if (captured.value.has_value()) {
        evaluator.operationCache()->store(
            "late-before-refresh-source", bloom::document::Revision{},
            {.image = {}, .values = {}, .bounds = {}, .sourceRevision = *captured.value},
            OperationCacheEntryKind::DecodedMedia, generation);
        expectations.expect(!evaluator.operationCache()->find("late-before-refresh-source",
                                                              bloom::document::Revision{}),
                            "capture started before refresh cannot repopulate revision authority");
        const auto stillValid = bloom::media::decodeImage(**captured.value);
        expectations.expect(stillValid.value.has_value(),
                            "refresh preserves already-held immutable snapshot lifetime");
    }
    evaluator.operationCache()->setByteBudget(0);
    expectations.expect(evaluator.operationCache()->retainedBytes() == 0,
                        "live source retention budget reduction removes encoded revision charges");
    const auto unretained = builder.build(planB, requestB);
    expectations.expect(unretained &&
                            unretained.scene->mediaStatistics().sourceRevisionCaptures == 1 &&
                            unretained.scene->mediaStatistics().sourceRevisionCacheHits == 0 &&
                            evaluator.operationCache()->retainedBytes() == 0,
                        "zero retention keeps fresh preparation valid without bypassing live cap");
    if (cold && warm) {
        const auto* uploadCold = firstUpload(*cold.scene);
        const auto* uploadWarm = firstUpload(*warm.scene);
        expectations.expect(
            uploadCold && uploadWarm && uploadCold->image && !uploadCold->image->pixels().empty(),
            "already published immutable upload remains valid after ticket eviction");
    }
}

// Final/per-frame export revalidates every frame. An unchanged source is an identity check against
// the verified revision: zero whole-file reads. A changed source is still rejected.
void testRevalidatedFramesReadNothing(Expectations& expectations, const MediaFixture& fixture) {
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.directory);
    auto context = deterministicMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = mediaPlan(format(8, 8), fixture.asset, LayerValues{}, 4300);
    auto request = requestFor(*plan);
    request.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Revalidate;
    request.requireVerifiedMediaSources = true;
    const auto cold = builder.build(plan, request);
    expectations.expect(cold.hasValue(), "cold verified frame prepares");
    const auto reads = bloom::platform::sourceReadWork().fullReads;
    for (int frame = 0; frame < 5; ++frame) {
        const auto warm = builder.build(plan, request);
        expectations.expect(warm && warm.scene->mediaStatistics().sourceRevisionCaptures == 0 &&
                                warm.scene->mediaStatistics().sourceRevisionCacheHits == 1 &&
                                warm.scene->mediaStatistics().sourceBytesRead == 0,
                            "revalidated GPU frame reuses the verified revision");
        const auto cpu = evaluator.evaluate(plan, request, {});
        expectations.expect(cpu.frame() != nullptr, "revalidated CPU frame evaluates");
    }
    expectations.expect(bloom::platform::sourceReadWork().fullReads == reads,
                        "per-frame revalidation performs zero full-file reads");
    std::vector<char> original(std::filesystem::file_size(fixture.path));
    {
        std::ifstream input(fixture.path, std::ios::binary);
        input.read(original.data(), static_cast<std::streamsize>(original.size()));
    }
    {
        std::ofstream output(fixture.path, std::ios::binary | std::ios::app);
        output << '\0';
    }
    expectations.expect(!builder.build(plan, request),
                        "a changed source is rejected despite the retained revision");
    const auto cpuChanged = evaluator.evaluate(plan, request, {});
    expectations.expect(cpuChanged.status() == bloom::runtime::EvaluationStatus::Failed,
                        "CPU final rejects the changed source");
    {
        std::ofstream output(fixture.path, std::ios::binary | std::ios::trunc);
        output.write(original.data(), static_cast<std::streamsize>(original.size()));
    }
    expectations.expect(builder.build(plan, request).hasValue(), "restored content verifies again");
}

// Two layers of one source in a linear pass (an export). The pass stores a source revision only
// when its key is offered a second time, so before the build shared its captures the second layer
// read and hashed the file again on every frame.
void testLinearPassLayersShareOneCapture(Expectations& expectations, const MediaFixture& fixture) {
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.directory);
    auto context = deterministicMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    constexpr std::uint64_t idBase = 4700;
    const auto parameter = [](const std::uint64_t raw) {
        return bloom::document::ParameterId::fromRaw(raw);
    };
    std::vector<CompiledOperation> operations;
    std::vector<CompiledMergeInput> inputs;
    for (std::uint64_t layer = 0; layer < 2; ++layer) {
        const auto base = idBase + layer * 20;
        const LayerIds ids{parameter(base + 0), parameter(base + 1), parameter(base + 2),
                           parameter(base + 3), parameter(base + 4), parameter(base + 5)};
        const auto source = OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size()));
        operations.emplace_back(
            bloom::runtime::CompiledImageSource{bloom::document::NodeId::fromRaw(base + 10),
                                                fixture.asset, 0, 0, 0, std::string{}, false});
        const auto output = OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size()));
        operations.emplace_back(
            layerOutput(bloom::document::NodeId::fromRaw(base + 11),
                        bloom::document::LayerId::fromRaw(base + 12), source, ids,
                        LayerValues{.position = {1.0 + static_cast<double>(layer), 2.0}}));
        inputs.push_back(CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(base + 13),
                                            bloom::document::LayerId::fromRaw(base + 12), output});
    }
    operations.emplace_back(
        CompiledMerge{bloom::document::NodeId::fromRaw(idBase + 60), std::move(inputs)});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(idBase + 61),
                                                      OperationIndex::fromRaw(4)});
    const auto plan = publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, format(8, 8),
        std::move(operations), OperationIndex::fromRaw(5)});
    auto request = requestFor(*plan);
    request.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Revalidate;
    request.requireVerifiedMediaSources = true;
    request.linearPass = std::make_shared<bloom::runtime::LinearPassRetention>();
    const auto built = builder.build(plan, request);
    expectations.expect(built && built.scene->mediaStatistics().sourceRevisionCaptures == 1 &&
                            built.scene->mediaStatistics().sourceRevisionCacheHits == 1,
                        "linear pass: two layers of one source capture it once per frame");
}

void testGestureSourceRevisionIsReadOnly(Expectations& checks, const MediaFixture& fixture) {
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.directory);
    auto context = deterministicMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto ordinary =
        mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {3.0, 3.0}}, 4200);
    auto definition = ordinary->copyDefinition();
    definition.bypassOperationCache = true;
    const auto gesture = publish(std::move(definition));
    auto request = requestFor(*gesture);
    request.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    const auto prepared = builder.build(gesture, request);
    checks.expect(prepared.hasValue() &&
                      prepared.scene->mediaStatistics().sourceRevisionCaptures == 1 &&
                      context.operationCache->retainedBytes() == 0,
                  "cold GPU gesture captures transient source without inserting a ticket");
    const auto cpu = evaluator.evaluate(gesture, request, {});
    checks.expect(cpu.frame() != nullptr && context.operationCache->retainedBytes() == 0,
                  "cold CPU gesture inserts neither source revision nor decoded/derived pixels");
    auto ordinaryRequest = requestFor(*ordinary);
    ordinaryRequest.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    const auto warmed = builder.build(ordinary, ordinaryRequest);
    const auto bytes = context.operationCache->retainedBytes();
    const auto reused = builder.build(gesture, request);
    if (!reused) {
        std::cerr << "warm gesture preparation: " << static_cast<int>(reused.diagnostic.code) << ' '
                  << reused.diagnostic.message << '\n';
    } else {
        const auto& work = reused.scene->mediaStatistics();
        if (work.sourceRevisionCacheHits != 1 || work.sourceRevisionCaptures != 0) {
            std::cerr << "warm gesture work: hits=" << work.sourceRevisionCacheHits
                      << " captures=" << work.sourceRevisionCaptures << '\n';
        }
    }
    checks.expect(warmed && bytes > 0 && reused &&
                      reused.scene->mediaStatistics().sourceRevisionCacheHits == 1 &&
                      reused.scene->mediaStatistics().sourceRevisionCaptures == 0 &&
                      context.operationCache->retainedBytes() == bytes,
                  "warm GPU gesture reads the existing immutable ticket without inserting");
    const auto cpuReused = evaluator.evaluate(gesture, request, {});
    checks.expect(cpuReused.frame() != nullptr && context.operationCache->retainedBytes() == bytes,
                  "warm CPU gesture remains a read-only source-cache consumer");
}

// Image-sequence read-ahead: preparing frame 0 captures the next members on the read-ahead
// workers, so preparing frame 1 then reuses a verified revision and reads nothing itself.
void testSequenceReadAhead(Expectations& expectations, const MediaFixture& fixture) {
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.directory);
    std::vector<document::AssetSequenceMember> members;
    for (int frame = 0; frame < 4; ++frame) {
        auto pixels = signedHdrPixels();
        pixels.front().red += static_cast<float>(frame);
        const auto path = fixture.directory / ("read_ahead." + std::to_string(frame) + ".exr");
        writeExrRgba(path, 3, 2, pixels);
        members.push_back(sequenceMember(path, frame, static_cast<std::uint64_t>(frame) + 10));
    }
    document::AssetRecord asset = fixture.asset;
    asset.id = bloom::document::AssetId::fromRaw(903);
    asset.kind = document::AssetKind::Sequence;
    asset.manifest.pattern = "read_ahead.#.exr";
    asset.manifest.padding = 1;
    asset.manifest.first = 0;
    asset.manifest.last = 3;
    asset.manifest.members = members;
    const auto plan = mediaPlan(format(8, 8), asset, LayerValues{}, 1800);
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    expectations.expect(context.sequenceReadAhead != nullptr,
                        "the evaluator's media context carries its sequence read-ahead");
    const CpuGpuSceneBuilder builder(nullptr, context);
    auto request = requestFor(*plan, RationalTime::fromInteger(0));
    request.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Revalidate;
    request.requireVerifiedMediaSources = true;
    const auto first = builder.build(plan, request);
    expectations.expect(first.hasValue() &&
                            first.scene->mediaStatistics().sourceRevisionCaptures == 1,
                        "frame 0 captures its own member");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (context.sequenceReadAhead->statistics().captured +
                   context.sequenceReadAhead->statistics().failed <
               3 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto statistics = context.sequenceReadAhead->statistics();
    expectations.expect(statistics.queued == 3 && statistics.captured == 3 &&
                            statistics.failed == 0,
                        "the three following members are captured ahead of need");
    for (int frame = 1; frame < 4; ++frame) {
        auto next = requestFor(*plan, rationalTime(frame, 24));
        next.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Revalidate;
        next.requireVerifiedMediaSources = true;
        const auto built = builder.build(plan, next);
        expectations.expect(built && built.scene->mediaStatistics().sourceRevisionCaptures == 0 &&
                                built.scene->mediaStatistics().sourceRevisionCacheHits == 1 &&
                                built.scene->mediaStatistics().sourceBytesRead == 0,
                            "a read-ahead member is reused without reading the file again");
    }
    // Back at frame 0 after playing to frame 3 (ADR 0037, the playback window): members 1 and 2
    // were released behind the playhead and are captured again; member 3, still inside the window
    // ahead of frame 0, is cached and is not requested twice.
    const auto again = builder.build(plan, request);
    expectations.expect(again.hasValue(), "frame 0 prepares again");
    const auto againDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (context.sequenceReadAhead->statistics().captured +
                   context.sequenceReadAhead->statistics().failed <
               5 &&
           std::chrono::steady_clock::now() < againDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    expectations.expect(context.sequenceReadAhead->statistics().failed == 0 &&
                            context.sequenceReadAhead->statistics().captured == 5 &&
                            context.sequenceReadAhead->statistics().queued == 5,
                        "released members are captured again; a cached member is not");
}

// #477: a frame claims the sequence member it needs from the read-ahead, so the file is read once.
// A capture still queued is withdrawn (the frame reads the file itself); a capture in flight is
// waited for until it ends, and the wait stops when the frame is cancelled. While a frame reads
// its own source, no capture starts.
void testSequenceReadAheadClaim(Expectations& expectations, const MediaFixture& fixture) {
    // A sparse file the single worker takes many milliseconds to read, then fails to probe.
    const auto slow = fixture.directory / "claim_slow.bin";
    { std::ofstream create(slow, std::ios::binary); }
    std::filesystem::resize_file(slow, 256ULL << 20);
    const auto queued = fixture.directory / "claim_queued.bin";
    { std::ofstream(queued, std::ios::binary) << "queued"; }
    const auto cache = std::make_shared<bloom::runtime::OperationCache>(std::size_t{1} << 30);
    using bloom::runtime::ImageSequenceReadAhead;
    ImageSequenceReadAhead readAhead(1);
    const auto capture = [](const std::filesystem::path& path, std::string key) {
        return ImageSequenceReadAhead::Request{
            .path = path,
            .digest = {},
            .stamp = std::nullopt,
            .recordedDigest = std::nullopt,
            .revisionKey = std::move(key),
            .byteBudget = std::size_t{1} << 30,
            .use = {}};
    };
    const auto waitStarted = [&](const std::uint64_t count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (readAhead.statistics().started < count &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return readAhead.statistics().started == count;
    };
    readAhead.request(cache, {capture(slow, "slow"), capture(queued, "queued")});
    expectations.expect(waitStarted(1), "the single worker starts the first capture");
    const bool queuedWaited = readAhead.claim("queued", {});
    auto statistics = readAhead.statistics();
    expectations.expect(!queuedWaited && statistics.withdrawn == 1 && statistics.awaited == 0,
                        "a queued capture is withdrawn and the frame reads the file itself");
    const bool slowWaited = readAhead.claim("slow", {});
    statistics = readAhead.statistics();
    expectations.expect(slowWaited && statistics.awaited == 1 &&
                            statistics.captured + statistics.failed == 1,
                        "a capture in flight is waited for until it ends");
    expectations.expect(!readAhead.claim("slow", {}) && !readAhead.claim("queued", {}) &&
                            readAhead.statistics().withdrawn == 1 &&
                            readAhead.statistics().awaited == 1 &&
                            readAhead.statistics().started == 1,
                        "a withdrawn capture never starts and an ended one is not claimed again");
    // A frame reading its own source holds every capture start back until its read ends.
    {
        const auto foreground = readAhead.foregroundRead();
        readAhead.request(cache, {capture(slow, "slow")});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        expectations.expect(readAhead.statistics().started == 1 &&
                                readAhead.statistics().deferred != 0,
                            "no capture starts while a frame reads its own source");
    }
    expectations.expect(waitStarted(2), "the capture starts once the frame's read ends");
    const auto claimStart = std::chrono::steady_clock::now();
    const bool cancelledWaited = readAhead.claim("slow", [] { return true; });
    expectations.expect(cancelledWaited && readAhead.statistics().awaited == 2 &&
                            std::chrono::steady_clock::now() - claimStart <
                                std::chrono::seconds(1),
                        "a cancelled frame stops waiting for the capture");
}
