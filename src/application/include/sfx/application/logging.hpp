#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {

enum class LogSeverity : std::uint8_t { debug, information, warning, error, security };
enum class FieldSensitivity : std::uint8_t { public_value, secret, private_text, sensitive_path };

struct LogField {
    std::string name;
    std::string value;
    FieldSensitivity sensitivity{FieldSensitivity::public_value};
};

struct LogRecord {
    LogSeverity severity{LogSeverity::information};
    std::string category;
    std::string event_code;
    std::string message;
    std::vector<LogField> fields;
};

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void write(const LogRecord& record) = 0;
};

class RedactingLogger final {
public:
    explicit RedactingLogger(ILogSink& sink);
    void write(LogRecord record);
    static bool is_sensitive_field_name(std::string_view name);

private:
    ILogSink& sink_;
};

struct FailureReport {
    std::string caller_code;
    std::string caller_message;
    LogRecord local_diagnostic;
};

class ILogRetentionPolicy {
public:
    virtual ~ILogRetentionPolicy() = default;
    virtual bool should_rotate(std::uint64_t current_bytes, std::uint64_t age_seconds) const noexcept = 0;
};

}  // namespace sfx::application

