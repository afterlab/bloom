// #641: a layer edge that covers part of a proxy pixel. A Solid whose size or position is odd
// lands between the pixels of a Half or Quarter proxy, so its edge pixels are partly covered. The
// CPU reference defines those pixels; the device must match it at the strict gate with exact
// alpha, the same as at full scale. Included by gpu_scene_effect_native_tests.cpp.

// A partial-alpha HDR Solid, placed by a Transform 2D or left at the origin, over an HDR Checker.
[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
proxyEdgePlan(const double width, const double height,
              const std::optional<bloom::document::Vec2d> position, const std::uint64_t idBase,
              const std::optional<double> cornerRadius = std::nullopt) {
    using namespace bloom::runtime;
    using bloom::document::NodeId;
    using bloom::document::ParameterId;
    CompiledCompositionPlanDefinition definition{
        bloom::document::Revision::fromRaw(1),
        bloom::document::ProjectId::fromRaw(1),
        bloom::document::CompositionId::fromRaw(idBase),
        *bloom::document::CompositionFormat::create(40, 24),
        {},
        OperationIndex::fromRaw(0)};
    definition.duration = bloom::core::RationalTime::fromInteger(10);
    auto& operations = definition.operations;
    detail::RecipeEmitter emitter(NodeId::fromRaw(idBase + 30), operations,
                                  detail::definitionValueSink(definition));
    const auto scalar = [](const std::uint64_t id, const double value) {
        return CompiledScalarParameter{ParameterId::fromRaw(id), value};
    };
    const auto vec2 = [](const std::uint64_t id, const double x, const double y) {
        return CompiledVec2Parameter{ParameterId::fromRaw(id), bloom::document::Vec2d{x, y}};
    };
    auto block = OperationIndex::fromRaw(operations.size());
    if (cornerRadius) {
        // A rounded rectangle Shape of the same size and color, read as an image.
        CompiledShape shape{};
        shape.sourceNodeId = NodeId::fromRaw(idBase + 1);
        shape.kind = bloom::document::ShapeKind::Rectangle;
        shape.size = {ParameterId::fromRaw(idBase + 3), bloom::document::Vec2d{width, height}};
        shape.cornerRadius = *cornerRadius;
        shape.fillEnabled = true;
        shape.fillColor = {ParameterId::fromRaw(idBase + 2),
                           bloom::core::Color4d{1.6, 0.4, 0.2, 0.75}};
        shape.strokeEnabled = false;
        operations.emplace_back(std::move(shape));
    } else {
        operations.emplace_back(
            CompiledSolid{NodeId::fromRaw(idBase + 1),
                          {ParameterId::fromRaw(idBase + 2),
                           bloom::core::Color4d{1.6, 0.4, 0.2, 0.75}},
                          scalar(idBase + 3, width), scalar(idBase + 4, height)});
    }
    if (position) {
        block = emitter.effect(
            block, Transform2DKernel{vec2(idBase + 5, position->x, position->y),
                                     vec2(idBase + 11, 0.0, 0.0), vec2(idBase + 12, 1.0, 1.0),
                                     scalar(idBase + 13, 0.0)});
    }
    const auto checker = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledChecker{
        NodeId::fromRaw(idBase + 14),
        {ParameterId::fromRaw(idBase + 15), bloom::core::Color4d{0.45, 0.4, 0.3, 1.0}},
        {ParameterId::fromRaw(idBase + 16), bloom::core::Color4d{0.08, 0.1, 0.2, 1.0}},
        5U,
        4U,
        40U,
        24U});
    const auto picture = emitter.merge(
        {{block, bloom::core::BlendMode::Normal}, {checker, bloom::core::BlendMode::Normal}});
    const auto merged = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 95), {{{}, {}, picture}}});
    definition.output = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 96), merged});
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

void runProxyEdges(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::document::Vec2d;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Proxy final {
        std::uint32_t width;
        std::uint32_t height;
        const char* name;
    };
    struct Size final {
        double width;
        double height;
        std::optional<double> cornerRadius = std::nullopt;
    };
    const Size sizes[] = {{9, 7}, {7, 5}, {5, 3}, {8, 6}, {21, 13, 4.0}, {17, 15, 6.5}};
    const std::optional<Vec2d> positions[] = {std::nullopt, Vec2d{6, 8}, Vec2d{5, 7}, Vec2d{3, 1},
                                              Vec2d{11, 9}};
    const Proxy proxies[] = {{40, 24, "Full"}, {20, 12, "Half"}, {10, 6, "Quarter"},
                             {37, 29, "an odd scale"}};
    std::uint64_t id = 64100;
    for (const Size& size : sizes) {
        for (const auto& position : positions) {
            const auto plan =
                proxyEdgePlan(size.width, size.height, position, id, size.cornerRadius);
            id += 200;
            for (const Proxy& proxy : proxies) {
                cache.clear();
                const std::string name =
                    "proxy edge native (" + std::to_string(static_cast<int>(size.width)) + "x" +
                    std::to_string(static_cast<int>(size.height)) +
                    (size.cornerRadius ? " rounded" : "") +
                    (position ? " at " + std::to_string(static_cast<int>(position->x)) + "," +
                                    std::to_string(static_cast<int>(position->y))
                              : std::string(" at the origin")) +
                    ", " + proxy.name + "): ";
                auto request = requestFor(*plan);
                const auto extent = bloom::render::ImageExtent::create(proxy.width, proxy.height);
                request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
                auto oracleRequest = request;
                oracleRequest.bypassOperationCache = true;
                const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
                const auto prepared = builder.build(plan, request);
                expectations.expect(oracle.frame() && prepared,
                                    name + "CPU oracle and production scene prepare: " +
                                        prepared.diagnostic.message);
                if (!oracle.frame() || !prepared)
                    continue;
                auto executor = GpuSceneExecutor::create(device, cache);
                expectations.expect(executor.hasValue(), name + "executor creates");
                if (!executor)
                    continue;
                const auto run = runScene(*executor.executor, prepared.scene);
                std::string detail;
                expectations.expect(run.pollResult == GpuSceneExecutorPollResult::Ready &&
                                        parity(run.pixels,
                                               oracle.frame()->processImage().pixels(), detail,
                                               AlphaGate::Exact),
                                    name + "CPU parity: " + detail);
                const auto& descriptor = *oracle.frame()->processImage().descriptor();
                expectations.expect(run.dataWindow == descriptor.dataWindow() &&
                                        run.displayWindow == descriptor.displayWindow() &&
                                        run.pixelAspect == descriptor.pixelAspect(),
                                    name + "data/display-window and PAR parity");
            }
        }
    }
}
