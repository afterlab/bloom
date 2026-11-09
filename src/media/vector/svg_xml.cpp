#include "svg_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <map>

namespace bloom::media::svg {
namespace {

bool isSpace(const char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
bool isNameStart(const char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == ':' ||
           (static_cast<unsigned char>(c) & 0x80U) != 0;
}
bool isNameChar(const char c) noexcept {
    return isNameStart(c) || std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '.';
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU))
        cp = 0xFFFDU;
    if (cp < 0x80U) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

class Reader final {
  public:
    Reader(std::string_view text, const SvgCancel& cancel) : text_(text), cancel_(cancel) {}

    XmlDocument run() {
        XmlDocument document;
        document_ = &document;
        parse();
        if (document.error == SvgError::None && document.nodes.empty())
            fail(SvgError::NotSvg, "The file has no root element, so it is not an SVG document.");
        if (document.error == SvgError::None && depth_ != 0)
            fail(SvgError::Malformed, "The markup ends inside an element that is never closed.");
        return document;
    }

  private:
    std::string_view text_;
    const SvgCancel& cancel_;
    XmlDocument* document_ = nullptr;
    std::size_t pos_ = 0;
    std::size_t depth_ = 0;
    std::vector<std::size_t> stack_;
    std::map<std::string, std::string> entities_;
    bool rootClosed_ = false;

    bool failed() const noexcept { return document_->error != SvgError::None; }
    void fail(const SvgError error, std::string message) {
        if (failed())
            return;
        document_->error = error;
        document_->diagnostic = std::move(message);
    }
    bool atEnd() const noexcept { return pos_ >= text_.size(); }
    bool startsWith(std::string_view token) const noexcept {
        return text_.substr(pos_, token.size()) == token;
    }
    void skipSpace() {
        while (!atEnd() && isSpace(text_[pos_]))
            ++pos_;
    }

    void parse() {
        // A byte order mark.
        if (startsWith("\xEF\xBB\xBF"))
            pos_ += 3;
        std::size_t sinceCancel = 0;
        while (!atEnd() && !failed()) {
            if (++sinceCancel >= 4096) {
                sinceCancel = 0;
                try {
                    if (cancel_ && cancel_()) {
                        fail(SvgError::Cancelled, "The import was cancelled.");
                        return;
                    }
                } catch (...) {
                    fail(SvgError::Cancelled, "The import was cancelled.");
                    return;
                }
            }
            if (text_[pos_] != '<') {
                const auto next = text_.find('<', pos_);
                const auto end = next == std::string_view::npos ? text_.size() : next;
                if (!stack_.empty())
                    appendText(text_.substr(pos_, end - pos_));
                pos_ = end;
                continue;
            }
            if (startsWith("<?")) {
                const auto end = text_.find("?>", pos_ + 2);
                if (end == std::string_view::npos)
                    return fail(SvgError::Malformed, "A processing instruction is never closed.");
                pos_ = end + 2;
            } else if (startsWith("<!--")) {
                const auto end = text_.find("-->", pos_ + 4);
                if (end == std::string_view::npos)
                    return fail(SvgError::Malformed, "A comment is never closed.");
                pos_ = end + 3;
            } else if (startsWith("<![CDATA[")) {
                const auto end = text_.find("]]>", pos_ + 9);
                if (end == std::string_view::npos)
                    return fail(SvgError::Malformed, "A data section is never closed.");
                if (!stack_.empty())
                    document_->nodes[stack_.back()].text.append(text_.substr(pos_ + 9, end - pos_ - 9));
                pos_ = end + 3;
            } else if (startsWith("<!DOCTYPE") || startsWith("<!doctype")) {
                parseDoctype();
            } else if (startsWith("<!")) {
                return fail(SvgError::Malformed, "The markup holds a declaration that is not allowed.");
            } else if (startsWith("</")) {
                parseClose();
            } else {
                parseOpen();
            }
        }
    }

    void appendText(std::string_view raw) {
        auto& target = document_->nodes[stack_.back()].text;
        if (target.size() > kMaxSvgStyleBytes)
            return;
        std::string decoded;
        decode(raw, decoded);
        target += decoded;
    }

    // Decodes the five predefined entities, character references and the document's own entities.
    void decode(std::string_view raw, std::string& out) {
        out.reserve(out.size() + raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') {
                out.push_back(raw[i]);
                continue;
            }
            const auto semi = raw.find(';', i + 1);
            if (semi == std::string_view::npos || semi - i > 40) {
                out.push_back('&');
                continue;
            }
            const auto name = raw.substr(i + 1, semi - i - 1);
            if (name == "lt")
                out.push_back('<');
            else if (name == "gt")
                out.push_back('>');
            else if (name == "amp")
                out.push_back('&');
            else if (name == "quot")
                out.push_back('"');
            else if (name == "apos")
                out.push_back('\'');
            else if (!name.empty() && name[0] == '#') {
                std::uint32_t cp = 0;
                const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
                bool ok = name.size() > (hex ? 2U : 1U);
                for (std::size_t k = hex ? 2 : 1; k < name.size() && ok; ++k) {
                    const char c = name[k];
                    const int digit = std::isdigit(static_cast<unsigned char>(c)) != 0 ? c - '0'
                                      : hex && c >= 'a' && c <= 'f'                    ? c - 'a' + 10
                                      : hex && c >= 'A' && c <= 'F'                    ? c - 'A' + 10
                                                                                       : -1;
                    if (digit < 0 || cp > 0x10FFFFU)
                        ok = false;
                    else
                        cp = cp * (hex ? 16U : 10U) + static_cast<std::uint32_t>(digit);
                }
                if (ok)
                    appendUtf8(out, cp);
                else
                    out.append(raw.substr(i, semi - i + 1));
            } else if (const auto found = entities_.find(std::string(name)); found != entities_.end()) {
                // Declared values never contain a reference (checked at declaration), so this is one
                // level, whatever the document declares.
                out += found->second;
            } else {
                out.append(raw.substr(i, semi - i + 1));
            }
            i = semi;
        }
    }

    void parseDoctype() {
        pos_ += 9;
        while (!atEnd() && !failed()) {
            const char c = text_[pos_];
            if (c == '>') {
                ++pos_;
                return;
            }
            if (c == '[') {
                ++pos_;
                parseInternalSubset();
                continue;
            }
            if (c == '"' || c == '\'') {
                const auto end = text_.find(c, pos_ + 1);
                if (end == std::string_view::npos)
                    return fail(SvgError::Malformed, "A declaration is never closed.");
                pos_ = end + 1;
                continue;
            }
            ++pos_;
        }
        fail(SvgError::Malformed, "A declaration is never closed.");
    }

    void parseInternalSubset() {
        while (!atEnd() && !failed()) {
            skipSpace();
            if (atEnd())
                break;
            if (text_[pos_] == ']') {
                ++pos_;
                return;
            }
            if (startsWith("<!--")) {
                const auto end = text_.find("-->", pos_ + 4);
                if (end == std::string_view::npos)
                    return fail(SvgError::Malformed, "A comment is never closed.");
                pos_ = end + 3;
            } else if (startsWith("<!ENTITY")) {
                pos_ += 8;
                skipSpace();
                if (!atEnd() && text_[pos_] == '%')
                    return fail(SvgError::Malformed,
                                "The file declares a parameter entity, which is not supported.");
                const auto nameStart = pos_;
                while (!atEnd() && isNameChar(text_[pos_]))
                    ++pos_;
                const std::string name(text_.substr(nameStart, pos_ - nameStart));
                skipSpace();
                if (atEnd() || (text_[pos_] != '"' && text_[pos_] != '\''))
                    return fail(SvgError::Malformed,
                                "The file declares an external entity, which is never read.");
                const char quote = text_[pos_];
                const auto end = text_.find(quote, pos_ + 1);
                if (end == std::string_view::npos)
                    return fail(SvgError::Malformed, "An entity declaration is never closed.");
                const auto value = text_.substr(pos_ + 1, end - pos_ - 1);
                if (value.find('&') != std::string_view::npos || value.find('<') != std::string_view::npos ||
                    value.size() > 512 || entities_.size() >= kMaxSvgEntities)
                    return fail(SvgError::TooComplex,
                                "The file declares an entity whose value refers to another entity or "
                                "markup, or declares too many entities, so it was not read.");
                entities_[name] = std::string(value);
                const auto close = text_.find('>', end);
                if (close == std::string_view::npos)
                    return fail(SvgError::Malformed, "An entity declaration is never closed.");
                pos_ = close + 1;
            } else if (startsWith("<!")) {
                // ELEMENT / ATTLIST / NOTATION: nothing here is used.
                const auto close = text_.find('>', pos_);
                if (close == std::string_view::npos)
                    return fail(SvgError::Malformed, "A declaration is never closed.");
                pos_ = close + 1;
            } else if (startsWith("<?")) {
                const auto end = text_.find("?>", pos_ + 2);
                if (end == std::string_view::npos)
                    return fail(SvgError::Malformed, "A processing instruction is never closed.");
                pos_ = end + 2;
            } else {
                return fail(SvgError::Malformed, "The document type holds markup that is not allowed.");
            }
        }
    }

    void parseClose() {
        pos_ += 2;
        const auto start = pos_;
        while (!atEnd() && isNameChar(text_[pos_]))
            ++pos_;
        skipSpace();
        if (atEnd() || text_[pos_] != '>')
            return fail(SvgError::Malformed, "A closing tag is malformed.");
        ++pos_;
        if (stack_.empty())
            return fail(SvgError::Malformed, "A closing tag has no opening tag.");
        const auto name = text_.substr(start, pos_ - 1 - start);
        const auto& open = document_->nodes[stack_.back()];
        const auto trimmed = trim(name);
        const auto colon = trimmed.rfind(':');
        const auto local = colon == std::string_view::npos ? trimmed : trimmed.substr(colon + 1);
        const auto openColon = open.tag.rfind(':');
        const auto openLocal = openColon == std::string::npos ? std::string_view(open.tag)
                                                              : std::string_view(open.tag).substr(openColon + 1);
        if (local != openLocal)
            return fail(SvgError::Malformed, "A closing tag does not match its opening tag.");
        stack_.pop_back();
        --depth_;
        if (stack_.empty())
            rootClosed_ = true;
    }

    void parseOpen() {
        ++pos_;
        if (atEnd() || !isNameStart(text_[pos_]))
            return fail(SvgError::Malformed, "A tag has no name.");
        const auto nameStart = pos_;
        while (!atEnd() && isNameChar(text_[pos_]))
            ++pos_;
        const std::string qualified(text_.substr(nameStart, pos_ - nameStart));
        XmlNode node;
        const auto colon = qualified.find(':');
        if (colon == std::string::npos || qualified.substr(0, colon) == "svg") {
            node.tag = colon == std::string::npos ? qualified : qualified.substr(colon + 1);
        } else {
            node.tag = qualified;
            node.foreign = true;
        }
        bool selfClosing = false;
        while (!failed()) {
            skipSpace();
            if (atEnd())
                return fail(SvgError::Malformed, "A tag is never closed.");
            if (text_[pos_] == '>') {
                ++pos_;
                break;
            }
            if (startsWith("/>")) {
                pos_ += 2;
                selfClosing = true;
                break;
            }
            if (!isNameStart(text_[pos_]))
                return fail(SvgError::Malformed, "An attribute has no name.");
            const auto attrStart = pos_;
            while (!atEnd() && isNameChar(text_[pos_]))
                ++pos_;
            std::string name(text_.substr(attrStart, pos_ - attrStart));
            skipSpace();
            if (atEnd() || text_[pos_] != '=')
                return fail(SvgError::Malformed, "An attribute has no value.");
            ++pos_;
            skipSpace();
            if (atEnd() || (text_[pos_] != '"' && text_[pos_] != '\''))
                return fail(SvgError::Malformed, "An attribute value is not quoted.");
            const char quote = text_[pos_];
            const auto end = text_.find(quote, pos_ + 1);
            if (end == std::string_view::npos)
                return fail(SvgError::Malformed, "An attribute value is never closed.");
            std::string value;
            decode(text_.substr(pos_ + 1, end - pos_ - 1), value);
            pos_ = end + 1;
            if (name == "xlink:href")
                name = "href";
            if (std::ranges::none_of(node.attributes, [&](const auto& a) { return a.name == name; }))
                node.attributes.push_back({std::move(name), std::move(value)});
            if (node.attributes.size() > 4096)
                return fail(SvgError::TooComplex, "An element has too many attributes.");
        }
        if (failed())
            return;
        if (document_->nodes.size() >= kMaxSvgXmlNodes)
            return fail(SvgError::TooComplex, "The file has too many elements to read.");
        if (stack_.empty()) {
            if (rootClosed_ || !document_->nodes.empty())
                return fail(SvgError::Malformed, "The document has more than one root element.");
            if (node.tag != "svg" || node.foreign)
                return fail(SvgError::NotSvg, "The root element is not <svg>, so the file is not an SVG document.");
        } else {
            node.parent = stack_.back();
        }
        const auto index = document_->nodes.size();
        document_->nodes.push_back(std::move(node));
        if (!stack_.empty())
            document_->nodes[stack_.back()].children.push_back(index);
        if (!selfClosing) {
            if (depth_ + 1 > kMaxSvgNesting)
                return fail(SvgError::TooComplex, "The elements are nested more than 128 levels deep.");
            stack_.push_back(index);
            ++depth_;
        } else if (stack_.empty()) {
            rootClosed_ = true;
        }
    }
};

} // namespace

const std::string* XmlNode::find(const std::string_view name) const noexcept {
    for (const auto& attribute : attributes)
        if (attribute.name == name)
            return &attribute.value;
    return nullptr;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

XmlDocument parseXml(const std::span<const std::byte> bytes, const SvgCancel& cancel) {
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return Reader(text, cancel).run();
}

} // namespace bloom::media::svg
