#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

namespace sfx::application {

enum class NewCallerNameDecision { allowed, unavailable, temporarily_unavailable };

// A decision is scoped to one name check. Implementations may acquire a fresh
// bounded snapshot; a caller must never retain a decision for a later check.
class INewCallerNamePolicy {
public:
    virtual ~INewCallerNamePolicy() = default;
    virtual NewCallerNameDecision evaluate(std::string_view name) const = 0;
};

// Immutable parsed contents of one read, not a Runtime-lifetime cache.
class NewCallerNames final : public INewCallerNamePolicy {
public:
    static constexpr std::size_t maximum_bytes = 64U * 1024U;
    static constexpr std::size_t maximum_lines = 1024;
    static constexpr std::size_t maximum_line_bytes = 256;
    static NewCallerNames parse(std::string_view bytes);
    static NewCallerNames unavailable() noexcept;
    NewCallerNameDecision evaluate(std::string_view name) const override;
    bool valid() const noexcept { return valid_; }
    std::size_t size() const noexcept { return keys_.size(); }
private:
    bool valid_{true};
    std::unordered_set<std::string> keys_;
};

} // namespace sfx::application
