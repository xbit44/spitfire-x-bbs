#pragma once
#include "setup_engine.hpp"
#include "sfx/platform/win32/backup_collections.hpp"

namespace sfx::apps::setup {
// Coordinator-thread only. Counts are observations, never estimates or an ETA.
// All chunk callbacks may call this cheaply; Windows text updates are throttled.
class MaintenanceProgress final {
public:
    explicit MaintenanceProgress(Progress output):output_(std::move(output)),started_(GetTickCount64()){}
    void status(const std::wstring& text){text_=text;counted_=false;retention_=false;emit(true);}
    void backup(const archive::BackupProgressEvent& event,bool published=false){
        const auto next=published?L"Verifying published backup":
            event.stage==archive::BackupStage::inventory?L"Preparing backup: hashing source files":
            event.stage==archive::BackupStage::writing?L"Writing backup ZIP":
            event.stage==archive::BackupStage::verification?L"Verifying backup ZIP":L"Publishing verified backup";
        const bool changed=text_!=next;text_=next;event_=event;counted_=true;retention_=false;emit(changed);
    }
    void retention(std::size_t done,std::size_t total){
        text_=L"Checking retained upgrade backups";counted_=false;
        retention_=true;done_=done;total_=total;emit(true);
    }
    void finalization(platform::win32::BackupFinalizationStage stage){
        switch(stage){
        case platform::win32::BackupFinalizationStage::existing_archives:status(L"Checking existing upgrade backups");break;
        case platform::win32::BackupFinalizationStage::published_archive:status(L"Verifying the new backup");break;
        case platform::win32::BackupFinalizationStage::ownership:status(L"Verifying backup ownership");break;
        case platform::win32::BackupFinalizationStage::dependencies:status(L"Checking backup dependencies");break;
        case platform::win32::BackupFinalizationStage::retirement:status(L"Finishing backup retention");break;
        }
    }
    void heartbeat(){emit(false);}
private:
    void emit(bool force){
        const auto now=GetTickCount64();if(!force&&now-last_<100)return;last_=now;
        auto message=text_;
        if(counted_){
            message+=L"\nBytes: "+std::to_wstring(event_.completed_bytes);
            if(event_.total_bytes)message+=L" / "+std::to_wstring(event_.total_bytes);
            message+=L"; files: "+std::to_wstring(event_.completed_files);
            if(event_.total_files)message+=L" / "+std::to_wstring(event_.total_files);
            if(!event_.total_bytes&&!event_.total_files)message+=L" (total not yet known)";
        }else if(retention_)message+=L"\nExisting archives checked: "+std::to_wstring(done_)+L" / "+std::to_wstring(total_);
        const auto seconds=(now-started_)/1000;
        message+=L"\nTotal since confirmation: "+std::to_wstring(seconds)+(seconds==1?L" second":L" seconds");
        if(output_)output_(message);
    }
    Progress output_;std::wstring text_;
    ULONGLONG started_{},last_{};bool counted_{},retention_{};
    std::size_t done_{},total_{};archive::BackupProgressEvent event_;
};
}
