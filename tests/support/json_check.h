// A strict JSON reader for tests (RFC 8259): it checks that a text is exactly one JSON value and
// returns it as a small tree, so a test can look at fields. Strings must be valid UTF-8 without
// raw control characters, escapes must be valid (a \u escape of a surrogate must come in a
// pair), numbers must have JSON's syntax, and nothing may follow the value but white space.
// Not for production use: no limits beyond a nesting depth of 64.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pdft::test {

struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  double number = 0;
  std::string text;              // kString: the decoded UTF-8; kNumber: as written
  std::vector<JsonValue> items;  // kArray
  std::vector<std::pair<std::string, JsonValue>> members;  // kObject, in order

  // The member |key| of an object; nullptr when this is not an object or has no such member.
  const JsonValue* Get(std::string_view key) const;
  bool IsNull() const { return type == Type::kNull; }
};

// Parses |text|; false (with |error| saying where and why) when it is not valid JSON.
bool ParseJson(std::string_view text, JsonValue* value, std::string* error = nullptr);

}  // namespace pdft::test
