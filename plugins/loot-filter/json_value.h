#pragma once
//
// A small JSON reader for filter files.
//
// It accepts the same relaxed JSON the game's own .json files use:
//   - // line comments and /* block */ comments
//   - a trailing comma before ] or }
//   - an optional UTF-8 byte order mark
// Everything else is plain JSON. Object members keep their file order, and every
// value remembers its line and column so problems can point at the exact spot.
//
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lootfilter::json {

enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

struct Member;

struct Value {
    Type               type{ Type::Null };
    bool               boolean{};
    bool               isInteger{};
    std::int64_t       integer{};
    double             number{};
    std::string        string;
    std::vector<Value> array;
    std::vector<Member> object;
    std::uint32_t      line{};
    std::uint32_t      column{};

    [[nodiscard]] auto IsNull() const noexcept -> bool { return type == Type::Null; }
    [[nodiscard]] auto IsBool() const noexcept -> bool { return type == Type::Bool; }
    [[nodiscard]] auto IsNumber() const noexcept -> bool { return type == Type::Number; }
    [[nodiscard]] auto IsInteger() const noexcept -> bool { return type == Type::Number && isInteger; }
    [[nodiscard]] auto IsString() const noexcept -> bool { return type == Type::String; }
    [[nodiscard]] auto IsArray() const noexcept -> bool { return type == Type::Array; }
    [[nodiscard]] auto IsObject() const noexcept -> bool { return type == Type::Object; }

    // Last member with this key, or nullptr. Duplicate keys are reported by the
    // filter compiler, and the last one wins, as in most JSON readers.
    [[nodiscard]] auto Find(std::string_view key) const noexcept -> const Value*;
};

struct Member {
    std::string key;
    Value       value;
};

struct ParseError {
    std::uint32_t line{};
    std::uint32_t column{};
    std::string   message;
};

// Returns false and fills error when the text is not valid relaxed JSON.
auto Parse(std::string_view text, Value& out, ParseError& error) -> bool;

// Name of a type for messages ("a string", "an array", ...).
auto Describe(Type type) noexcept -> const char*;

}  // namespace lootfilter::json
