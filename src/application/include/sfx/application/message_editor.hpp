#pragma once
#include "sfx/application/message.hpp"

namespace sfx::application {
enum class MessageEditorCommand : std::uint8_t { none, save, abort, quote, subject, help, list, edit_line, delete_line, insert_line, invalid, actions, upload };
struct MessageLineCommand { MessageEditorCommand command{}; std::optional<std::size_t> number; };
// Live submission only. Replacement/quote/paste text never passes this decoder.
MessageLineCommand message_line_command(std::string_view live_line);
enum class MessageEditorKey : std::uint8_t { text, enter, backspace, erase, left, right, up, down, home, end, page_up, page_down, insert_mode, resume };
struct MessageEditorEvent { MessageEditorKey key{MessageEditorKey::text}; unsigned char byte{}; };
struct MessageEditorInput {
    std::optional<MessageEditorEvent> event;
    MessageEditorCommand command{MessageEditorCommand::none};
};
// Bounded live-key decoder. Unknown escape sequences never fall through as body
// text or control shortcuts. A bare ESC resolves only at a transport idle poll.
class MessageEditorKeyDecoder final {
public:
    std::optional<MessageEditorInput> feed(unsigned char);
    std::optional<MessageEditorInput> idle();
    void reset() noexcept;
private:
    std::string sequence_;
    bool lf_after_cr_{},string_escape_{},string_tail_{},discard_sequence_{};
};
struct MessageEditorScreen {
    std::vector<std::string> rows;
    std::size_t cursor_row{},cursor_column{};
    void clear(){rows.clear();cursor_row=cursor_column=0;}
};
class MessageEditorState final {
public:
    explicit MessageEditorState(std::uint32_t maximum_lines):draft_(maximum_lines){}
    MessageDraft& draft() noexcept {return draft_;}
    const MessageDraft& draft() const noexcept {return draft_;}
    void resize(bool ansi,unsigned columns,unsigned rows) noexcept;
    MessageValidation apply(MessageEditorEvent);
    MessageValidation insert_data(std::string_view); // never interprets commands
    MessageValidation quote(const std::vector<std::string>&,const std::vector<std::size_t>&);
    std::size_t cursor_offset() const noexcept {return cursor_;}
    std::size_t cursor_row() const noexcept;
    std::size_t cursor_column() const noexcept;
    std::size_t top_row() const noexcept {return top_;}
    unsigned body_rows() const noexcept {return rows_-6;}
    bool full_screen() const noexcept {return full_screen_;}
    bool inserting() const noexcept {return inserting_;}
    // ANSI frame owns fixed controls; bodies/labels are safe CP437 data only.
    std::string frame(std::string_view to,std::string_view from) const;
    std::string update_frame(std::string_view to,std::string_view from,MessageEditorScreen&) const;
private:
    void reveal() noexcept;
    void vertical(std::int64_t rows) noexcept;
    std::size_t offset(std::size_t row,std::size_t column) const noexcept;
    MessageDraft draft_;
    std::size_t cursor_{},top_{},preferred_column_{};
    unsigned rows_{24};
    bool full_screen_{true},inserting_{true};
};
} // namespace sfx::application
