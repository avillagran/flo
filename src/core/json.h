// A minimal JSON reader (for `tailscale status --json`) and string escaping
// for the CLI's --json output. Depth and size are bounded; it is meant for
// small, untrusted documents.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace flo::json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object; // in document order

    const Value &operator[](const std::string &key) const; // Null when absent
    const Value &operator[](std::size_t i) const;
    bool is_object() const { return type == Object; }
    bool is_array() const { return type == Array; }
    std::string str() const { return type == String ? string : std::string(); }
    bool truthy() const { return type == Bool && boolean; }
};

// Returns false on malformed input, nesting deeper than 64, or more than
// max_bytes of text.
bool parse(const std::string &text, Value &out, std::size_t max_bytes = 16u << 20);

// A JSON string literal (with quotes) for any byte string; invalid UTF-8 is
// replaced by U+FFFD so the output is always valid JSON.
std::string quote(const std::string &s);

} // namespace flo::json
