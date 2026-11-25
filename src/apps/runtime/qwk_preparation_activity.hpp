#pragma once
#include "sfx/archive/qwk_packet_builder.hpp"
#include "sfx/application/file_scan.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sfx::apps::runtime {
using QwkPreparationWork=std::function<archive::QwkPreparedDownload(archive::CancelCheck,archive::QwkPreparationObserver)>;
using QwkActivityWrite=std::function<bool(std::string_view)>;

inline std::string qwk_activity_frame(std::uint64_t elapsed,std::size_t columns){
    const auto width=columns?std::min<std::size_t>(24,columns-1):0;
    if(width==24)return application::file_scan_activity(elapsed);
    if(width<=5)return std::string(width,'#');
    const auto original=application::file_scan_activity(elapsed);
    const auto at=original.find('#')*(width-5)/19;
    std::string frame(width,' ');frame.replace(at,5,5,'#');return frame;
}

// Only the calling thread touches terminal/host state. The joined builder
// worker has one bounded rendezvous slot for the original live callbacks.
// There is no guard cache, input polling or background terminal output.
inline archive::QwkPreparedDownload prepare_qwk_with_activity(QwkPreparationWork work,
    archive::CancelCheck cancelled,archive::QwkPreparationObserver progress,
    QwkActivityWrite write,std::size_t columns){
    using Clock=std::chrono::steady_clock;
    const auto failure=[](archive::Error error,const char* text){archive::QwkPreparedDownload r;r.result={error,text};return r;};
    const auto cancelled_result=[&]{return failure(archive::Error::cancelled,"Packet preparation cancelled; nothing delivered.");};
    bool visible=false,output_ok=true;
    const auto clear=[&]{if(!visible)return true;visible=false;return output_ok=write&&write("\r"+std::string(qwk_activity_frame(0,columns).size(),' ')+"\r");};
    const auto began=Clock::now();
    const auto draw=[&]{const auto frame=qwk_activity_frame(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-began).count()),columns);if(frame.empty())return true;visible=true;return output_ok=write&&write("\r"+frame);};
    try{
        if(!work)return failure(archive::Error::invalid_request,"Packet preparation worker unavailable.");
        if(cancelled&&cancelled())return cancelled_result();
        if(!draw())return cancelled_result();
        struct Bridge {
            std::mutex mutex;std::condition_variable changed;
            bool pending{},answered{},answer{},stopping{},done{};
            std::optional<archive::QwkPreparationEvent> event;
            archive::QwkPreparedDownload result;
            bool ask(std::optional<archive::QwkPreparationEvent> value){
                std::unique_lock<std::mutex> lock(mutex);if(stopping)return false;
                event=std::move(value);pending=true;answered=false;changed.notify_all();
                changed.wait(lock,[&]{return answered||stopping;});return !stopping&&answer;
            }
            void stop(){std::lock_guard<std::mutex> lock(mutex);stopping=true;changed.notify_all();}
        } bridge;
        std::thread worker([&]{
            archive::QwkPreparedDownload result;
            try{result=work([&]{return !bridge.ask({});},[&](const auto& event){return bridge.ask(event);});}
            catch(...){result=failure(archive::Error::archive_io,"Packet preparation failed; nothing delivered.");}
            std::lock_guard<std::mutex> lock(bridge.mutex);bridge.result=std::move(result);bridge.done=true;bridge.changed.notify_all();
        });
        struct Join {
            Bridge& bridge;std::thread& worker;
            ~Join(){bridge.stop();if(worker.joinable())worker.join();}
        } join{bridge,worker};
        bool refused=false;auto next=began+std::chrono::milliseconds(500);
        for(;;){
            std::optional<archive::QwkPreparationEvent> event;bool request=false;
            {
                std::unique_lock<std::mutex> lock(bridge.mutex);
                bridge.changed.wait_until(lock,next,[&]{return bridge.pending||bridge.done;});
                if(bridge.done)break;
                if(bridge.pending){request=true;event=bridge.event;bridge.pending=false;}
            }
            if(request){
                bool allowed=!cancelled||!cancelled();
                if(allowed&&event){allowed=clear()&&(!progress||progress(*event));if(allowed)allowed=draw();}
                {std::lock_guard<std::mutex> lock(bridge.mutex);bridge.answer=allowed;bridge.answered=true;bridge.changed.notify_all();}
                if(!allowed){refused=true;bridge.stop();break;}
            }
            if(Clock::now()>=next){
                if((cancelled&&cancelled())||!draw()){refused=true;bridge.stop();break;}
                next=Clock::now()+std::chrono::milliseconds(500);
            }
        }
        // Joining precedes result inspection, clearing and every later prompt
        // or binary handoff. Never return a proxy capturing this bridge.
        if(worker.joinable())worker.join();
        auto packet=std::move(bridge.result);
        if(packet.progress)packet.progress->cancelled=cancelled;
        if(!refused&&packet.result.ok()&&cancelled&&cancelled())refused=true;
        if(output_ok&&!clear())refused=true;
        return refused?cancelled_result():std::move(packet);
    }catch(...){
        if(output_ok){try{(void)clear();}catch(...){}}
        return failure(archive::Error::archive_io,"Packet preparation failed; nothing delivered.");
    }
}
}
