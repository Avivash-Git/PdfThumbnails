#include "json_check.h"

#include <cstdlib>

namespace pdft::test {
namespace {

constexpr int kMaxDepth = 64;

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  bool Document(JsonValue* value) {
    SkipSpace();
    if (!Value(value, 0)) return false;
    SkipSpace();
    if (at_ != text_.size()) return Fail("text after the value");
    return true;
  }

  const std::string& error() const { return error_; }

 private:
  bool Fail(const char* why) {
    if (error_.empty()) error_ = std::string(why) + " at byte " + std::to_string(at_);
    return false;
  }

  void SkipSpace() {
    while (at_ < text_.size() &&
           (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) {
      ++at_;
    }
  }

  bool Literal(std::string_view word) {
    if (text_.substr(at_, word.size()) != word) return Fail("unknown literal");
    at_ += word.size();
    return true;
  }

  bool Value(JsonValue* value, int depth) {
    if (depth > kMaxDepth) return Fail("nested too deeply");
    if (at_ >= text_.size()) return Fail("value expected");
    switch (text_[at_]) {
      case '{':
        return Object(value, depth);
      case '[':
        return Array(value, depth);
      case '"':
        value->type = JsonValue::Type::kString;
        return String(&value->text);
      case 't':
        value->type = JsonValue::Type::kBool;
        value->boolean = true;
        return Literal("true");
      case 'f':
        value->type = JsonValue::Type::kBool;
        value->boolean = false;
        return Literal("false");
      case 'n':
        value->type = JsonValue::Type::kNull;
        return Literal("null");
      default:
        return Number(value);
    }
  }

  bool Object(JsonValue* value, int depth) {
    value->type = JsonValue::Type::kObject;
    ++at_;
    SkipSpace();
    if (at_ < text_.size() && text_[at_] == '}') {
      ++at_;
      return true;
    }
    for (;;) {
      SkipSpace();
      if (at_ >= text_.size() || text_[at_] != '"') return Fail("member name expected");
      std::pair<std::string, JsonValue> member;
      if (!String(&member.first)) return false;
      SkipSpace();
      if (at_ >= text_.size() || text_[at_] != ':') return Fail("':' expected");
      ++at_;
      SkipSpace();
      if (!Value(&member.second, depth + 1)) return false;
      value->members.push_back(std::move(member));
      SkipSpace();
      if (at_ < text_.size() && text_[at_] == ',') {
        ++at_;
        continue;
      }
      if (at_ < text_.size() && text_[at_] == '}') {
        ++at_;
        return true;
      }
      return Fail("',' or '}' expected");
    }
  }

  bool Array(JsonValue* value, int depth) {
    value->type = JsonValue::Type::kArray;
    ++at_;
    SkipSpace();
    if (at_ < text_.size() && text_[at_] == ']') {
      ++at_;
      return true;
    }
    for (;;) {
      SkipSpace();
      JsonValue item;
      if (!Value(&item, depth + 1)) return false;
      value->items.push_back(std::move(item));
      SkipSpace();
      if (at_ < text_.size() && text_[at_] == ',') {
        ++at_;
        continue;
      }
      if (at_ < text_.size() && text_[at_] == ']') {
        ++at_;
        return true;
      }
      return Fail("',' or ']' expected");
    }
  }

  static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

  bool Number(JsonValue* value) {
    const size_t start = at_;
    if (at_ < text_.size() && text_[at_] == '-') ++at_;
    if (at_ >= text_.size() || !IsDigit(text_[at_])) return Fail("number expected");
    if (text_[at_] == '0') {
      ++at_;
    } else {
      while (at_ < text_.size() && IsDigit(text_[at_])) ++at_;
    }
    if (at_ < text_.size() && text_[at_] == '.') {
      ++at_;
      if (at_ >= text_.size() || !IsDigit(text_[at_])) return Fail("digit expected");
      while (at_ < text_.size() && IsDigit(text_[at_])) ++at_;
    }
    if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
      ++at_;
      if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) ++at_;
      if (at_ >= text_.size() || !IsDigit(text_[at_])) return Fail("digit expected");
      while (at_ < text_.size() && IsDigit(text_[at_])) ++at_;
    }
    value->type = JsonValue::Type::kNumber;
    value->text = std::string(text_.substr(start, at_ - start));
    value->number = std::strtod(value->text.c_str(), nullptr);
    return true;
  }

  static int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  bool Hex4(unsigned* code) {
    if (text_.size() - at_ < 4) return Fail("short \\u escape");
    *code = 0;
    for (int i = 0; i < 4; ++i) {
      const int digit = HexValue(text_[at_++]);
      if (digit < 0) return Fail("bad \\u escape");
      *code = *code * 16 + static_cast<unsigned>(digit);
    }
    return true;
  }

  static void AppendUtf8(unsigned code, std::string* out) {
    if (code < 0x80) {
      *out += static_cast<char>(code);
    } else if (code < 0x800) {
      *out += static_cast<char>(0xC0 | (code >> 6));
      *out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      *out += static_cast<char>(0xE0 | (code >> 12));
      *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      *out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
      *out += static_cast<char>(0xF0 | (code >> 18));
      *out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      *out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }

  // One UTF-8 sequence starting at at_, checked strictly (shortest form, no surrogates).
  bool Utf8Sequence(std::string* out) {
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(text_[i]); };
    const unsigned char lead = byte(at_);
    size_t length = 0;
    unsigned code = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
      length = 2;
      code = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      length = 3;
      code = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      length = 4;
      code = lead & 0x07;
    } else {
      return Fail("invalid UTF-8");
    }
    if (text_.size() - at_ < length) return Fail("cut UTF-8");
    for (size_t i = 1; i < length; ++i) {
      if ((byte(at_ + i) & 0xC0) != 0x80) return Fail("invalid UTF-8");
      code = (code << 6) | (byte(at_ + i) & 0x3F);
    }
    if ((length == 3 && code < 0x800) || (length == 4 && (code < 0x10000 || code > 0x10FFFF)) ||
        (code >= 0xD800 && code <= 0xDFFF)) {
      return Fail("invalid UTF-8");
    }
    out->append(text_.substr(at_, length));
    at_ += length;
    return true;
  }

  bool String(std::string* out) {
    ++at_;  // the opening quote
    for (;;) {
      if (at_ >= text_.size()) return Fail("unterminated string");
      const unsigned char c = static_cast<unsigned char>(text_[at_]);
      if (c == '"') {
        ++at_;
        return true;
      }
      if (c < 0x20) return Fail("raw control character in a string");
      if (c >= 0x80) {
        if (!Utf8Sequence(out)) return false;
        continue;
      }
      if (c != '\\') {
        *out += static_cast<char>(c);
        ++at_;
        continue;
      }
      ++at_;
      if (at_ >= text_.size()) return Fail("unterminated escape");
      const char escape = text_[at_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          *out += escape;
          break;
        case 'b':
          *out += '\b';
          break;
        case 'f':
          *out += '\f';
          break;
        case 'n':
          *out += '\n';
          break;
        case 'r':
          *out += '\r';
          break;
        case 't':
          *out += '\t';
          break;
        case 'u': {
          unsigned code = 0;
          if (!Hex4(&code)) return false;
          if (code >= 0xDC00 && code <= 0xDFFF) return Fail("lone low surrogate");
          if (code >= 0xD800 && code <= 0xDBFF) {
            unsigned low = 0;
            if (text_.substr(at_, 2) != "\\u") return Fail("lone high surrogate");
            at_ += 2;
            if (!Hex4(&low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return Fail("lone high surrogate");
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          }
          AppendUtf8(code, out);
          break;
        }
        default:
          return Fail("unknown escape");
      }
    }
  }

  std::string_view text_;
  size_t at_ = 0;
  std::string error_;
};

}  // namespace

const JsonValue* JsonValue::Get(std::string_view key) const {
  if (type != Type::kObject) return nullptr;
  for (const auto& member : members) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

bool ParseJson(std::string_view text, JsonValue* value, std::string* error) {
  *value = JsonValue{};
  Parser parser(text);
  const bool ok = parser.Document(value);
  if (error) *error = parser.error();
  return ok;
}

}  // namespace pdft::test
