#include "util/json.h"

#include <cstdio>

namespace snr
{
std::string jsonNumber(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", double(value));
    return buffer;
}

std::string jsonString(const std::string& value)
{
    std::string result = "\"";
    for (const char character : value)
    {
        switch (character)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20)
            {
                char escape[8];
                std::snprintf(escape, sizeof(escape), "\\u%04x", unsigned(static_cast<unsigned char>(character)));
                result += escape;
            }
            else
                result += character;
            break;
        }
    }
    result += '"';
    return result;
}

std::string jsonStrings(const std::vector<std::string>& values)
{
    std::string result;
    for (size_t index = 0; index < values.size(); ++index)
        result += (index == 0 ? "" : ", ") + jsonString(values[index]);
    return result;
}

std::string jsonVector(glm::vec3 value)
{
    return "[" + jsonNumber(value.x) + ", " + jsonNumber(value.y) + ", " + jsonNumber(value.z) + "]";
}

std::string jsonVector(glm::vec4 value)
{
    return "[" + jsonNumber(value.x) + ", " + jsonNumber(value.y) + ", " + jsonNumber(value.z)
         + ", " + jsonNumber(value.w) + "]";
}
} // namespace snr
