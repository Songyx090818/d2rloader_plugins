#include "json_value.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace lootfilter::json {

auto Value::Find(std::string_view key) const noexcept -> const Value* {
    const Value* found = nullptr;
    for (const Member& member : object) {
        if (member.key == key) found = &member.value;
    }
    return found;
}

auto Describe(Type type) noexcept -> const char* {
    switch (type) {
    case Type::Null:   return "null";
    case Type::Bool:   return "true or false";
    case Type::Number: return "a number";
    case Type::String: return "a text";
    case Type::Array:  return "a list [ ... ]";
    case Type::Object: return "an object { ... }";
    }
    return "a value";
}

namespace {

constexpr int MaximumDepth = 64;

class Reader {
public:
    explicit Reader(std::string_view text) noexcept : text_(text) {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF
                && static_cast<unsigned char>(text_[1]) == 0xBB && static_cast<unsigned char>(text_[2]) == 0xBF) {
            position_ = 3;
        }
    }

    auto ReadDocument(Value& out, ParseError& error) -> bool {
        if (!SkipSpace()) return Fail(error);
        if (AtEnd()) {
            failure_ = "the file is empty";
            return Fail(error);
        }
        if (!ReadValue(out, 0)) return Fail(error);
        if (!SkipSpace()) return Fail(error);
        if (!AtEnd()) {
            failure_ = "unexpected text after the end of the filter";
            return Fail(error);
        }
        return true;
    }

private:
    std::string_view text_;
    std::size_t      position_{};
    std::uint32_t    line_{ 1 };
    std::size_t      lineStart_{};
    std::string      failure_;
    std::uint32_t    failureLine_{};
    std::uint32_t    failureColumn_{};

    [[nodiscard]] auto AtEnd() const noexcept -> bool { return position_ >= text_.size(); }
    [[nodiscard]] auto Peek() const noexcept -> char { return AtEnd() ? '\0' : text_[position_]; }
    [[nodiscard]] auto Column() const noexcept -> std::uint32_t {
        // Columns count characters, not bytes, so a message points where an editor shows the cursor.
        std::uint32_t column = 1;
        for (std::size_t i = lineStart_; i < position_ && i < text_.size(); ++i) {
            if ((static_cast<unsigned char>(text_[i]) & 0xC0) != 0x80) ++column;
        }
        return column;
    }

    void Advance() noexcept {
        if (AtEnd()) return;
        if (text_[position_] == '\n') {
            ++line_;
            lineStart_ = position_ + 1;
        }
        ++position_;
    }

    auto Error(const char* message) -> bool {
        if (failure_.empty()) {
            failure_       = message;
            failureLine_   = line_;
            failureColumn_ = Column();
        }
        return false;
    }

    auto Fail(ParseError& error) -> bool {
        if (failureLine_ == 0) {
            failureLine_   = line_;
            failureColumn_ = Column();
        }
        error.line    = failureLine_;
        error.column  = failureColumn_;
        error.message = failure_.empty() ? std::string("invalid JSON") : failure_;
        return false;
    }

    auto SkipSpace() -> bool {
        while (!AtEnd()) {
            const char c = Peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                Advance();
                continue;
            }
            if (c == '/' && position_ + 1 < text_.size()) {
                const char next = text_[position_ + 1];
                if (next == '/') {
                    while (!AtEnd() && Peek() != '\n') Advance();
                    continue;
                }
                if (next == '*') {
                    const std::uint32_t startLine   = line_;
                    const std::uint32_t startColumn = Column();
                    Advance();
                    Advance();
                    bool closed = false;
                    while (!AtEnd()) {
                        if (Peek() == '*' && position_ + 1 < text_.size() && text_[position_ + 1] == '/') {
                            Advance();
                            Advance();
                            closed = true;
                            break;
                        }
                        Advance();
                    }
                    if (!closed) {
                        failure_       = "a /* comment is never closed with */";
                        failureLine_   = startLine;
                        failureColumn_ = startColumn;
                        return false;
                    }
                    continue;
                }
            }
            break;
        }
        return true;
    }

    auto ReadValue(Value& out, int depth) -> bool {
        if (depth > MaximumDepth) return Error("the filter nests deeper than 64 levels");
        out.line   = line_;
        out.column = Column();
        const char c = Peek();
        switch (c) {
        case '{': return ReadObject(out, depth);
        case '[': return ReadArray(out, depth);
        case '"':
            out.type = Type::String;
            return ReadString(out.string);
        case 't': return ReadWord("true", out, Type::Bool, true);
        case 'f': return ReadWord("false", out, Type::Bool, false);
        case 'n': return ReadWord("null", out, Type::Null, false);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return ReadNumber(out);
            if (c == '\'') return Error("texts must use double quotes \"...\", not single quotes");
            if (c == '}' || c == ']') return Error("a value is missing here");
            if (AtEnd()) return Error("the file ends in the middle of a value");
            return Error("unexpected character; expected a value");
        }
    }

    auto ReadWord(const char* word, Value& out, Type type, bool boolean) -> bool {
        const std::size_t length = std::strlen(word);
        if (text_.substr(position_, length) != std::string_view(word, length)) {
            return Error("unexpected word; expected true, false, null, a number, a text, a list or an object");
        }
        for (std::size_t i = 0; i < length; ++i) Advance();
        out.type    = type;
        out.boolean = boolean;
        return true;
    }

    auto ReadNumber(Value& out) -> bool {
        const std::size_t start = position_;
        bool fraction = false;
        if (Peek() == '-') Advance();
        if (!(Peek() >= '0' && Peek() <= '9')) return Error("a minus sign must be followed by a digit");
        while (Peek() >= '0' && Peek() <= '9') Advance();
        if (Peek() == '.') {
            fraction = true;
            Advance();
            if (!(Peek() >= '0' && Peek() <= '9')) return Error("a decimal point must be followed by a digit");
            while (Peek() >= '0' && Peek() <= '9') Advance();
        }
        if (Peek() == 'e' || Peek() == 'E') {
            fraction = true;
            Advance();
            if (Peek() == '+' || Peek() == '-') Advance();
            if (!(Peek() >= '0' && Peek() <= '9')) return Error("an exponent must be followed by a digit");
            while (Peek() >= '0' && Peek() <= '9') Advance();
        }
        const std::string literal(text_.substr(start, position_ - start));
        out.type = Type::Number;
        errno    = 0;
        char* end = nullptr;
        out.number = std::strtod(literal.c_str(), &end);
        if (!fraction) {
            errno = 0;
            const long long value = std::strtoll(literal.c_str(), &end, 10);
            if (errno == 0 && end != nullptr && *end == '\0') {
                out.isInteger = true;
                out.integer   = value;
            }
        }
        return true;
    }

    static void AppendUtf8(std::string& out, std::uint32_t codePoint) {
        if (codePoint < 0x80) {
            out.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    auto ReadHex4(std::uint32_t& value) -> bool {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = Peek();
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint32_t>(c - 'A' + 10);
            else return Error("\\u must be followed by four hexadecimal digits");
            value = (value << 4) | digit;
            Advance();
        }
        return true;
    }

    auto ReadString(std::string& out) -> bool {
        const std::uint32_t startLine   = line_;
        const std::uint32_t startColumn = Column();
        Advance();  // opening quote
        out.clear();
        while (true) {
            if (AtEnd()) {
                failure_       = "a text is never closed with a double quote";
                failureLine_   = startLine;
                failureColumn_ = startColumn;
                return false;
            }
            const char c = Peek();
            if (c == '"') {
                Advance();
                return true;
            }
            if (c == '\n' || c == '\r') return Error("a text cannot span lines; write \\n for a line break");
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t') return Error("a text contains a control character");
            if (c != '\\') {
                out.push_back(c);
                Advance();
                continue;
            }
            Advance();
            const char escape = Peek();
            switch (escape) {
            case '"':  out.push_back('"'); Advance(); break;
            case '\\': out.push_back('\\'); Advance(); break;
            case '/':  out.push_back('/'); Advance(); break;
            case 'b':  out.push_back('\b'); Advance(); break;
            case 'f':  out.push_back('\f'); Advance(); break;
            case 'n':  out.push_back('\n'); Advance(); break;
            case 'r':  out.push_back('\r'); Advance(); break;
            case 't':  out.push_back('\t'); Advance(); break;
            case 'u': {
                Advance();
                std::uint32_t codePoint = 0;
                if (!ReadHex4(codePoint)) return false;
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    if (Peek() != '\\') return Error("a \\u surrogate pair is incomplete");
                    Advance();
                    if (Peek() != 'u') return Error("a \\u surrogate pair is incomplete");
                    Advance();
                    std::uint32_t low = 0;
                    if (!ReadHex4(low)) return false;
                    if (low < 0xDC00 || low > 0xDFFF) return Error("a \\u surrogate pair is invalid");
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                    return Error("a \\u surrogate pair is invalid");
                }
                AppendUtf8(out, codePoint);
                break;
            }
            default:
                return Error("unknown escape; use \\\", \\\\, \\/, \\b, \\f, \\n, \\r, \\t or \\uXXXX");
            }
        }
    }

    auto ReadArray(Value& out, int depth) -> bool {
        out.type = Type::Array;
        Advance();  // [
        if (!SkipSpace()) return false;
        if (Peek() == ']') {
            Advance();
            return true;
        }
        while (true) {
            Value element;
            if (!ReadValue(element, depth + 1)) return false;
            out.array.push_back(std::move(element));
            if (!SkipSpace()) return false;
            if (Peek() == ',') {
                Advance();
                if (!SkipSpace()) return false;
                if (Peek() == ']') {
                    Advance();
                    return true;
                }
                continue;
            }
            if (Peek() == ']') {
                Advance();
                return true;
            }
            if (AtEnd()) return Error("a list is never closed with ]");
            return Error("expected , or ] after a list element");
        }
    }

    auto ReadObject(Value& out, int depth) -> bool {
        out.type = Type::Object;
        Advance();  // {
        if (!SkipSpace()) return false;
        if (Peek() == '}') {
            Advance();
            return true;
        }
        while (true) {
            if (Peek() != '"') {
                if (AtEnd()) return Error("an object is never closed with }");
                return Error("expected a property name in double quotes");
            }
            Member member;
            if (!ReadString(member.key)) return false;
            if (!SkipSpace()) return false;
            if (Peek() != ':') return Error("expected : after a property name");
            Advance();
            if (!SkipSpace()) return false;
            if (!ReadValue(member.value, depth + 1)) return false;
            out.object.push_back(std::move(member));
            if (!SkipSpace()) return false;
            if (Peek() == ',') {
                Advance();
                if (!SkipSpace()) return false;
                if (Peek() == '}') {
                    Advance();
                    return true;
                }
                continue;
            }
            if (Peek() == '}') {
                Advance();
                return true;
            }
            if (AtEnd()) return Error("an object is never closed with }");
            return Error("expected , or } after a property");
        }
    }
};

}  // namespace

auto Parse(std::string_view text, Value& out, ParseError& error) -> bool {
    out = Value{};
    Reader reader(text);
    return reader.ReadDocument(out, error);
}

}  // namespace lootfilter::json
