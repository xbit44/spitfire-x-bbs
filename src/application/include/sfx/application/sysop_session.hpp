#pragma once
#include "sfx/application/menu_session.hpp"
#include "sfx/application/local_account_administration.hpp"
#include "sfx/application/file_management.hpp"
#include <map>

namespace sfx::application {
enum class SysopLog : std::uint8_t { heysysop, callers, bink, freshclam, botgate };
struct SysopTextPage {
    std::string title, notice;
    std::vector<std::string> lines;
    bool previous{}, next{};
    std::uint64_t page{};
};
std::vector<std::string> wrap_sysop_log_text(std::string_view text,unsigned columns,bool indent=true);
unsigned sysop_log_content_rows(std::string_view title,std::string_view details,
    std::uint64_t total_rows,unsigned columns,unsigned rows);
inline constexpr std::string_view kSysopLogControls="[P] Prev [N] Next [R] Refresh [Q] Back";
struct SysopNamedChoice {std::string key,name;bool enabled{true};};
enum class SysopFtnView : std::uint8_t { queues, held, activity };
// All callbacks are bound inside Runtime to one authenticated session. No IPC,
// authority flag, path, arbitrary FTN command or database handle is exposed.
struct SysopSessionPorts {
    ICallerTerminal& terminal;
    std::function<bool()> authorized{};
    std::function<bool()> ansi{},hotkeys{};
    std::function<unsigned()> rows{},columns{};
    std::function<bool(std::string_view)> write{};
    std::function<bool()> artwork{};
    std::function<LocalAccountSnapshot(const LocalAccountQuery&)> accounts{};
    std::function<LocalAccountResult(LocalAccountRequest&)> account_change{};
    std::function<bool()> online{},messages{};
    std::function<std::optional<FileConfiguration>()> file_configuration{};
    std::function<FileManagementReadSnapshot(const FileManagementReadRequest&)> files{};
    std::function<std::optional<std::vector<ManagedFile>>(const FileManagementQuery&)> select_all_files{};
    std::function<FileManagementJobStatus(const FileManagementTask&)> file_start{};
    std::function<FileManagementJobStatus(Identifier128)> file_status{},file_cancel{};
    // Pump caller lifetime/AFK/control while owned asynchronous work runs.
    // false cancels remaining presentation/work; never undoes committed work.
    std::function<bool(const std::function<bool()>&)> wait{};
    std::function<Identifier128()> identifier{};
    std::function<std::vector<SysopNamedChoice>()> networks{};
    std::function<std::vector<SysopNamedChoice>(std::string_view)> links{};
    std::function<std::string(std::string_view,std::string_view)> poll{};
    std::function<SysopTextPage(std::string_view,SysopFtnView,std::uint64_t)> ftn_view{};
    // null page means latest. Fixed enum only; never a caller-supplied path.
    std::function<SysopTextPage(SysopLog,std::optional<std::uint64_t>)> log{};
    std::function<bool()> newsletter{};
};
class SysopCallerSession final {
public:
    explicit SysopCallerSession(SysopSessionPorts ports):p_(std::move(ports)){}
    bool run();
private:
    bool text(std::string_view),screen(std::string_view,const std::vector<std::pair<char,std::string>>&);
    std::optional<char> command(std::string_view keys,char fallback='Q');
    std::optional<std::string> line(std::string_view,std::size_t=128,std::string initial={});
    bool confirm(std::string_view),ack(std::string_view),pause(std::string_view);
    bool callers(),accounts(),edit_account(LocalAccountSnapshot,bool),files(),ftn(),logs();
    bool file_list(FileManagementQuery,bool),file_view(const ManagedFile&);
    std::optional<FileAreaId> choose_area(const FileConfiguration&);
    std::optional<std::string> choose(std::string_view,const std::vector<SysopNamedChoice>&);
    FileManagementJobStatus job(FileManagementTask);
    bool review_file_job();
    bool show_page(const SysopTextPage&),show_log_page(const SysopTextPage&);
    SysopSessionPorts p_;
    bool input_cancelled_{};
    std::optional<FileManagementTask> pending_file_;
    FileManagementJobStatus pending_status_;
};
std::string sysop_account_result(const LocalAccountResult&);
} // namespace sfx::application
