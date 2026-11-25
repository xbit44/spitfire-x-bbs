#include "alpha_botgate_feed.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "botgate_https.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <windows.h>
#include <sddl.h>
#include <algorithm>
#include <charconv>
#include <chrono>

namespace sfx::apps::runtime {
namespace {
namespace app=application;
namespace win=platform::win32;
namespace install=win::installation;
std::int64_t utc() { return std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::system_clock::now().time_since_epoch()).count(); }
std::optional<std::string> download(const std::atomic<bool>& stop,AlphaBotGateFeed::Progress progress,std::string& error) {
    auto result=https::get(https::feed_url,stop,progress);
    error=std::move(result.error);
    return std::move(result.bytes);
}
std::vector<install::Handle> pin(const std::filesystem::path& root,const std::wstring& owner,
    const std::filesystem::path& role=L"BotGate\\feeds") {
    std::vector<std::filesystem::path> paths;
    for (auto path=root/role; !path.empty();) {
        paths.push_back(path); const auto parent=path.parent_path(); if (parent==path) break; path=parent;
    }
    std::reverse(paths.begin(),paths.end());
    std::vector<install::Handle> held;
    for (const auto& path:paths) held.push_back(install::open_object(path,true,READ_CONTROL|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE));
    install::require(win::WindowsStorageDaclPolicy(owner).assess(root/role,win::StorageDaclObjectKind::directory).status.succeeded(),
        "feed_cache_directory_security_invalid");
    return held;
}
std::optional<std::pair<std::int64_t,std::vector<std::uint32_t>>> load_cache(
    const std::filesystem::path& root,const std::wstring& owner) {
    try {
        auto role=std::filesystem::path(L"BotGate\\feeds");
        const auto attributes=GetFileAttributesW((root/role/L"BotGate-feed.ipv4").c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES && GetLastError()==ERROR_FILE_NOT_FOUND) {
            const auto marker=root/L"BotGate"/L"feeds"/L"migration-v1.marker";
            if(GetFileAttributesW(marker.c_str())!=INVALID_FILE_ATTRIBUTES || GetLastError()!=ERROR_FILE_NOT_FOUND) return {};
            role=L"Config";
        }
        auto held=pin(root,owner,role);
        const auto path=root/role/L"BotGate-feed.ipv4";
        auto file=install::open_object(path,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
        if (!win::WindowsStorageDaclPolicy(owner).assess(path,win::StorageDaclObjectKind::file).status) return {};
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(),&size) || size.QuadPart<=0 || size.QuadPart>app::kBotGateFeedMaximumBytes) return {};
        std::string text(static_cast<std::size_t>(size.QuadPart),'\0'); DWORD count{};
        if (!ReadFile(file.get(),text.data(),static_cast<DWORD>(text.size()),&count,nullptr) || count!=text.size()) return {};
        constexpr std::string_view prefix="# SFX BotGate cache v1 updated=";
        const auto end=text.find('\n');
        if (text.compare(0,prefix.size(),prefix) || end==text.npos || end<=prefix.size()) return {};
        std::int64_t updated{};
        auto parsed=std::from_chars(text.data()+prefix.size(),text.data()+end,updated);
        if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+end || updated<=0 || updated>utc()+300) return {};
        auto addresses=app::parse_botgate_feed(text);
        if (!addresses) return {};
        return std::make_pair(updated,std::move(*addresses));
    } catch (...) { return {}; }
}
bool persist(const std::filesystem::path& root,const std::wstring& owner,
    const std::vector<std::uint32_t>& addresses,std::int64_t updated) {
    std::string text="# SFX BotGate cache v1 updated="+std::to_string(updated)+"\n";
    for (const auto ip:addresses) text+=std::to_string(ip>>24)+'.'+std::to_string((ip>>16)&255)+'.'+
        std::to_string((ip>>8)&255)+'.'+std::to_string(ip&255)+"\n";
    if (text.size()>app::kBotGateFeedMaximumBytes) return false;
    auto held=pin(root,owner);
    const auto final=root/L"BotGate"/L"feeds"/L"BotGate-feed.ipv4";
    win::WindowsStorageDaclPolicy policy(owner);
    const auto attributes=GetFileAttributesW(final.c_str());
    if (attributes!=INVALID_FILE_ATTRIBUTES) {
        auto current=install::open_object(final,false,READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);
        if (!policy.assess(final,win::StorageDaclObjectKind::file).status) return false;
    } else if (GetLastError()!=ERROR_FILE_NOT_FOUND) return false;
    const auto pending=root/L"BotGate"/L"feeds"/(L"BotGate-feed-"+install::unique_id()+L".pending");
    // File ACL, not the inheritable mutable-directory ACL. The two are
    // intentionally different under the shared storage policy.
    PSECURITY_DESCRIPTOR raw_descriptor{};
    const auto sddl=L"O:"+owner+L"D:P(A;;0x13019f;;;"+owner+L")(A;;FA;;;SY)(A;;FA;;;BA)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw_descriptor,nullptr)) return false;
    install::Descriptor descriptor(raw_descriptor);
    SECURITY_ATTRIBUTES attributes_security{sizeof(SECURITY_ATTRIBUTES),descriptor.get(),FALSE};
    install::Handle candidate;
    {win::ServiceFileOwnerScope scope(owner);if(!scope.valid())return false;
        candidate.reset(CreateFileW(pending.c_str(),GENERIC_WRITE|READ_CONTROL|DELETE,FILE_SHARE_READ,
            &attributes_security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_WRITE_THROUGH,nullptr));}
    if (candidate.get()==INVALID_HANDLE_VALUE) { candidate.release(); return false; }
    struct PendingCleanup {
        HANDLE handle; bool published{};
        ~PendingCleanup() { if (!published) { FILE_DISPOSITION_INFO remove{TRUE};
            (void)SetFileInformationByHandle(handle,FileDispositionInfo,&remove,sizeof(remove)); } }
    } cleanup{candidate.get()};
    DWORD written{};
    if (!WriteFile(candidate.get(),text.data(),static_cast<DWORD>(text.size()),&written,nullptr) || written!=text.size() ||
        !FlushFileBuffers(candidate.get()) || !install::exact_security(candidate.get(),descriptor.get())) return false;
    const auto& target=final.native();
    std::vector<std::uint8_t> rename_buffer(sizeof(FILE_RENAME_INFO)+(target.size()+1)*sizeof(wchar_t));
    auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(rename_buffer.data());
    rename->ReplaceIfExists=TRUE; rename->RootDirectory=nullptr;
    rename->FileNameLength=static_cast<DWORD>(target.size()*sizeof(wchar_t));
    std::copy(target.begin(),target.end(),rename->FileName);
    if (!SetFileInformationByHandle(candidate.get(),FileRenameInfo,rename,static_cast<DWORD>(rename_buffer.size())))
        throw std::runtime_error("feed_cache_rename_error_"+std::to_string(GetLastError()));
    cleanup.published=true;
    return true;
}
}
AlphaBotGateFeed::AlphaBotGateFeed(std::filesystem::path root,std::wstring owner,
    std::shared_ptr<AlphaOperationalJournal> journal,Fetch fetch)
    : root_(std::move(root)),owner_(std::move(owner)),journal_(std::move(journal)),
      fetch_(fetch ? std::move(fetch) : Fetch(download)),active_(std::make_shared<const std::vector<std::uint32_t>>()) {
    try {
        auto held=pin(root_,owner_);
        auto cache=load_cache(root_,owner_);
        const auto marker=root_/L"BotGate"/L"feeds"/L"migration-v1.marker";
        if(GetFileAttributesW(marker.c_str())==INVALID_FILE_ATTRIBUTES) {
            install::require(GetLastError()==ERROR_FILE_NOT_FOUND,"feed_migration_marker_unavailable");
            const auto canonical=root_/L"BotGate"/L"feeds"/L"BotGate-feed.ipv4";
            if(cache && GetFileAttributesW(canonical.c_str())==INVALID_FILE_ATTRIBUTES)
                install::require(persist(root_,owner_,cache->second,cache->first),"feed_migration_failed");
            auto descriptor=install::mutable_descriptor(owner_);install::create_file(marker,{},descriptor.get());
            install::require(win::WindowsStorageDaclPolicy(owner_).apply(marker,win::StorageDaclObjectKind::file).succeeded(),"feed_migration_marker_security_invalid");
        } else {
            auto marker_handle=install::open_object(marker,false);
            install::require(win::WindowsStorageDaclPolicy(owner_).assess(marker,win::StorageDaclObjectKind::file).status.succeeded(),"feed_migration_marker_security_invalid");
        }
        if(cache) {
            active_=std::make_shared<const std::vector<std::uint32_t>>(std::move(cache->second));
            status_.available=true;status_.last_success=cache->first;status_.entries=active_->size();status_.result="feed_cache_loaded";
        }
    } catch(...) {status_.result="feed_cache_migration_or_security_failed";}
}
AlphaBotGateFeed::~AlphaBotGateFeed() { stop(); }
bool AlphaBotGateFeed::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || stopping_.load()) return false;
    const auto age=std::max<std::int64_t>(0,utc()-status_.last_success);
    next_due_=GetTickCount64()+static_cast<std::uint64_t>(std::max<std::int64_t>(0,86400-age))*1000;
    try { worker_=std::thread([this] { run(); }); started_=true; return true; }
    catch (...) { status_.result="feed_worker_unavailable"; return false; }
}
void AlphaBotGateFeed::stop() noexcept {
    stopping_.store(true); changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}
bool AlphaBotGateFeed::update_now() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopping_.load() || requested_ || status_.updating) return false;
        requested_=true; changed_.notify_one(); return true;
    } catch (...) { return false; }
}
BotGateFeedStatus AlphaBotGateFeed::status() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }
std::shared_ptr<const std::vector<std::uint32_t>> AlphaBotGateFeed::snapshot() const noexcept { return std::atomic_load(&active_); }
void AlphaBotGateFeed::run() noexcept {
    try {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_.load()) {
            if (!requested_ && GetTickCount64()<next_due_) {
                changed_.wait_for(lock,std::chrono::seconds(1)); continue;
            }
            requested_=false; status_.updating=true; status_.last_attempt=utc(); status_.bytes_received=0;
            status_.result="feed_downloading";
            lock.unlock();
            if (journal_) journal_->emit({app::OperationalKind::feed_update_started,{}, {}});
            bool success=false; std::string outcome="feed_update_exception";
            std::shared_ptr<const std::vector<std::uint32_t>> replacement;
            std::int64_t completed=0;
            try {
                auto bytes=fetch_(stopping_,[this](std::uint64_t count) {
                    std::lock_guard<std::mutex> update(mutex_); status_.bytes_received=count;
                },outcome);
                if (bytes && !stopping_.load()) {
                    { std::lock_guard<std::mutex> update(mutex_); status_.result="feed_validating"; }
                    auto parsed=app::parse_botgate_feed(*bytes);
                    if (!parsed) outcome="feed_candidate_invalid";
                    else {
                        replacement=std::make_shared<const std::vector<std::uint32_t>>(std::move(*parsed));
                        completed=utc(); outcome="feed_cache_publication_failed";
                        success=persist(root_,owner_,*replacement,completed);
                        if (success) std::atomic_store(&active_,replacement);
                    }
                }
            } catch (const std::runtime_error& error) {
                const std::string detail=error.what();
                outcome=detail.find("feed_cache_rename_error_")==0 ? detail : "feed_update_exception";
            } catch (...) { outcome="feed_update_exception"; }
            if (success) outcome="feed_update_success";
            if (journal_) {
                app::OperationalContext context; context.diagnostic_code=outcome;
                journal_->emit({success ? app::OperationalKind::feed_update_completed : app::OperationalKind::feed_update_failed,{},context});
            }
            lock.lock();
            status_.updating=false; status_.result=outcome;
            if (success) { status_.available=true; status_.last_success=completed; status_.entries=replacement->size(); }
            next_due_=GetTickCount64()+(success ? 86400000ULL : 3600000ULL);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.updating=false; status_.result="feed_worker_failed";
    }
}
} // namespace sfx::apps::runtime
