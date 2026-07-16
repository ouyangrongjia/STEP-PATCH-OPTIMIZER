#pragma once

#include <filesystem>
#include <sstream>
#include <string>

namespace spo::json_text {

inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.c_str()), utf8.size()};
}

inline std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (const char character : value) {
        switch (character) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                const char* digits = "0123456789ABCDEF";
                output << "\\u00"
                       << digits[(static_cast<unsigned char>(character) >> 4) & 0x0F]
                       << digits[static_cast<unsigned char>(character) & 0x0F];
            } else {
                output << character;
            }
            break;
        }
    }
    return output.str();
}

inline std::string quote_json(const std::string& value) {
    return "\"" + json_escape(value) + "\"";
}

}
