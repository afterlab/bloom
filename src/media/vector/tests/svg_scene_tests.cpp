// SVG scene reader (#343): known answers from generated files for every feature the importer relies
// on, and hostile or malformed files that must fail with a diagnostic and never crash, fetch or run
// anything.

#include <bloom/media/svg_scene.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
using namespace bloom::media;

int failures = 0;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

SvgReadResult parse(const std::string& text, const SvgCancel& cancel = {}) {
    return parseSvgScene(std::as_bytes(std::span(text)), cancel);
}
std::string wrap(const std::string& body, const std::string& attributes = R"svg(width="100" height="100")svg") {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" " + attributes +
           ">" + body + "</svg>";
}
bool near(const double a, const double b, const double tolerance = 1e-6) { return std::abs(a - b) <= tolerance; }

// Every shape in painting order.
void collect(const SvgElement& e, std::vector<const SvgElement*>& out) {
    if (e.kind == SvgElement::Kind::Shape)
        out.push_back(&e);
    for (const auto& child : e.children)
        collect(child, out);
}
const SvgElement* find(const SvgElement& e, const std::string& name) {
    if (e.name == name)
        return &e;
    for (const auto& child : e.children)
        if (const auto* found = find(child, name))
            return found;
    return nullptr;
}
bool skippedMentions(const SvgScene& scene, const std::string& word) {
    return std::ranges::any_of(scene.skipped, [&](const SvgSkipped& s) {
        return s.reason.find(word) != std::string::npos || s.name.find(word) != std::string::npos;
    });
}
bool approximates(const SvgScene& scene, const std::string& word) {
    std::vector<const SvgElement*> all;
    collect(scene.root, all);
    return std::ranges::any_of(all, [&](const SvgElement* e) {
        return std::ranges::any_of(e->approximations, [&](const std::string& s) { return s.find(word) != std::string::npos; });
    });
}

void shapesNamesAndGroups() {
    const auto r = parse(wrap(R"svg(<g id="logo"><rect id="box" x="10" y="20" width="30" height="40" fill="#ff0000"/>
        <g inkscape:label="Letters" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"><circle id="dot" cx="50" cy="50" r="10"/></g></g>
        <ellipse cx="5" cy="5" rx="2" ry="3" stroke="blue" fill="none"/>)svg"));
    CHECK(r.error == SvgError::None);
    if (!r.scene)
        return;
    const auto& scene = *r.scene;
    CHECK(near(scene.width, 100) && near(scene.height, 100));
    CHECK(scene.shapeCount == 3);
    const auto* logo = find(scene.root, "logo");
    CHECK(logo != nullptr && logo->kind == SvgElement::Kind::Group && logo->children.size() == 2);
    CHECK(find(scene.root, "Letters") != nullptr);
    const auto* box = find(scene.root, "box");
    CHECK(box != nullptr && box->contours.size() == 1 && box->contours[0].closed && box->contours[0].anchors.size() == 4);
    CHECK(box != nullptr && box->fill.kind == SvgPaint::Kind::Color && near(box->fill.color.red, 1.0) && near(box->fill.color.green, 0.0));
    const auto* dot = find(scene.root, "dot");
    // Default fill is black.
    CHECK(dot != nullptr && dot->fill.kind == SvgPaint::Kind::Color && near(dot->fill.color.red, 0.0) && dot->stroke.kind == SvgPaint::Kind::None);
}

void transformsAndViewBox() {
    const auto r = parse(wrap(R"svg(<g transform="translate(10 20) scale(2)"><rect id="r" x="1" y="1" width="2" height="3"/></g>)svg",
                              R"svg(width="200" height="100" viewBox="0 0 100 50")svg"));
    CHECK(r.error == SvgError::None && r.scene);
    if (!r.scene)
        return;
    CHECK(near(r.scene->width, 200) && near(r.scene->height, 100));
    const auto* rect = find(r.scene->root, "r");
    CHECK(rect != nullptr);
    if (rect == nullptr)
        return;
    // viewBox scale 2, then translate(10 20) scale(2): x = 2 * (10 + 2 * 1) = 24, y = 2 * (20 + 2) = 44.
    double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    for (const auto& a : rect->contours[0].anchors) {
        minX = std::min(minX, a.point.x);
        maxX = std::max(maxX, a.point.x);
        minY = std::min(minY, a.point.y);
        maxY = std::max(maxY, a.point.y);
    }
    CHECK(near(minX, 24) && near(minY, 44) && near(maxX, 32) && near(maxY, 56));
    // A stroke width is scaled by the mean scale.
    const auto s = parse(wrap(R"svg(<rect id="r" width="5" height="5" stroke="#000" stroke-width="3" transform="scale(2)"/>)svg"));
    const auto* stroked = s.scene ? find(s.scene->root, "r") : nullptr;
    CHECK(stroked != nullptr && near(stroked->strokeWidth, 6.0));
    // preserveAspectRatio centres a viewBox that does not match.
    const auto m = parse(wrap(R"svg(<rect id="r" width="10" height="10"/>)svg", R"svg(width="100" height="50" viewBox="0 0 10 10")svg"));
    const auto* mid = m.scene ? find(m.scene->root, "r") : nullptr;
    CHECK(mid != nullptr && near(mid->contours[0].anchors[0].point.x, 25) && near(mid->contours[0].anchors[2].point.x, 75));
}

void everyPathCommand() {
    const auto r = parse(wrap(R"svg(<path id="p" d="M10 10 L20 10 H30 V20 C30 30 20 30 20 20 S10 10 10 20 Q5 25 10 30 T20 30 A5 5 0 0 1 30 30 a5 5 0 1 0 -10 0 z
        m 40 40 l 5 5 5 -5 M60 60 70 70 80 60"/>)svg"));
    CHECK(r.error == SvgError::None && r.scene);
    const auto* p = r.scene ? find(r.scene->root, "p") : nullptr;
    CHECK(p != nullptr && p->contours.size() == 3);
    if (p == nullptr || p->contours.size() != 3)
        return;
    CHECK(p->contours[0].closed && !p->contours[1].closed && !p->contours[2].closed);
    // The first sub-path ends on the arc's end point before it closes.
    const auto& first = p->contours[0];
    CHECK(first.anchors.size() > 8);
    // A half circle arc: its middle sits on the circle of radius 5 around (25, 30).
    // (Anchors from the first A command: end at (30, 30), previous (20, 30).)
    bool onCircle = false;
    for (const auto& a : first.anchors) {
        if (a.hasIn || a.hasOut)
            continue;
    }
    for (const auto& a : first.anchors)
        if (near(std::hypot(a.point.x - 25, a.point.y - 30), 5, 1e-6) && a.point.y < 29)
            onCircle = true;
    CHECK(onCircle);
    // Implicit lineto after M: "M60 60 70 70 80 60" is three anchors.
    CHECK(p->contours[2].anchors.size() == 3);
    // Relative m after z starts from the sub-path start (10, 10): (50, 50).
    CHECK(near(p->contours[1].anchors[0].point.x, 50) && near(p->contours[1].anchors[0].point.y, 50));
    // Packed arc flags.
    const auto flags = parse(wrap(R"svg(<path id="a" d="M0 0a5 5 0 1110 0"/>)svg"));
    const auto* a = flags.scene ? find(flags.scene->root, "a") : nullptr;
    CHECK(a != nullptr && !a->contours.empty() && near(a->contours[0].anchors.back().point.x, 10));
    // An error keeps what was read before it.
    const auto broken = parse(wrap(R"svg(<path id="b" d="M0 0 L10 10 L oops"/>)svg"));
    const auto* b = broken.scene ? find(broken.scene->root, "b") : nullptr;
    CHECK(b != nullptr && b->contours.size() == 1 && b->contours[0].anchors.size() == 2 && !b->approximations.empty());
}

void styleSheetsAndInheritance() {
    const auto r = parse(wrap(R"svg(<style>
        .a { fill: #00ff00; stroke: #000; stroke-width: 4 }
        g.group .b { fill: rgb(0, 0, 255) }
        #special { fill: orange }
        rect { stroke-linejoin: round }
        g > .c { fill: #123456 }
        .a { fill: #ff00ff }
        </style>
        <g class="group" fill="red">
          <rect id="one" class="a" width="5" height="5"/>
          <rect id="two" class="b" width="5" height="5"/>
          <rect id="three" class="a" id2="x" style="fill:#abcdef" width="5" height="5"/>
          <rect id="special" class="a" width="5" height="5"/>
          <g><rect id="four" class="c" width="5" height="5"/></g>
          <rect id="five" width="5" height="5"/>
        </g>)svg"));
    CHECK(r.error == SvgError::None && r.scene);
    if (!r.scene)
        return;
    const auto get = [&](const char* name) { return find(r.scene->root, name); };
    // Later rule of equal specificity wins.
    CHECK(near(get("one")->fill.color.red, 1.0) && near(get("one")->fill.color.green, 0.0) && near(get("one")->fill.color.blue, 1.0));
    CHECK(near(get("one")->strokeWidth, 4.0) && get("one")->join == SvgJoin::Round);
    // The descendant selector beats the presentation attribute inherited from the group.
    CHECK(near(get("two")->fill.color.blue, 1.0));
    // A style attribute beats a class.
    CHECK(near(get("three")->fill.color.red, 0xab / 255.0));
    // An id selector beats a class.
    CHECK(near(get("special")->fill.color.red, 1.0) && near(get("special")->fill.color.green, 165 / 255.0));
    // The child combinator: "four" sits directly in a g.
    CHECK(near(get("four")->fill.color.red, 0x12 / 255.0) && near(get("four")->fill.color.blue, 0x56 / 255.0));
    // Inherited presentation attribute.
    CHECK(near(get("five")->fill.color.red, 1.0) && near(get("five")->fill.color.green, 0.0));
}

void useSymbolsAndDefs() {
    const auto r = parse(wrap(R"svg(<defs>
        <path id="leaf" d="M0 0 L10 0 L10 10 Z" fill="#ff0000"/>
        <g id="pair"><rect id="inner" width="4" height="4"/><circle r="2" cx="10" cy="10"/></g>
        <symbol id="sym" viewBox="0 0 10 10"><rect id="symrect" width="10" height="10"/></symbol>
        </defs>
        <use id="u1" href="#leaf" x="20" y="30"/>
        <use id="u2" xlink:href="#pair" transform="translate(50 50)" fill="#00ff00"/>
        <use id="u3" href="#sym" x="60" y="0" width="20" height="20"/>)svg"));
    CHECK(r.error == SvgError::None && r.scene);
    if (!r.scene)
        return;
    CHECK(r.scene->shapeCount == 4); // leaf, rect, circle, symbol rect. Definitions draw nothing themselves.
    const auto* u1 = find(r.scene->root, "u1");
    CHECK(u1 != nullptr && u1->kind == SvgElement::Kind::Group && u1->children.size() == 1);
    CHECK(u1 != nullptr && near(u1->children[0].contours[0].anchors[0].point.x, 20) && near(u1->children[0].contours[0].anchors[0].point.y, 30));
    // The use element's presentation attribute reaches the referenced group's children.
    const auto* inner = find(r.scene->root, "inner");
    CHECK(inner != nullptr && near(inner->fill.color.green, 1.0) && near(inner->contours[0].anchors[0].point.x, 50));
    // A symbol's view box maps to the use's size: scale 2, at x = 60.
    const auto* symrect = find(r.scene->root, "symrect");
    CHECK(symrect != nullptr && near(symrect->contours[0].anchors[2].point.x, 80) && near(symrect->contours[0].anchors[2].point.y, 20));
}

void gradients() {
    const auto r = parse(wrap(R"svg(<defs>
        <linearGradient id="base" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff" stop-opacity=".5"/></linearGradient>
        <linearGradient id="derived" href="#base" gradientTransform="rotate(90)" spreadMethod="reflect"/>
        <radialGradient id="rad" gradientUnits="userSpaceOnUse" cx="50" cy="50" r="20"><stop offset="0" style="stop-color:white"/><stop offset="1" style="stop-color:black"/></radialGradient>
        <radialGradient id="ell" gradientTransform="scale(2 1)"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#000"/></radialGradient>
        </defs>
        <rect id="a" x="10" y="10" width="40" height="20" fill="url(#base)"/>
        <rect id="b" x="10" y="10" width="40" height="20" fill="url(#derived)"/>
        <circle id="c" cx="50" cy="50" r="30" fill="url(#rad)" stroke="url(#base)" stroke-width="2"/>
        <rect id="d" width="10" height="10" fill="url(#ell)"/>
        <rect id="e" width="10" height="10" fill="url(#missing) #123456"/>)svg"));
    CHECK(r.error == SvgError::None && r.scene);
    if (!r.scene)
        return;
    const auto& scene = *r.scene;
    const auto* a = find(scene.root, "a");
    CHECK(a != nullptr && a->fill.kind == SvgPaint::Kind::Gradient);
    if (a == nullptr || a->fill.kind != SvgPaint::Kind::Gradient)
        return;
    const auto& ga = scene.gradients[a->fill.gradient];
    // Bounding box units: the line runs along the box's top from x = 10 to x = 50.
    CHECK(!ga.radial && near(ga.p0.x, 10) && near(ga.p0.y, 10) && near(ga.p1.x, 50) && near(ga.p1.y, 10, 1e-6));
    CHECK(ga.stops.size() == 2 && near(ga.stops[1].color.alpha, 0.5) && near(ga.stops[1].color.blue, 1.0));
    // The derived gradient takes the stops from its reference and turns the line a quarter turn,
    // in the box's own (non-uniform) space: from (10, 10) down by the box height... along +y.
    const auto* b = find(scene.root, "b");
    CHECK(b != nullptr && b->fill.kind == SvgPaint::Kind::Gradient);
    if (b != nullptr && b->fill.kind == SvgPaint::Kind::Gradient) {
        const auto& gb = scene.gradients[b->fill.gradient];
        CHECK(gb.stops.size() == 2 && gb.spread == SvgSpread::Reflect);
        // t is projection on the line, so the line must be perpendicular to the lines of constant t.
        // In the unit box rotate(90) maps (0,0)-(1,0) to (0,0)-(0,1): the box's height, 20.
        CHECK(near(gb.p0.x, 10) && near(gb.p0.y, 10) && near(gb.p1.x, 10, 1e-6) && near(gb.p1.y, 30, 1e-6));
    }
    const auto* c = find(scene.root, "c");
    CHECK(c != nullptr && c->fill.kind == SvgPaint::Kind::Gradient && c->stroke.kind == SvgPaint::Kind::Gradient);
    if (c != nullptr && c->fill.kind == SvgPaint::Kind::Gradient) {
        const auto& gc = scene.gradients[c->fill.gradient];
        CHECK(gc.radial && near(gc.center.x, 50) && near(gc.center.y, 50) && near(gc.radius, 20) && !gc.approximate);
    }
    // An elliptical radial gradient keeps its linear map, so it can be placed exactly.
    const auto* d = find(scene.root, "d");
    if (d != nullptr && d->fill.kind == SvgPaint::Kind::Gradient) {
        const auto& gd = scene.gradients[d->fill.gradient];
        CHECK(gd.radial && !gd.approximate && near(gd.linearA, 20) && near(gd.linearD, 10) && near(gd.userRadius, 0.5));
    } else {
        CHECK(false);
    }
    // A reference that is not there falls back to the paint after it.
    const auto* e = find(scene.root, "e");
    CHECK(e != nullptr && e->fill.kind == SvgPaint::Kind::Color && near(e->fill.color.red, 0x12 / 255.0));
}

void clipsMasksAndOpacity() {
    const auto r = parse(wrap(R"svg(<defs>
        <clipPath id="cp"><circle cx="20" cy="20" r="10"/><rect x="30" y="30" width="10" height="10"/></clipPath>
        <mask id="mk"><rect width="100" height="100" fill="#fff"/></mask>
        </defs>
        <g id="clipped" clip-path="url(#cp)" opacity=".5"><rect id="r" width="100" height="100"/></g>
        <rect id="masked" width="10" height="10" mask="url(#mk)" opacity=".25"/>
        <rect id="gone" width="10" height="10" clip-path="url(#nowhere)"/>)svg"));
    CHECK(r.error == SvgError::None && r.scene);
    if (!r.scene)
        return;
    const auto* clipped = find(r.scene->root, "clipped");
    CHECK(clipped != nullptr && clipped->clip && clipped->clip->children.size() == 2 && near(clipped->opacity, 0.5));
    const auto* masked = find(r.scene->root, "masked");
    CHECK(masked != nullptr && masked->mask && masked->mask->children.size() == 1 && near(masked->opacity, 0.25));
    CHECK(skippedMentions(*r.scene, "nowhere"));
}

void dashesAndStrokes() {
    const auto r = parse(wrap(R"svg(<path id="p" d="M0 0 H50" fill="none" stroke="#000" stroke-width="3" stroke-dasharray="10 5 2" stroke-dashoffset="4"
        stroke-linecap="round" stroke-linejoin="bevel" stroke-miterlimit="2"/>)svg"));
    const auto* p = r.scene ? find(r.scene->root, "p") : nullptr;
    CHECK(p != nullptr);
    if (p == nullptr)
        return;
    // An odd list repeats to make an even one.
    CHECK(p->dashes.size() == 6 && near(p->dashes[0], 10) && near(p->dashes[2], 2) && near(p->dashes[3], 10) && near(p->dashOffset, 4));
    CHECK(p->cap == SvgCap::Round && p->join == SvgJoin::Bevel && near(p->miterLimit, 2));
}

void whatIsNotImportedIsListed() {
    const auto r = parse(wrap(R"svg(<text id="title" x="5" y="5">Hello</text>
        <image id="photo" href="photo.png" width="10" height="10"/>
        <script>alert(1)</script>
        <rect id="hidden" width="5" height="5" display="none"/>
        <rect id="ghost" width="5" height="5" fill="none"/>
        <rect id="filtered" width="5" height="5" filter="url(#f)"/>
        <rect id="marked" width="5" height="5" marker-start="url(#m)"/>
        <rect id="textured" width="5" height="5" fill="url(#pat)"/>
        <defs><pattern id="pat" width="2" height="2"><rect width="1" height="1"/></pattern></defs>
        <rect id="visible" width="5" height="5"/>)svg"));
    CHECK(r.error == SvgError::None && r.scene);
    if (!r.scene)
        return;
    CHECK(find(r.scene->root, "title") != nullptr && find(r.scene->root, "title")->kind == SvgElement::Kind::Text &&
          skippedMentions(*r.scene, "photo") && skippedMentions(*r.scene, "script"));
    CHECK(skippedMentions(*r.scene, "hidden") && skippedMentions(*r.scene, "ghost"));
    CHECK(approximates(*r.scene, "filter") && approximates(*r.scene, "markers") && skippedMentions(*r.scene, "pattern"));
    CHECK(find(r.scene->root, "visible") != nullptr);
    // A pattern-filled shape with nothing else to draw is listed, not silently dropped.
    CHECK(find(r.scene->root, "textured") == nullptr && skippedMentions(*r.scene, "textured"));
}

void textRuns() {
    const auto r = parse(wrap(R"svg(<g transform="translate(10 20)"><text id="t" x="5" y="30" font-size="12" text-anchor="middle" fill="#00f">One   two<tspan id="t2" x="5" y="50" fill="#f00">Three</tspan></text></g>)svg"));
    CHECK(r.error == SvgError::None && r.scene);
    if (!r.scene)
        return;
    const auto* a = find(r.scene->root, "t");
    const auto* b = find(r.scene->root, "t2");
    CHECK(a != nullptr && a->kind == SvgElement::Kind::Text && a->text == "One two" && near(a->textX, 15) &&
          near(a->textY, 50) && near(a->fontSize, 12) && a->textAnchor == 1);
    CHECK(b != nullptr && b->text == "Three" && near(b->textY, 70) && b->fill.color.red > 0.9);
    CHECK(r.scene->textCount == 2);
}

void nestedSvg() {
    const auto r = parse(wrap(R"svg(<svg id="inner" x="10" y="10" width="20" height="20" viewBox="0 0 10 10"><rect id="r" width="10" height="10"/></svg>)svg"));
    CHECK(r.error == SvgError::None && r.scene);
    const auto* inner = r.scene ? find(r.scene->root, "inner") : nullptr;
    CHECK(inner != nullptr && inner->clip != nullptr);
    const auto* rect = r.scene ? find(r.scene->root, "r") : nullptr;
    CHECK(rect != nullptr && near(rect->contours[0].anchors[0].point.x, 10) && near(rect->contours[0].anchors[2].point.x, 30));
}

// ---- hostile and malformed ----

void hostileFiles() {
    // Entity expansion: a declared value may not refer to another entity.
    const auto laughs = parse(R"svg(<?xml version="1.0"?><!DOCTYPE svg [<!ENTITY a "aaaaaaaaaa"><!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;">]>
        <svg xmlns="http://www.w3.org/2000/svg"><text>&b;</text><rect width="1" height="1"/></svg>)svg");
    CHECK(laughs.error == SvgError::TooComplex && laughs.scene == nullptr);
    // External entities are never read.
    const auto external = parse(R"svg(<!DOCTYPE svg [<!ENTITY x SYSTEM "file:///etc/passwd">]><svg xmlns="http://www.w3.org/2000/svg"><rect width="1" height="1"/></svg>)svg");
    CHECK(external.error == SvgError::Malformed && external.scene == nullptr);
    const auto parameter = parse(R"svg(<!DOCTYPE svg [<!ENTITY % p "x">]><svg xmlns="http://www.w3.org/2000/svg"><rect width="1" height="1"/></svg>)svg");
    CHECK(parameter.error != SvgError::None);
    // A document type that only declares namespace names, as common design tools write, is read.
    const auto namespaces = parse(R"svg(<?xml version="1.0"?><!DOCTYPE svg [<!ENTITY ns_svg "http://www.w3.org/2000/svg">]>
        <svg xmlns="&ns_svg;" width="10" height="10"><rect id="r" width="5" height="5"/></svg>)svg");
    CHECK(namespaces.error == SvgError::None && namespaces.scene && namespaces.scene->shapeCount == 1);
    // Malformed markup.
    for (const char* bad : {"<svg xmlns=\"http://www.w3.org/2000/svg\"><!-- never closed", "<svg><rect></svg>",
                            "<svg><g></svg>", "<svg xmlns=\"x\" a=b/>", "<svg><![CDATA[ x", "", "plain text",
                            "<svg><rect/></svg><svg/>", "<html><body/></html>"}) {
        const auto result = parse(bad);
        CHECK(result.error != SvgError::None && result.scene == nullptr && !result.diagnostic.empty());
    }
    // Nesting beyond the bound.
    std::string deep = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\">";
    for (int i = 0; i < 200; ++i)
        deep += "<g>";
    deep += "<rect width=\"1\" height=\"1\"/>";
    for (int i = 0; i < 200; ++i)
        deep += "</g>";
    deep += "</svg>";
    CHECK(parse(deep).error == SvgError::TooComplex);
    // A use that refers to itself or its own ancestor ends, and the rest of the file is read.
    const auto cycle = parse(wrap(R"svg(<g id="g"><use id="loop" href="#g"/><rect id="ok" width="3" height="3"/></g><use id="self" href="#self"/>)svg"));
    CHECK(cycle.error == SvgError::None && cycle.scene && find(cycle.scene->root, "ok") != nullptr && skippedMentions(*cycle.scene, "ancestors"));
    // Exponential use nesting is refused, not expanded.
    std::string bomb = "<defs><rect id=\"u0\" width=\"1\" height=\"1\"/>";
    for (int i = 1; i <= 12; ++i) {
        bomb += "<g id=\"u" + std::to_string(i) + "\">";
        for (int k = 0; k < 10; ++k)
            bomb += "<use href=\"#u" + std::to_string(i - 1) + "\"/>";
        bomb += "</g>";
    }
    bomb += "</defs><use href=\"#u12\"/>";
    const auto exploded = parse(wrap(bomb));
    CHECK(exploded.error == SvgError::TooComplex && exploded.scene == nullptr);
    // Nothing outside the file is followed or run.
    const auto external2 = parse(wrap(R"svg(<use id="far" href="http://example.invalid/a.svg#x"/><rect id="r" width="2" height="2" fill="url(http://example.invalid/g.svg#x)"/><rect id="shown" width="2" height="2"/><script>boom()</script>)svg"));
    CHECK(external2.error == SvgError::None && external2.scene && skippedMentions(*external2.scene, "external"));
    // Absurd sizes.
    CHECK(parse(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="1e12" height="10"><rect width="1" height="1"/></svg>)svg").error == SvgError::TooComplex);
    // Nothing to draw.
    CHECK(parse(wrap("<g/>")).error == SvgError::Empty);
    // Too many drawn elements.
    std::string many;
    for (int i = 0; i < 20'100; ++i)
        many += "<rect width=\"1\" height=\"1\"/>";
    CHECK(parse(wrap(many)).error == SvgError::TooComplex);
    // A contour beyond the Path value's bound is listed and skipped, not truncated.
    std::string longPath = "M0 0";
    for (int i = 1; i <= 4100; ++i)
        longPath += " L" + std::to_string(i) + " " + std::to_string(i % 7);
    const auto big = parse(wrap("<path id=\"big\" d=\"" + longPath + "\"/><rect id=\"fine\" width=\"2\" height=\"2\"/>"));
    CHECK(big.error == SvgError::None && big.scene && find(big.scene->root, "big") == nullptr && skippedMentions(*big.scene, "4096"));
    // Garbage numbers and transforms do not crash or produce NaN.
    const auto junk = parse(wrap(R"svg(<rect id="r" width="nan" height="inf" transform="matrix(1e999 0 0 1 0 0)"/><circle id="c" r="5" cx="1e400"/><rect id="ok" width="1" height="1"/>)svg"));
    CHECK(junk.error == SvgError::None && junk.scene && find(junk.scene->root, "ok") != nullptr);
    // Cancellation stops a read.
    std::atomic<int> polls{0};
    const auto cancelled = parse(wrap(many), [&] { return ++polls > 1; });
    CHECK(cancelled.error == SvgError::Cancelled || cancelled.error == SvgError::TooComplex);
    std::string big2(40U * 1024U * 1024U, ' ');
    CHECK(parse(big2).error == SvgError::TooLarge);
}

} // namespace

int main() {
    shapesNamesAndGroups();
    transformsAndViewBox();
    everyPathCommand();
    styleSheetsAndInheritance();
    useSymbolsAndDefs();
    gradients();
    clipsMasksAndOpacity();
    dashesAndStrokes();
    whatIsNotImportedIsListed();
    textRuns();
    nestedSvg();
    hostileFiles();
    if (failures != 0)
        std::fprintf(stderr, "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
