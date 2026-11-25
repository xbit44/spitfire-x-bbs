#pragma once
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
namespace sfx::apps::runtime {
// Runtime-owned completion survives loss of its terminal. No detached threads,
// and destroying a caller's future never joins a worker that is revoking it.
class SysopWork final {
public:
    ~SysopWork(){stop();}
    template<class F> auto submit(F function)->std::future<decltype(function())>{
        using R=decltype(function());auto task=std::make_shared<std::packaged_task<R()>>(std::move(function));auto result=task->get_future();
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_||queue_.size()>=128)throw std::runtime_error("sysop_work_unavailable");
        if(!thread_.joinable())thread_=std::thread([this]{run();});
        queue_.push_back([task]{(*task)();});changed_.notify_one();return result;
    }
    void stop(){ {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;changed_.notify_all();}if(thread_.joinable())thread_.join(); }
private:
    void run(){for(;;){std::function<void()> work;{std::unique_lock<std::mutex> lock(mutex_);changed_.wait(lock,[&]{return stopping_||!queue_.empty();});if(queue_.empty())return;work=std::move(queue_.front());queue_.pop_front();}work();}}
    std::mutex mutex_;std::condition_variable changed_;std::deque<std::function<void()>> queue_;std::thread thread_;bool stopping_{};
};
}
