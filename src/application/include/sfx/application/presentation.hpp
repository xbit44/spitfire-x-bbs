#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {

enum class GraphicsMode : std::uint8_t { ascii, ansi };
enum class InputEcho : std::uint8_t { visible, hidden };

struct PromptRequest {
    std::string identity;
    std::string text;
    InputEcho echo{InputEcho::visible};
    std::uint16_t maximum_characters{0};
};

struct MenuChoice {
    std::string command_identity;
    std::string key;
    std::string label;
};

struct GeneratedMenu {
    std::string identity;
    std::vector<MenuChoice> choices;
};

struct DisplayRequest {
    std::string basename;
    GraphicsMode graphics{GraphicsMode::ascii};
    std::uint16_t security_level{0};
};

class ISessionPresentation {
public:
    virtual ~ISessionPresentation() = default;
    virtual void write_text(std::string_view text) = 0;
    virtual void clear_screen() = 0;
    virtual void move_cursor(std::uint16_t row, std::uint16_t column) = 0;
    virtual void set_paging(bool enabled) = 0;
    virtual void set_abort_enabled(bool enabled) = 0;
    virtual void beep() = 0;
    virtual void pause_for_enter() = 0;
    virtual std::string prompt(const PromptRequest& request) = 0;
    virtual void show_generated_menu(const GeneratedMenu& menu) = 0;
    virtual bool render_named_display(const DisplayRequest& request) = 0;
};

}  // namespace sfx::application

