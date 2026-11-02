#include "product_names.hpp"
#include <iostream>
int main() {
    using bloom::quality::productNameViolations;
    int failures = 0;
    const auto check = [&](const char* path, std::string_view text, std::size_t count) {
        const auto findings = productNameViolations(path, text);
        if (findings.size() != count) {
            std::cerr << "Expected " << count << " product-name findings in " << path << "; got "
                      << findings.size() << " in " << text << '\n';
            ++failures;
        }
    };
    // Documentation is checked in full, across a line break, case-sensitively.
    check("docs/user-guide/example.md", "Works like the After Effects Render Queue.", 1);
    check("docs/ux/example.md", "as in After\nEffects, and Blender's editors", 2);
    check("README.md", "Bloom is not Photoshop, Nuke or DaVinci Resolve.", 3);
    check("docs/example.md", "Resolve the premiere; nuke the cache; the AE keys.", 1);
    check("docs/example.md",
          "A layer-based compositor's render queue; ProRes, OpenEXR, OCIO, ACES.", 0);
    // Source: comments and string literals only; identifiers never match.
    check("src/ui/example.cpp", "// as in After Effects\nint kBlenderAutoClamped = 0;", 1);
    check("src/ui/example.hpp", "/* a Nuke reader */ auto text = \"Illustrator's key\";", 2);
    check("src/ui/example.cpp", "void testBlenderReference(); int Blender = 0;", 0);
    check("src/scripting/example.cpp", "R\"(Photoshop \"quoted\" Premiere)\"; auto x = Maya;", 2);
    check("src/ui/example.cpp", "char q = '\"'; int n = 1'000; // Adobe-standard", 1);
    // Qt's Fusion style is a library name; any other Fusion is the product.
    check("src/ui/kit/example.cpp", "QStyleFactory::create(QStringLiteral(\"Fusion\"));", 0);
    check("src/ui/example.cpp", "// a node graph as in Fusion", 1);
    // Out of scope: third-party text, the owner-edited catalogue, the benchmark guide, other files.
    check("src/ui/kit/third_party/x/example.cpp", "// After Effects", 0);
    check("docs/product/node-catalogue.md", "After Effects", 0);
    check("docs/development/benchmarks.md", "Blender 4.5 LTS", 0);
    check("tools/bench/example.py", "# Blender", 0);
    check("src/document/tests/example.inc", "// Blender", 0);
    return failures == 0 ? 0 : 1;
}
