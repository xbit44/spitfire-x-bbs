#include "sfx/application/logging.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace sfx::application {

namespace {

constexpr std::string_view kRedacted = "[REDACTED]";

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

}  // namespace

RedactingLogger::RedactingLogger(ILogSink& sink) : sink_(sink) {}

bool RedactingLogger::is_sensitive_field_name(const std::string_view name) {
    static constexpr std::array<std::string_view, 8> fragments{
        "password", "credential", "secret", "private_key", "rlogin", "private_text", "raw_output", "path"};
    const auto normalized = lower(name);
    return std::any_of(fragments.begin(), fragments.end(), [&normalized](const std::string_view fragment) {
        return normalized.find(fragment) != std::string::npos;
    });
}

void RedactingLogger::write(LogRecord record) {
    for (auto& field : record.fields) {
        if (field.sensitivity != FieldSensitivity::public_value || is_sensitive_field_name(field.name)) {
            field.value = std::string(kRedacted);
        }
    }
    sink_.write(record);
}

}  // namespace sfx::application

