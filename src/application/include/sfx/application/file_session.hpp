#pragma once
#include "sfx/application/file_presentation.hpp"
#include "sfx/application/file_storage.hpp"
#include "sfx/application/phase3a_storage.hpp"
#include "sfx/application/menu.hpp"

namespace sfx::application {
struct CallerFileReport {
    std::string name, detail;
    FileScanResult scan;
    FileId file;
    std::uint64_t bytes{};
    bool completed{}, held{}, deleted{}, skipped{};
    bool received{}; // Proven complete receive, separate from durable acceptance.
};
struct CallerFileBatchResult { bool connected{true}; std::vector<CallerFileReport> files; };
struct CallerUploadReview { FileDescriptions descriptions; bool sysop_only{}; };
struct FileSessionEffect {
    std::string notice;
    std::optional<MenuId> destination{};
    bool goodbye{};
};
struct FileSessionContext { std::string group, area; std::uint16_t number{}; FileAreaId id; };
struct FileSessionPorts {
    IFileStorage& storage;
    FileListPorts list;
    std::function<std::optional<FileActor>()> actor{};
    std::function<FileAuthority()> authority{};
    std::function<Identifier128()> operation{};
    std::function<std::string(std::int64_t)> date{};
    std::function<std::optional<std::int64_t>(std::string_view)> scan_date{};
    std::function<std::string()> caller_name{};
    std::function<std::optional<TransferProtocolPreference>()> protocol{};
    std::function<CallerFileBatchResult(const std::vector<FileUploadItem>&,bool,TransferProtocolPreference)> upload{};
    std::function<CallerFileBatchResult(const std::vector<FileId>&,TransferProtocolPreference)> download{};
    std::function<bool(FileId,bool archive)> view{};
    // Already reserve-adjusted and capped, not raw host drive free space.
    std::function<std::optional<std::uint64_t>(FileAreaId)> upload_space{};
    // null = no custom display; true = displayed; false = output failure.
    std::function<std::optional<bool>(bool groups)> selection_artwork{};
    // Host capability only, not the saved scanning-policy switch.
    std::function<bool()> malware_scanning_available{};
    std::function<bool(TransferProtocolPreference)> protocol_available{};
    // Connected report: true = log off, false = Abort/return, null = lost caller.
    std::function<std::optional<bool>()> transfer_logoff{};
};
// Per authenticated connection. No physical paths or payload ownership here.
// Storage rechecks every action; native transfer ports own payload acceptance.
class FileCallerSession final {
public:
    explicit FileCallerSession(FileSessionPorts);
    std::optional<FileSessionContext> context();
    std::optional<FileSessionEffect> action(MenuAction,MenuId);
    // null = connection failure; false = cancelled; true = proceed.
    std::optional<bool> leave(MenuId from,MenuId to);
    bool login_scan();
    std::optional<CallerUploadReview> review_upload(const FileUploadItem&,
        const std::optional<FileDescriptions>& diz,std::uint64_t bytes);
    const FileUploadQueue& uploads()const noexcept{return uploads_;}
    const FileDownloadQueue& downloads()const noexcept{return downloads_;}
private:
    FileSessionPorts p_;
    FileCatalog catalog_;
    FileCallerState state_;
    FileAreaId current_;
    FileSort sort_{FileSort::filename};
    FileUploadQueue uploads_;
    FileDownloadQueue downloads_;
    bool prefer_diz_{true};
    bool refresh();
    const FileArea* area(FileAreaId)const;
    const FileGroup* group(FileGroupId)const;
    bool say(std::string_view);
    bool options(const std::vector<std::pair<char,std::string>>&);
    std::optional<char> key(std::string identity,std::string prompt,std::vector<char>,char fallback=0,bool advance_line=true);
    std::optional<bool> yes(std::string identity,std::string prompt,bool default_yes);
    std::optional<std::string> text(std::string identity,std::string prompt,std::size_t max);
    std::optional<bool> change_area();
    std::optional<FileId> choose_exact(std::string_view prompt,bool downloadable);
    bool information(FileId);
    bool browse(FileQuery,std::vector<FileAreaId> completed_scan_areas={});
    bool search(FileSearchKind);
    bool new_scan(bool login);
    bool settings();
    bool tag(FileId,std::int64_t revision);
    bool add_uploads(bool private_submission,std::vector<FileUploadItem>* private_items=nullptr);
    std::optional<FileSessionEffect> add_downloads();
    bool queue_view(bool upload,bool download);
    bool queue_remove(bool upload,bool download);
    std::optional<bool> clear(bool upload,bool download,bool returning=false);
    std::optional<FileSessionEffect> transfer(bool upload,bool private_submission=false);
    std::optional<FileDescriptions> descriptions(FileDescriptions initial,bool force_short,bool* sysop_only);
};
} // namespace sfx::application
