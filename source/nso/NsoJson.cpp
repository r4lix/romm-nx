#include "NsoJson.hpp"

#include <cctype>

namespace romm::nso::json {

    namespace {

        constexpr int kMaxDepth = 32;

        bool IsWs(char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        // One past the closing quote of the string literal starting at `pos`,
        // or npos. Handles backslash escapes properly: a scan that only checks
        // `text[i-1] != '\\'` misreads a value ending in a literal backslash.
        size_t SkipString(const std::string& text, size_t pos) {
            if (pos >= text.size() || text[pos] != '"') return std::string::npos;
            ++pos;
            while (pos < text.size()) {
                const char c = text[pos];
                if (c == '\\') {
                    pos += 2; // consume the escape and whatever it escapes
                    continue;
                }
                if (c == '"') return pos + 1;
                ++pos;
            }
            return std::string::npos;
        }

        size_t SkipNumber(const std::string& text, size_t pos) {
            const size_t start = pos;
            if (pos < text.size() && text[pos] == '-') ++pos;
            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
            if (pos < text.size() && text[pos] == '.') {
                ++pos;
                while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
            }
            if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
                ++pos;
                if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
                while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
            }
            return (pos == start) ? std::string::npos : pos;
        }

        size_t SkipLiteral(const std::string& text, size_t pos, const char* literal) {
            const size_t n = std::string(literal).size();
            if (text.compare(pos, n, literal) == 0) return pos + n;
            return std::string::npos;
        }

        size_t SkipValueDepth(const std::string& text, size_t pos, int depth);

        size_t SkipContainer(const std::string& text, size_t pos, int depth, char open, char close) {
            if (pos >= text.size() || text[pos] != open) return std::string::npos;
            ++pos;
            pos = SkipWhitespace(text, pos);
            if (pos == std::string::npos) return std::string::npos;
            if (text[pos] == close) return pos + 1; // empty container

            while (true) {
                if (open == '{') {
                    pos = SkipString(text, pos);
                    if (pos == std::string::npos) return std::string::npos;
                    pos = SkipWhitespace(text, pos);
                    if (pos == std::string::npos || text[pos] != ':') return std::string::npos;
                    pos = SkipWhitespace(text, pos + 1);
                    if (pos == std::string::npos) return std::string::npos;
                }
                pos = SkipValueDepth(text, pos, depth + 1);
                if (pos == std::string::npos) return std::string::npos;
                pos = SkipWhitespace(text, pos);
                if (pos == std::string::npos) return std::string::npos;
                if (text[pos] == ',') {
                    pos = SkipWhitespace(text, pos + 1);
                    if (pos == std::string::npos) return std::string::npos;
                    continue;
                }
                if (text[pos] == close) return pos + 1;
                return std::string::npos;
            }
        }

        size_t SkipValueDepth(const std::string& text, size_t pos, int depth) {
            if (depth > kMaxDepth) return std::string::npos;
            if (pos >= text.size()) return std::string::npos;
            switch (text[pos]) {
                case '"': return SkipString(text, pos);
                case '{': return SkipContainer(text, pos, depth, '{', '}');
                case '[': return SkipContainer(text, pos, depth, '[', ']');
                case 't': return SkipLiteral(text, pos, "true");
                case 'f': return SkipLiteral(text, pos, "false");
                case 'n': return SkipLiteral(text, pos, "null");
                default: return SkipNumber(text, pos);
            }
        }

        void AppendUtf8(std::string& out, unsigned int cp) {
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        bool ReadHex4(const std::string& text, size_t pos, unsigned int& out) {
            if (pos + 4 > text.size()) return false;
            unsigned int v = 0;
            for (int i = 0; i < 4; ++i) {
                const char h = text[pos + i];
                v <<= 4;
                if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                else return false;
            }
            out = v;
            return true;
        }

    } // namespace

    size_t SkipWhitespace(const std::string& text, size_t pos) {
        while (pos < text.size() && IsWs(text[pos])) ++pos;
        return (pos < text.size()) ? pos : std::string::npos;
    }

    size_t SkipValue(const std::string& text, size_t pos) {
        return SkipValueDepth(text, pos, 0);
    }

    bool Validate(const std::string& text, std::string& error) {
        error.clear();
        if (text.empty()) {
            error = "document is empty";
            return false;
        }
        size_t pos = SkipWhitespace(text, 0);
        if (pos == std::string::npos) {
            error = "document contains only whitespace";
            return false;
        }
        // A UTF-8 BOM would make the emulator's own parser choke too; treat it
        // as a corrupt file rather than silently accepting it.
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) {
            error = "document starts with a UTF-8 BOM";
            return false;
        }
        const size_t end = SkipValue(text, pos);
        if (end == std::string::npos) {
            error = "malformed JSON near byte " + std::to_string(pos);
            return false;
        }
        const size_t trailing = SkipWhitespace(text, end);
        if (trailing != std::string::npos) {
            error = "trailing data after the root value at byte " + std::to_string(trailing);
            return false;
        }
        return true;
    }

    bool ScanObject(const std::string& text, size_t obj_start, std::vector<Member>& out) {
        out.clear();
        if (obj_start >= text.size() || text[obj_start] != '{') return false;

        size_t pos = SkipWhitespace(text, obj_start + 1);
        if (pos == std::string::npos) return false;
        if (text[pos] == '}') return true; // empty object

        while (true) {
            if (text[pos] != '"') return false;
            Member m;
            m.key_start = pos;
            if (!DecodeString(text, pos, m.key)) return false;

            pos = SkipString(text, pos);
            if (pos == std::string::npos) return false;
            pos = SkipWhitespace(text, pos);
            if (pos == std::string::npos || text[pos] != ':') return false;
            pos = SkipWhitespace(text, pos + 1);
            if (pos == std::string::npos) return false;

            m.value_start = pos;
            const size_t value_end = SkipValue(text, pos);
            if (value_end == std::string::npos) return false;
            m.value_end = value_end;
            out.push_back(m);

            pos = SkipWhitespace(text, value_end);
            if (pos == std::string::npos) return false;
            if (text[pos] == ',') {
                pos = SkipWhitespace(text, pos + 1);
                if (pos == std::string::npos) return false;
                continue;
            }
            if (text[pos] == '}') return true;
            return false;
        }
    }

    bool FindRootMember(const std::string& text, const std::string& key, Member& out) {
        const size_t root = SkipWhitespace(text, 0);
        if (root == std::string::npos || text[root] != '{') return false;
        std::vector<Member> members;
        if (!ScanObject(text, root, members)) return false;
        for (const auto& m : members) {
            if (m.key == key) {
                out = m;
                return true;
            }
        }
        return false;
    }

    const Member* FindMember(const std::vector<Member>& members, const std::string& key) {
        for (const auto& m : members) {
            if (m.key == key) return &m;
        }
        return nullptr;
    }

    bool InsertMember(const std::string& text, size_t obj_start,
                      const std::string& key, const std::string& value_json,
                      std::string& out, std::string& error) {
        error.clear();
        const size_t obj_end = SkipValue(text, obj_start);
        if (obj_end == std::string::npos || obj_start >= text.size() || text[obj_start] != '{') {
            error = "target is not a well-formed object";
            return false;
        }

        // obj_end is one past '}'. Splice immediately before that brace so the
        // formatting of everything already in the file is preserved.
        const size_t brace = obj_end - 1;
        std::vector<Member> members;
        if (!ScanObject(text, obj_start, members)) {
            error = "target object could not be scanned";
            return false;
        }

        std::string insertion;
        if (!members.empty()) insertion.push_back(',');
        insertion += QuoteString(key);
        insertion.push_back(':');
        insertion += value_json;

        out.clear();
        out.reserve(text.size() + insertion.size());
        out.append(text, 0, brace);
        out.append(insertion);
        out.append(text, brace, text.size() - brace);
        return true;
    }

    std::string ReplaceValue(const std::string& text, const Member& member, const std::string& value_json) {
        std::string out;
        out.reserve(text.size() + value_json.size());
        out.append(text, 0, member.value_start);
        out.append(value_json);
        out.append(text, member.value_end, text.size() - member.value_end);
        return out;
    }

    std::string QuoteString(const std::string& value) {
        std::string out;
        out.reserve(value.size() + 2);
        out.push_back('"');
        for (unsigned char c : value) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20) {
                        static const char* hex = "0123456789abcdef";
                        out += "\\u00";
                        out.push_back(hex[(c >> 4) & 0xF]);
                        out.push_back(hex[c & 0xF]);
                    } else {
                        // Pass UTF-8 through byte for byte: the database in the
                        // wild stores "©" and "ー" literally, not escaped.
                        out.push_back(static_cast<char>(c));
                    }
                    break;
            }
        }
        out.push_back('"');
        return out;
    }

    bool DecodeString(const std::string& text, size_t quote_pos, std::string& out) {
        out.clear();
        if (quote_pos >= text.size() || text[quote_pos] != '"') return false;
        size_t pos = quote_pos + 1;
        while (pos < text.size()) {
            const char c = text[pos++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos >= text.size()) return false;
            const char esc = text[pos++];
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!ReadHex4(text, pos, cp)) return false;
                    pos += 4;
                    // Recombine a surrogate pair so astral characters survive.
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos + 1 < text.size() &&
                        text[pos] == '\\' && text[pos + 1] == 'u') {
                        unsigned int low = 0;
                        if (ReadHex4(text, pos + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            pos += 6;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: out.push_back(esc); break; // \" \\ \/
            }
        }
        return false;
    }

}
