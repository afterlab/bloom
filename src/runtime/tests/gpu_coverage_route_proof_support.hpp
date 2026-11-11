#pragma once

// Private route-proof support for the bounded GPU coverage contract test. It proves the typed
// route-proof sink rejects every generic, type-invalid, stale-shaped, or policy-violating fake. It
// never publishes a fabricated "real" proof: no genuine route harness exists in this tree yet, so
// every route stays MISSING until one does.

#include <bloom/runtime/gpu_coverage_route_proof_contract.hpp>

#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::gpu_coverage_route_proof {

// Clearly synthetic, canonical 64-character lowercase SHA-256 digests, used only as unit data.
inline constexpr std::string_view kUnitFrameDigest =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
inline constexpr std::string_view kUnitEvidenceDigest =
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

// A shape-valid PREVIEW proof used only as a mutation base: every field below is synthetic unit
// data, never route acceptance. Each negative case perturbs exactly one field so the expected
// rejection reason is unambiguous.
[[nodiscard]] inline bloom::runtime::GpuRouteExecutionProof representativeRouteProof() {
    return bloom::runtime::GpuRouteExecutionProof{
        .routeId = "route.preview.viewer",
        .harness = bloom::runtime::GpuRouteHarnessKind::ViewerPreview,
        .processIdentityDigest = std::string{kUnitFrameDigest},
        .deviceOwnershipEpoch = "12345",
        .nativeDispatches = 4,
        .verifiedFrames = 1,
        .readbackSubmissions = 0,
        .payloads = 0,
        .transferredBytes = 0,
        .evidenceDigest = std::string{kUnitEvidenceDigest}};
}

// A shape-valid combined-export base: one verified frame, one final submission, two payloads.
[[nodiscard]] inline bloom::runtime::GpuRouteExecutionProof representativeExportProof() {
    auto proof = representativeRouteProof();
    proof.routeId = "route.export.video";
    proof.harness = bloom::runtime::GpuRouteHarnessKind::VideoExport;
    proof.readbackSubmissions = 1;
    proof.payloads = 2;
    proof.transferredBytes = 4096;
    return proof;
}

// Proves the route sink rejects every generic/boolean-shaped, type-invalid, or policy-violating
// fake and never retains one. A two-payload combined export is accepted by the validator's
// arithmetic but is never published here; this helper only asserts rejections.
inline void routeProofSinkRejectsFakes(std::vector<std::string>& failures) {
    using bloom::runtime::GpuCoverageContractIssue;
    bloom::runtime::GpuRouteProofSink sink;
    const auto expectReject = [&](const bloom::runtime::GpuRouteExecutionProof& proof,
                                  const GpuCoverageContractIssue expected,
                                  const std::string_view name) {
        if (sink.publish(proof) != expected) {
            failures.push_back("route-proof negative: '" + std::string{name} +
                               "' was not rejected with the expected reason");
        }
    };
    expectReject(bloom::runtime::GpuRouteExecutionProof{},
                 GpuCoverageContractIssue::RouteProofEmptyOrGeneric, "empty/generic proof");
    auto unknown = representativeRouteProof();
    unknown.routeId = "route.does-not-exist";
    expectReject(unknown, GpuCoverageContractIssue::RouteProofUnknownRoute, "unknown route");
    auto mismatch = representativeRouteProof();
    mismatch.harness = bloom::runtime::GpuRouteHarnessKind::RamPreview;
    expectReject(mismatch, GpuCoverageContractIssue::RouteProofHarnessMismatch,
                 "harness/route mismatch");
    auto noProvenance = representativeRouteProof();
    noProvenance.evidenceDigest.clear();
    expectReject(noProvenance, GpuCoverageContractIssue::RouteProofMissingProvenance,
                 "missing evidence digest");
    auto shortDigest = representativeRouteProof();
    shortDigest.processIdentityDigest = "not-a-sha256";
    expectReject(shortDigest, GpuCoverageContractIssue::RouteProofMissingProvenance,
                 "non-canonical frame digest");
    auto uppercaseDigest = representativeRouteProof();
    uppercaseDigest.evidenceDigest =
        "FEDCBA9876543210FEDCBA9876543210FEDCBA9876543210FEDCBA9876543210";
    expectReject(uppercaseDigest, GpuCoverageContractIssue::RouteProofMissingProvenance,
                 "uppercase evidence digest");
    auto zeroEpoch = representativeRouteProof();
    zeroEpoch.deviceOwnershipEpoch = "0000";
    expectReject(zeroEpoch, GpuCoverageContractIssue::RouteProofMissingProvenance,
                 "zero device epoch");
    auto alphaEpoch = representativeRouteProof();
    alphaEpoch.deviceOwnershipEpoch = "abc";
    expectReject(alphaEpoch, GpuCoverageContractIssue::RouteProofMissingProvenance,
                 "non-decimal device epoch");
    auto negativeEpoch = representativeRouteProof();
    negativeEpoch.deviceOwnershipEpoch = "-1";
    expectReject(negativeEpoch, GpuCoverageContractIssue::RouteProofMissingProvenance,
                 "negative device epoch");
    auto noDispatch = representativeRouteProof();
    noDispatch.nativeDispatches = 0;
    expectReject(noDispatch, GpuCoverageContractIssue::RouteProofNoNativeDispatch,
                 "zero cold native dispatch");
    auto noFrame = representativeRouteProof();
    noFrame.verifiedFrames = 0;
    expectReject(noFrame, GpuCoverageContractIssue::RouteProofNoVerifiedFrame, "no verified frame");
    auto previewReadback = representativeRouteProof();
    previewReadback.readbackSubmissions = 1;
    expectReject(previewReadback, GpuCoverageContractIssue::RouteProofExcessReadback,
                 "preview readback submission");
    auto tooManyPayloads = representativeExportProof();
    tooManyPayloads.payloads = 3; // > two payloads per verified frame
    expectReject(tooManyPayloads, GpuCoverageContractIssue::RouteProofExcessReadback,
                 "export three payloads for one frame");
    auto tooManySubmissions = representativeExportProof();
    tooManySubmissions.readbackSubmissions = 2; // > one submission per verified frame
    expectReject(tooManySubmissions, GpuCoverageContractIssue::RouteProofExcessReadback,
                 "export two submissions for one frame");
    auto fewerPayloads = representativeExportProof();
    fewerPayloads.verifiedFrames = 2;
    fewerPayloads.readbackSubmissions = 2;
    fewerPayloads.payloads = 1; // fewer payloads than submissions is impossible
    expectReject(fewerPayloads, GpuCoverageContractIssue::RouteProofExcessReadback,
                 "export fewer payloads than submissions");
    auto overflowPayloads = representativeExportProof();
    overflowPayloads.payloads = std::numeric_limits<std::uint64_t>::max();
    expectReject(overflowPayloads, GpuCoverageContractIssue::RouteProofExcessReadback,
                 "export payloads at UINT64_MAX");
    auto overflowSubmissions = representativeExportProof();
    overflowSubmissions.readbackSubmissions = std::numeric_limits<std::uint64_t>::max();
    expectReject(overflowSubmissions, GpuCoverageContractIssue::RouteProofExcessReadback,
                 "export submissions at UINT64_MAX");
    auto noBytes = representativeExportProof();
    noBytes.transferredBytes = 0;
    expectReject(noBytes, GpuCoverageContractIssue::RouteProofMissingTransferredBytes,
                 "export without transferred bytes");
    if (!sink.proofs().empty()) {
        failures.push_back("route-proof sink retained a rejected proof");
    }
    // The host-painted preview proofs: accepted only where the platform paints the preview on the
    // host, and only with exactly one final display transfer per verified frame.
    using bloom::runtime::GpuPreviewRouteDisplay;
    using bloom::runtime::GpuRouteHarnessKind;
    const auto firstIssue = [](const bloom::runtime::GpuRouteExecutionProof& proof,
                               const GpuPreviewRouteDisplay display) {
        const auto issues = bloom::runtime::validateGpuRouteExecutionProof(proof, display);
        return issues.empty() ? GpuCoverageContractIssue::None : issues.front().issue;
    };
    const auto expectIssue = [&](const bloom::runtime::GpuRouteExecutionProof& proof,
                                 const GpuPreviewRouteDisplay display,
                                 const GpuCoverageContractIssue expected,
                                 const std::string_view name) {
        if (firstIssue(proof, display) != expected) {
            failures.push_back("route-proof host-painted: '" + std::string{name} +
                               "' did not validate as expected");
        }
    };
    auto painted = representativeRouteProof();
    painted.harness = GpuRouteHarnessKind::ViewerPreviewHostPainted;
    painted.verifiedFrames = 3;
    painted.readbackSubmissions = 3;
    painted.payloads = 3;
    painted.transferredBytes = 3U * 4096U;
    expectIssue(painted, GpuPreviewRouteDisplay::HostPainted, GpuCoverageContractIssue::None,
                "one transfer per frame on a host-painting platform");
    expectIssue(painted, GpuPreviewRouteDisplay::Resident,
                GpuCoverageContractIssue::RouteProofHarnessMismatch,
                "host-painted proof on a resident platform");
    auto paintedRam = painted;
    paintedRam.routeId = "route.preview.ram_preview";
    paintedRam.harness = GpuRouteHarnessKind::RamPreviewHostPainted;
    expectIssue(paintedRam, GpuPreviewRouteDisplay::HostPainted, GpuCoverageContractIssue::None,
                "RAM preview one transfer per frame");
    auto paintedWrongRoute = painted;
    paintedWrongRoute.harness = GpuRouteHarnessKind::RamPreviewHostPainted;
    expectIssue(paintedWrongRoute, GpuPreviewRouteDisplay::HostPainted,
                GpuCoverageContractIssue::RouteProofHarnessMismatch,
                "host-painted harness for the other route");
    auto paintedNoTransfer = painted;
    paintedNoTransfer.readbackSubmissions = 0;
    paintedNoTransfer.payloads = 0;
    paintedNoTransfer.transferredBytes = 0;
    expectIssue(paintedNoTransfer, GpuPreviewRouteDisplay::HostPainted,
                GpuCoverageContractIssue::RouteProofExcessReadback,
                "host-painted proof without its transfers");
    auto paintedExtra = painted;
    paintedExtra.readbackSubmissions = 4;
    expectIssue(paintedExtra, GpuPreviewRouteDisplay::HostPainted,
                GpuCoverageContractIssue::RouteProofExcessReadback,
                "host-painted proof with an extra readback");
    auto paintedFewer = painted;
    paintedFewer.readbackSubmissions = 2;
    paintedFewer.payloads = 2;
    expectIssue(paintedFewer, GpuPreviewRouteDisplay::HostPainted,
                GpuCoverageContractIssue::RouteProofExcessReadback,
                "host-painted proof with a frame that was not transferred");
    auto paintedTwoPayloads = painted;
    paintedTwoPayloads.payloads = 6;
    expectIssue(paintedTwoPayloads, GpuPreviewRouteDisplay::HostPainted,
                GpuCoverageContractIssue::RouteProofExcessReadback,
                "host-painted proof with two payloads per frame");
    auto paintedNoBytes = painted;
    paintedNoBytes.transferredBytes = 0;
    expectIssue(paintedNoBytes, GpuPreviewRouteDisplay::HostPainted,
                GpuCoverageContractIssue::RouteProofMissingTransferredBytes,
                "host-painted proof without transferred bytes");
    auto paintedNoDispatch = painted;
    paintedNoDispatch.nativeDispatches = 0;
    expectIssue(paintedNoDispatch, GpuPreviewRouteDisplay::HostPainted,
                GpuCoverageContractIssue::RouteProofNoNativeDispatch,
                "host-painted proof with no native dispatch");
    // The resident proof keeps its rule on both kinds of platform.
    auto residentReadback = representativeRouteProof();
    residentReadback.readbackSubmissions = 1;
    expectIssue(residentReadback, GpuPreviewRouteDisplay::HostPainted,
                GpuCoverageContractIssue::RouteProofExcessReadback,
                "resident proof with a readback on a host-painting platform");
    expectIssue(representativeRouteProof(), GpuPreviewRouteDisplay::Resident,
                GpuCoverageContractIssue::None, "resident proof on a resident platform");
    std::cout << "NEGATIVE route-proof: eighteen generic/fake/type-invalid/policy proofs rejected; "
                 "sink retains none\n";
}

} // namespace bloom::gpu_coverage_route_proof
