#pragma once
#include "sfx/application/terminal.hpp"
#include <functional>
namespace sfx::application {
TerminalDisplayProgram message_reader_command_program(bool ansi,unsigned columns,bool can_delete,bool can_mark,bool can_reply=true,bool list_return=false,bool allow_skip=false,bool permanent=false);
class IMessageStorage;
struct MessageActor;
std::string message_network_name(IMessageStorage&,const MessageActor&,std::string_view code);
// Inert CP437 text; never evaluated as trusted display source. A final logical
// newline is host framing, including a deliberately empty separator row.
bool present_message_text(ICallerTerminal&,std::string identity,std::string_view text);
// Generated reader label plus inert caller-colored value. Neither span is
// interpreted as authored artwork, pipe colors or display-file input.
bool present_message_reader_field(ICallerTerminal&,bool ansi,std::uint8_t color,
    std::string_view label,std::string_view value,unsigned columns=80);
struct MessageLoginScanSummary;
bool present_message_login_summary(ICallerTerminal&,const MessageLoginScanSummary&,bool ansi,unsigned columns=80);
bool present_message_read_options(ICallerTerminal&,bool ansi,unsigned columns=80);
std::string wrap_message_prompt(std::string_view,unsigned columns,unsigned input_cells=1);
// Generated reader footer only. Reserves the prompt space, one echoed key and
// the terminal's right-edge cell; stored message bodies are never reformatted.
bool present_message_reader_commands(ICallerTerminal&,bool ansi,unsigned columns,
    bool can_delete,bool can_mark,bool permanent,bool can_reply=true,bool list_return=false,bool allow_skip=false);
bool present_message_options(ICallerTerminal&,bool ansi,std::string identity,const std::vector<std::pair<char,std::string>>& options,unsigned columns=80);
// Message-only question framing; no changes to artwork or other terminal users.
class MessageQuestionTerminal final : public ICallerTerminal {
public:
    MessageQuestionTerminal(ICallerTerminal& terminal,std::function<bool()> ansi,
        std::function<bool(std::string_view)> write,std::function<unsigned()> columns={},std::function<unsigned()> rows={}):
        terminal_(terminal),ansi_(std::move(ansi)),write_(std::move(write)),columns_(std::move(columns)),rows_(std::move(rows)){}
    TerminalOutcome set_text_style(TerminalTextStyle style) override{return terminal_.set_text_style(style);}
    TerminalOutcome clear_screen() override;
    TerminalInputResult read_visible(const TerminalInputRequest&) override;
    TerminalInputResult read_secret(const TerminalInputRequest&) override;
    TerminalKeyResult read_key(const TerminalKeyRequest&) override;
    TerminalOutcome present(const TerminalPresentationRequest& r) override{line_start_=false;return terminal_.present(r);}
    TerminalOutcome present_display(const TerminalDisplayPresentationRequest& r) override;
    TerminalOutcome pause(const TerminalPauseRequest& r) override{return terminal_.pause(r);}
    void menu_boundary() noexcept{line_start_=false;}
private:
    ICallerTerminal& terminal_;
    std::function<bool()> ansi_;
    std::function<bool(std::string_view)> write_;
    std::function<unsigned()> columns_;
    std::function<unsigned()> rows_;
    bool line_start_{};
    bool before();
    bool after(TerminalOutcome);
};
}
