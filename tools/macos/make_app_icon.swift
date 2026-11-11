// Draws Bloom's macOS application icon and writes an .iconset directory.
//
//   swift tools/macos/make_app_icon.swift <output.iconset>
//   iconutil --convert icns --output apps/bloom/macos/Bloom.icns <output.iconset>
//
// The icon is drawn, not stored as art: a dark rounded tile with eight overlapping petals of
// light around a bright core. Replace apps/bloom/macos/Bloom.icns with finished artwork at any
// time; nothing else refers to this script.

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

func draw(pixels: Int) -> CGImage {
    let size = CGFloat(pixels)
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    let context = CGContext(
        data: nil, width: pixels, height: pixels, bitsPerComponent: 8, bytesPerRow: 0,
        space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    context.interpolationQuality = .high

    // The tile: the system icon grid leaves about a tenth of the canvas as margin.
    let inset = size * 0.098
    let tile = CGRect(x: inset, y: inset, width: size - 2 * inset, height: size - 2 * inset)
    let radius = tile.width * 0.2237
    let shape = CGPath(
        roundedRect: tile, cornerWidth: radius, cornerHeight: radius, transform: nil)
    context.saveGState()
    context.addPath(shape)
    context.clip()
    let background = CGGradient(
        colorsSpace: space,
        colors: [
            CGColor(srgbRed: 0.10, green: 0.09, blue: 0.16, alpha: 1),
            CGColor(srgbRed: 0.03, green: 0.03, blue: 0.06, alpha: 1),
        ] as CFArray, locations: [0, 1])!
    context.drawLinearGradient(
        background, start: CGPoint(x: tile.midX, y: tile.maxY),
        end: CGPoint(x: tile.midX, y: tile.minY), options: [])

    // Petals: circles on a ring, added together so overlaps grow brighter.
    let center = CGPoint(x: tile.midX, y: tile.midY)
    let ring = tile.width * 0.17
    let petal = tile.width * 0.20
    context.setBlendMode(.plusLighter)
    let petals = 8
    for index in 0..<petals {
        let turn = CGFloat(index) / CGFloat(petals)
        let angle = turn * 2 * .pi + .pi / 2
        let at = CGPoint(x: center.x + cos(angle) * ring, y: center.y + sin(angle) * ring)
        // From warm magenta through orange around the ring.
        let warm = 0.5 + 0.5 * cos(turn * 2 * .pi)
        let color = CGColor(
            srgbRed: 1.0, green: 0.30 + 0.35 * warm, blue: 0.62 - 0.42 * warm, alpha: 0.55)
        let clear = CGColor(srgbRed: 1.0, green: 0.4, blue: 0.4, alpha: 0)
        let glow = CGGradient(
            colorsSpace: space, colors: [color, color, clear] as CFArray,
            locations: [0, 0.62, 1])!
        context.drawRadialGradient(
            glow, startCenter: at, startRadius: 0, endCenter: at, endRadius: petal,
            options: [])
    }
    // The core.
    let core = CGGradient(
        colorsSpace: space,
        colors: [
            CGColor(srgbRed: 1, green: 0.98, blue: 0.92, alpha: 0.95),
            CGColor(srgbRed: 1, green: 0.85, blue: 0.6, alpha: 0.35),
            CGColor(srgbRed: 1, green: 0.8, blue: 0.5, alpha: 0),
        ] as CFArray, locations: [0, 0.45, 1])!
    context.drawRadialGradient(
        core, startCenter: center, startRadius: 0, endCenter: center,
        endRadius: tile.width * 0.20, options: [])
    context.restoreGState()
    return context.makeImage()!
}

func write(_ image: CGImage, to url: URL) {
    let destination = CGImageDestinationCreateWithURL(
        url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(destination, image, nil)
    precondition(CGImageDestinationFinalize(destination), "could not write \(url.path)")
}

guard CommandLine.arguments.count == 2 else {
    FileHandle.standardError.write("usage: make_app_icon.swift <output.iconset>\n".data(using: .utf8)!)
    exit(2)
}
let output = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
for points in [16, 32, 128, 256, 512] {
    write(draw(pixels: points), to: output.appendingPathComponent("icon_\(points)x\(points).png"))
    write(
        draw(pixels: points * 2),
        to: output.appendingPathComponent("icon_\(points)x\(points)@2x.png"))
}
