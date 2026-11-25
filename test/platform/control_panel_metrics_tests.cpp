#include "sfx/platform/win32/control_panel_metrics.hpp"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
    namespace fs = std::filesystem;
    unsigned checks{};
    const auto require=[&](bool ok,const char* label) { ++checks; if(!ok) throw std::runtime_error(label); };
    const fs::path root = fs::path(SFX_METRIC_SCRATCH) /
        (L"monitor-volumes-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    bool created=false;
    try {
        require(fs::create_directory(root),"exclusive test root"); created=true;
        require(fs::create_directory(root/L"Data") && fs::create_directory(root/L"Backup"),"two empty volume probes");
        sfx::platform::win32::WindowsControlPanelMetricSampler metrics(root);
        const auto sample=metrics.sample();
        require(sample.database_volume_available && sample.backup_volume_available && sample.shared_storage_volume,
            "database and canonical backup on same volume deduplicate by identity");
        require(sample.database_volume_free_bytes>0 && sample.backup_volume_free_bytes>0,
            "actual Win32 available capacity sampled");
        require(fs::is_empty(root/L"Data") && fs::is_empty(root/L"Backup"),"sampling writes no BBS data or logs");
        sfx::platform::win32::WindowsControlPanelMetricSampler missing(L"?:\\invalid-sfx-volume");
        const auto unavailable=missing.sample();
        require(!unavailable.database_volume_available && !unavailable.backup_volume_available &&
            !unavailable.shared_storage_volume,"unresolvable volumes are unavailable, not fabricated zero capacity");
        require(unavailable.memory_available,"failed volume does not disable independent metrics");
        require(fs::remove(root/L"Data") && fs::remove(root/L"Backup") && fs::remove(root),"remove only empty created directories");
        std::cout<<"PASS monitor metrics: "<<checks<<" checks\n"; return 0;
    } catch(const std::exception& error) {
        if(created) { std::error_code ignored; fs::remove(root/L"Data",ignored); fs::remove(root/L"Backup",ignored); fs::remove(root,ignored); }
        std::cerr<<"FAIL monitor metrics: "<<error.what()<<'\n'; return 1;
    }
}
