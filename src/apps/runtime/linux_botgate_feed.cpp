#include "alpha_botgate_feed.hpp"
#include "botgate_https.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <algorithm>
#include <charconv>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sfx::apps::runtime {
namespace {
namespace app=application;namespace host=platform::linux_host;
std::int64_t utc(){return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::uint64_t monotonic_now(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
bool exists(int directory,const char* name){struct stat info{};if(fstatat(directory,name,&info,AT_SYMLINK_NOFOLLOW)==0)return true;require(errno==ENOENT,"feed_cache_inspection_failed");return false;}
std::optional<std::string> download(const std::atomic<bool>& stop,AlphaBotGateFeed::Progress progress,std::string& error){auto result=https::get(https::feed_url,stop,progress);error=std::move(result.error);return std::move(result.bytes);}
std::optional<std::pair<std::int64_t,std::vector<std::uint32_t>>> load_cache(const std::filesystem::path& directory){
    auto pin=host::open_owned_directory(directory);
    if(!exists(pin.get(),"botgate-feed.ipv4"))return {};
    const auto bytes=host::read_private_file(pin.get(),"botgate-feed.ipv4",app::kBotGateFeedMaximumBytes);
    const std::string text(bytes.begin(),bytes.end());constexpr std::string_view prefix="# SFX BotGate cache v1 updated=";
    const auto end=text.find('\n');if(text.compare(0,prefix.size(),prefix)||end==text.npos||end<=prefix.size())return {};
    std::int64_t updated{};const auto parsed=std::from_chars(text.data()+prefix.size(),text.data()+end,updated);
    if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+end||updated<=0||updated>utc()+300)return {};
    auto addresses=app::parse_botgate_feed(text);if(!addresses)return {};
    return std::make_pair(updated,std::move(*addresses));
}
bool persist(const std::filesystem::path& root,const std::wstring& owner,const std::vector<std::uint32_t>& addresses,std::int64_t updated){
    require(geteuid()!=0&&owner==std::to_wstring(geteuid()),"feed_owner_mismatch");
    std::string text="# SFX BotGate cache v1 updated="+std::to_string(updated)+"\n";
    for(const auto ip:addresses)text+=std::to_string(ip>>24)+'.'+std::to_string((ip>>16)&255)+'.'+std::to_string((ip>>8)&255)+'.'+std::to_string(ip&255)+"\n";
    if(text.size()>app::kBotGateFeedMaximumBytes)return false;
    auto pin=host::open_owned_directory(root/"botgate/feeds");host::publish_private_file(pin.get(),"botgate-feed.ipv4",{text.begin(),text.end()},true);return true;
}
}
AlphaBotGateFeed::AlphaBotGateFeed(std::filesystem::path root,std::wstring owner,std::shared_ptr<AlphaOperationalJournal> journal,Fetch fetch):
    root_(std::move(root)),owner_(std::move(owner)),journal_(std::move(journal)),fetch_(fetch?std::move(fetch):Fetch(download)),active_(std::make_shared<const std::vector<std::uint32_t>>()){
    try{
        require(geteuid()!=0&&owner_==std::to_wstring(geteuid()),"feed_owner_mismatch");
        auto pin=host::open_owned_directory(root_/"botgate/feeds");auto cache=load_cache(root_/"botgate/feeds");
        if(!exists(pin.get(),"migration-v1.marker")){
            if(!exists(pin.get(),"botgate-feed.ipv4")){
                cache=load_cache(root_/"config");if(cache)require(persist(root_,owner_,cache->second,cache->first),"feed_migration_failed");
            }
            host::publish_private_file(pin.get(),"migration-v1.marker",{},false);
        }else require(host::read_private_file(pin.get(),"migration-v1.marker",64).empty(),"feed_migration_marker_invalid");
        if(cache){active_=std::make_shared<const std::vector<std::uint32_t>>(std::move(cache->second));status_.available=true;status_.last_success=cache->first;status_.entries=active_->size();status_.result="feed_cache_loaded";}
    }catch(...){status_.result="feed_cache_migration_or_security_failed";}
}
// The platform-independent scheduling/publication sequence below follows the
// accepted SFX Windows feed worker; only its monotonic clock adapter differs.
AlphaBotGateFeed::~AlphaBotGateFeed() { stop(); }
bool AlphaBotGateFeed::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || stopping_.load()) return false;
    const auto age=std::max<std::int64_t>(0,utc()-status_.last_success);
    next_due_=monotonic_now()+static_cast<std::uint64_t>(std::max<std::int64_t>(0,86400-age))*1000;
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
            if (!requested_ && monotonic_now()<next_due_) {
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
            next_due_=monotonic_now()+(success ? 86400000ULL : 3600000ULL);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.updating=false; status_.result="feed_worker_failed";
    }
}
} // namespace sfx::apps::runtime

