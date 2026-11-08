// #587 Test Card: lowering the source node that draws a test pattern. Included inside CompilePass,
// beside snapshot_compiler_fields.ipp, whose field program builder it uses.
//
// The card is one field raster (the TestCard instruction over the Coordinate) of a transparent canvas
// the size of the composition, cut or padded to a Custom size about the composition's centre. The raster
// writes the card's signal; every card but a working-space Patch then runs through one colour space
// transform into the working space, so the pixels reach the Viewer and every export as any source's do.

// The glyph code of one character of a label (field_kernel.glsl, bfk_tc_glyph_a): a space, the digits,
// the capitals, then . : , - / %. Other characters are a space.
[[nodiscard]] static std::int64_t testCardGlyph(const char c) {
    if (c >= '0' && c <= '9')
        return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z')
        return 11 + (c - 'A');
    if (c >= 'a' && c <= 'z')
        return 11 + (c - 'a');
    switch (c) {
    case '.':
        return 37;
    case ':':
        return 38;
    case ',':
        return 39;
    case '-':
        return 40;
    case '/':
        return 41;
    case '%':
        return 42;
    default:
        return 0;
    }
}

[[nodiscard]] bool
lowerTestCard(const document::NodeRecord& node, std::vector<runtime::CompiledOperation>& operations,
              std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    using core::field::FieldOutputMode;
    using core::field::FieldResultKind;
    const auto integer = [&](const std::string_view role) -> std::int64_t {
        const auto* spec = testCardParameterSpecForRole(role);
        const auto* value = parameterConstant<std::int64_t>(findParameterBinding(node, role));
        return value != nullptr ? *value : static_cast<std::int64_t>(spec->value);
    };
    const auto number = [&](const std::string_view role) -> double {
        const auto* spec = testCardParameterSpecForRole(role);
        const auto* value = parameterConstant<double>(findParameterBinding(node, role));
        return value != nullptr ? *value : spec->value;
    };
    const auto flag = [&](const std::string_view role) -> bool {
        const auto* spec = testCardParameterSpecForRole(role);
        const auto* value = parameterConstant<bool>(findParameterBinding(node, role));
        return value != nullptr ? *value : spec->value != 0.0;
    };
    const auto card = integer("card");
    const auto size = integer("sizeMode");
    const auto& format = composition_->format();
    const auto compositionWidth = static_cast<std::int64_t>(format.width());
    const auto compositionHeight = static_cast<std::int64_t>(format.height());
    const bool custom = size == kTestCardSizeCustom;
    const auto width = custom ? integer("width") : compositionWidth;
    const auto height = custom ? integer("height") : compositionHeight;
    if (card < 0 || card >= kTestCardCount || (size != kTestCardSizeComposition && !custom) ||
        width < 1 || height < 1 || width > kMaximumTestCardPixels ||
        height > kMaximumTestCardPixels) {
        addTopologyFailure(node.id, "Test Card parameters could not be lowered.");
        return false;
    }
    if (isMuted(node.id)) {
        // A muted source draws nothing.
        const auto index = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledMerge{node.id, {}});
        portOperations_.insert_or_assign({node.id, std::string(kTestCardImagePort)}, index);
        indices.emplace(node.id, index);
        return true;
    }

    // The canvas: the composition's, cut or padded about its centre to the card's own size. The
    // integer margins fix the card's centre exactly, whatever the parities.
    const auto floorHalf = [](const std::int64_t d) { return d >= 0 ? d / 2 : -((1 - d) / 2); };
    const auto left = floorHalf(compositionWidth - width);
    const auto top = floorHalf(compositionHeight - height);
    const auto right = compositionWidth - width - left;
    const auto bottom = compositionHeight - height - top;
    auto canvas = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(runtime::CompiledSolid{
        node.id,
        runtime::CompiledColorParameter{document::ParameterId{}, core::Color4d{0.0, 0.0, 0.0, 0.0}},
        fieldConstant(static_cast<double>(compositionWidth)),
        fieldConstant(static_cast<double>(compositionHeight))});
    const std::array<std::int64_t, 4> margins{left, top, right, bottom};
    const auto cut = [&](const runtime::CanvasImageEffect effect, const std::int64_t sign) {
        std::array<runtime::CompiledScalarParameter, 4> amounts;
        bool any = false;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto amount = std::max<std::int64_t>(sign * margins[i], 0);
            any = any || amount > 0;
            amounts[i] = fieldConstant(static_cast<double>(amount));
        }
        if (!any)
            return;
        const auto index = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledImageEffect{
            node.id, canvas, runtime::CanvasImageKernel{effect, amounts}});
        canvas = index;
    };
    cut(runtime::CanvasImageEffect::Crop, 1);
    cut(runtime::CanvasImageEffect::Pad, -1);
    // The card's centre in field positions (composition pixels from the centre, y up).
    const auto centreX = static_cast<double>(left) + 0.5 * static_cast<double>(width) -
                         0.5 * static_cast<double>(compositionWidth);
    const auto centreY = 0.5 * static_cast<double>(compositionHeight) -
                         (static_cast<double>(top) + 0.5 * static_cast<double>(height));

    const auto cardId = static_cast<TestCard>(card);
    const auto hdrVariant = integer("hdrVariant");
    const auto patchSpace = integer("patchSpace");
    const bool labelled = flag("label") && testCardParameterRelevant(card, "label");
    const bool patchStep = card == static_cast<std::int64_t>(TestCard::Patch) && flag("patchStep");
    const bool zoneMoving = cardId == TestCard::ZonePlate && flag("zoneMoving");
    const bool vertical = integer("barDirection") == 1;
    const bool animated = patchStep || zoneMoving || cardId == TestCard::MovingBar ||
                          cardId == TestCard::Countdown;
    std::int64_t variant = 0;
    std::int64_t option = 0;
    double v0 = 0.0;
    double v1 = 0.0;
    switch (cardId) {
    case TestCard::HdrBars:
        variant = hdrVariant;
        break;
    case TestCard::Patch:
        variant = integer("patchValue");
        option = integer("patchWindow");
        break;
    case TestCard::SafeAreas:
        option = (flag("guide169") ? 1 : 0) | (flag("guide916") ? 2 : 0) |
                 (flag("guide11") ? 4 : 0) | (flag("guide239") ? 8 : 0);
        break;
    case TestCard::Grid:
        v0 = number("gridSpacing");
        v1 = number("gridWidth");
        break;
    case TestCard::UvGrid:
        v0 = static_cast<double>(width) / static_cast<double>(std::max<std::int64_t>(integer("uvCells"), 1));
        break;
    case TestCard::SiemensStar:
        v0 = static_cast<double>(integer("starSpokes"));
        break;
    case TestCard::MovingBar:
        option = vertical ? 1 : 0;
        v0 = number("barWidth") * 0.01 * static_cast<double>(vertical ? height : width);
        v1 = number("barDuration");
        break;
    case TestCard::Countdown:
        v0 = static_cast<double>(integer("countdownStart"));
        break;
    default:
        break;
    }
    std::vector<std::int64_t> choices{card, variant, option,
                                      (labelled ? 1 : 0) | (animated ? 2 : 0)};
    if (labelled) {
        const auto& names = test_card_parameters::kCards;
        std::string text(names[static_cast<std::size_t>(card)]);
        text += ' ';
        text += std::to_string(width);
        text += 'X';
        text += std::to_string(height);
        text.resize(std::min<std::size_t>(text.size(), core::field::kTestCardMaxLabel));
        choices[3] |= static_cast<std::int64_t>(text.size()) << 4;
        for (const auto c : text)
            choices.push_back(testCardGlyph(c));
    }
    const auto rate = format.frameRate();
    const auto fps = static_cast<double>(rate.numerator()) / static_cast<double>(rate.denominator());

    FieldBuild build;
    build.operations = &operations;
    build.indices = &indices;
    build.coordinate = fieldEmit(build, FieldOp::Coordinate, {-1, -1, -1, -1});
    // The Countdown reads its start in the fifth scalar and the Moving Bar its crossing time in the
    // sixth (runtime/field_raster.cpp).
    const auto result = fieldEmit(
        build, FieldOp::TestCard, {build.coordinate, -1, -1, -1}, std::move(choices),
        {fieldConstant(static_cast<double>(width)), fieldConstant(static_cast<double>(height)),
         fieldConstant(centreX), fieldConstant(centreY), fieldConstant(v0), fieldConstant(v1),
         fieldConstant(0.0), fieldConstant(0.0), fieldConstant(fps)});
    build.usesTime = animated;
    auto program = finishFieldProgram(build, result, FieldResultKind::Premultiplied,
                                      FieldOutputMode::Raw, node.id);
    if (!program)
        return false;
    const std::uint32_t samples =
        cardId == TestCard::ZonePlate || cardId == TestCard::SiemensStar ? 16U : 4U;
    auto last = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(runtime::CompiledImageEffect{
        node.id, canvas, runtime::FieldRasterKernel{std::move(*program), samples}});
    // Into the working space, from the colour space the signal is written in.
    const std::string from(testCardSignalSpaceId(card, patchSpace, hdrVariant));
    if (!from.empty()) {
        const auto index = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledImageEffect{
            node.id, last, runtime::CstKernel{from, std::string{}}, false});
        last = index;
    }
    portOperations_.insert_or_assign({node.id, std::string(kTestCardImagePort)}, last);
    indices.emplace(node.id, last);
    return true;
}
