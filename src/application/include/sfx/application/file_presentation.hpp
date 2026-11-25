#pragma once
#include "sfx/application/message_compose.hpp"
#include "sfx/application/menu.hpp"
#include <chrono>

namespace sfx::application {
// Untrusted UTF-8 metadata becomes inert CP437 text. Unsupported codepoints are
// replaced visibly, never interpreted as ANSI, display codes or commands.
std::string file_terminal_text(std::string_view, std::size_t maximum = 16384);
std::string file_option(char key, std::string_view description, bool ansi);
std::string file_size_label(std::uint64_t bytes);
std::string render_file_menu_rows(const std::vector<MenuTextRow>&,bool ansi,
    const MenuPreferences&,std::uint8_t default_color);
bool present_file_text(ICallerTerminal&, std::string_view identity, std::string_view utf8,
                       unsigned columns = 80, unsigned rows = 24);
bool present_file_transfer_text(ICallerTerminal&, std::string_view identity,
                                const std::vector<std::string>& lines, MessageEditorDimensions);
std::string_view file_transfer_text_handoff(bool ansi) noexcept;
bool present_file_login_scan_question(ICallerTerminal&, bool ansi);
struct FileDetailView {
    std::string filename, area, added, uploader, description;
    std::uint64_t bytes{}, downloads{};
    bool downloadable{}, editable{};
    std::string feedback{};
};
// Metadata is inert. Measured pages reserve status, footer and input cells;
// continuation uses the terminal's existing forced paging prompt.
std::optional<TerminalDisplayProgram> prepare_file_information(
    const FileDetailView&, MessageEditorDimensions);
bool present_file_information(ICallerTerminal&, const FileDetailView&, MessageEditorDimensions);
// Bounded content detection/rendition for a registered standalone file or a
// selected ZIP member. No original terminal escape or display token executes.
std::optional<TerminalDisplayProgram> prepare_file_view(std::string_view bytes,bool ansi);

struct FileListRow {
    unsigned number{};
    std::int64_t identity{}, revision{};
    std::string text;
    std::string filename{}; // exact inert name for nonnumeric plain-text selection
};
struct FileListPage {
    std::string title, context, columns, notice;
    std::vector<FileListRow> rows;
    bool previous{}, next{}, tag{}, sort{}, jump{};
    bool numbered{true}; // group/area selectors retain their explicit numbers
};
enum class FileListAction : std::uint8_t { cancel, open, tag, previous, next, sort, jump, failed };
struct FileListChoice {
    FileListAction action{FileListAction::failed};
    std::int64_t identity{}, revision{};
};
struct FileListPorts {
    ICallerTerminal& terminal;
    std::function<MessageEditorDimensions()> dimensions{};
    std::function<StreamCallerTerminal::EditorByteResult()> byte{};
    std::function<bool(std::string_view)> write{};
    std::function<void(bool)> accepted_key{};
    std::function<bool()> live{};
    // After successful output only, exact rows emitted on this screen. A failed
    // acknowledgement stops the scan without advancing unseen publications.
    std::function<bool(const std::vector<FileListRow>&)> emitted{};
    bool lightbars{true};
    std::uint8_t selected_foreground{0}, selected_background{7};
    std::function<bool()> lightbars_enabled{};
    std::function<std::pair<std::uint8_t,std::uint8_t>()> selection_colors{};
};
// Uses the session's guarded poll reader, so lifetime/revocation still apply.
// A quiet poll separates trailing transfer input from fresh hotkey commands.
std::optional<bool> file_transfer_logoff_countdown(const FileListPorts&,
    const std::function<std::chrono::steady_clock::time_point()>& now =
        std::chrono::steady_clock::now);
// One immutable, bounded authorized page. Returns the displayed identity and
// revision, never a freshly interpreted row position. The caller must recheck
// current authority/identity before opening, tagging, or mutating anything.
FileListChoice choose_file_row(const FileListPorts&, const FileListPage&,
                               std::size_t& highlighted);
} // namespace sfx::application
