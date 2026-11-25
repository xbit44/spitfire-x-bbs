#include "linux_hostname_lookup.hpp"
#include "linux_verified_domain.hpp"
#include "sfx/application/botgate.hpp"
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
namespace sfx::apps::runtime {
class LinuxHostnameLookup::Impl {
public:
    std::filesystem::path root;bool enabled;std::atomic<bool> stopping{false};std::mutex mutex;
    std::condition_variable changed;std::deque<std::weak_ptr<application::CallerAssignment>> queue;std::array<std::thread,4> workers;
    Impl(std::filesystem::path path,bool use):root(std::move(path)),enabled(use){
        if(enabled)try{for(auto& worker:workers)worker=std::thread([this]{run();});}catch(...){stop();throw;}
    }
    ~Impl(){stop();}
    void stop()noexcept{
        stopping=true;changed.notify_all();for(auto& worker:workers)if(worker.joinable())worker.join();
        std::lock_guard<std::mutex> lock(mutex);queue.clear();
    }
    void run()noexcept{
        for(;;){
            std::weak_ptr<application::CallerAssignment> next;
            {std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[&]{return stopping.load()||!queue.empty();});if(stopping)return;next=queue.front();queue.pop_front();}
            try{
                const auto assignment=next.lock();if(!assignment||assignment->stopping.load())continue;
                const auto identity=assignment->identity();const auto source=assignment->snapshot().source_ip;const auto ip=application::parse_botgate_ipv4(source);if(!ip)continue;
                const auto hint=lookup_linux_hostname(root,*ip,[&]{return stopping.load()||assignment->stopping.load();});
                if(hint&&!stopping&&!assignment->stopping.load())assignment->hostname(identity,source,hint->view());
            }catch(...){/* Numeric source remains authoritative. */}
        }
    }
};
LinuxHostnameLookup::LinuxHostnameLookup(std::filesystem::path root,bool enabled):impl_(std::make_unique<Impl>(std::move(root),enabled)){}
LinuxHostnameLookup::~LinuxHostnameLookup()=default;
void LinuxHostnameLookup::stop()noexcept{impl_->stop();}
void LinuxHostnameLookup::subscribe(const std::shared_ptr<application::CallerAssignment>& assignment){
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->enabled||impl_->stopping||!assignment||impl_->queue.size()>=application::kAlphaNodePoolCapacity)return;
    impl_->queue.push_back(assignment);impl_->changed.notify_one();
}
}
