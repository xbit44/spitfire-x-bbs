// Bounded explicit engineering workload; all writes stay in a fresh supplied
// scratch directory. Independent PDH query brackets the shipping sampler.
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include "sfx/platform/win32/control_panel_metrics.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
struct Query {
    PDH_HQUERY query{};
    PDH_HCOUNTER idle{}, old{}, reads{}, writes{};
    Query() {
        require(PdhOpenQueryW(nullptr,0,&query)==ERROR_SUCCESS,"reference query");
        require(PdhAddEnglishCounterW(query,L"\\PhysicalDisk(*)\\% Idle Time",0,&idle)==ERROR_SUCCESS &&
            PdhAddEnglishCounterW(query,L"\\PhysicalDisk(_Total)\\% Disk Time",0,&old)==ERROR_SUCCESS &&
            PdhAddEnglishCounterW(query,L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",0,&reads)==ERROR_SUCCESS &&
            PdhAddEnglishCounterW(query,L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",0,&writes)==ERROR_SUCCESS,
            "reference English counters");
        require(PdhCollectQueryData(query)==ERROR_SUCCESS,"reference baseline");
    }
    ~Query() { if(query) PdhCloseQuery(query); }
    double get(PDH_HCOUNTER counter) {
        PDH_FMT_COUNTERVALUE v{};
        require(PdhGetFormattedCounterValue(counter,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,nullptr,&v)==ERROR_SUCCESS &&
            (v.CStatus==PDH_CSTATUS_VALID_DATA || v.CStatus==PDH_CSTATUS_NEW_DATA),"reference format");
        return v.doubleValue;
    }
    double busiest() {
        DWORD bytes{},count{};
        require(PdhGetFormattedCounterArrayW(idle,PDH_FMT_DOUBLE,&bytes,&count,nullptr)==PDH_MORE_DATA &&
            bytes>0 && bytes<1024*1024,"reference array size");
        std::vector<unsigned char> buffer(bytes);
        auto* values=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        require(PdhGetFormattedCounterArrayW(idle,PDH_FMT_DOUBLE,&bytes,&count,values)==ERROR_SUCCESS,"reference array");
        double busy=-1;
        for(DWORD i=0;i<count;++i) {
            if(_wcsicmp(values[i].szName,L"_Total")==0) continue;
            const auto& v=values[i].FmtValue;
            require(v.CStatus==PDH_CSTATUS_VALID_DATA || v.CStatus==PDH_CSTATUS_NEW_DATA,"reference disk status");
            busy=std::max(busy,100.0-std::clamp(v.doubleValue,0.0,100.0));
        }
        require(busy>=0,"at least one physical disk");return busy;
    }
};

int wmain(int argc,wchar_t** argv) {
    try {
        require(argc==3,"fresh scratch path and source search path required");
        const fs::path root=fs::absolute(argv[1]).lexically_normal(), search=fs::absolute(argv[2]);
        require(root.parent_path().filename()==L"build" && root.filename().wstring().find(L"section11-monitor-io-")==0 &&
            !fs::exists(root),"exclusive repository build scratch");
        fs::create_directory(root);fs::create_directory(root/L"Data");fs::create_directory(root/L"Backup");
        std::ofstream output(root/L"samples.csv");
        output<<std::setprecision(12)<<"phase,sample,available,throughput,reference_idle_busiest,old_disk_time,reads,reference_reads,writes,reference_writes,sampler_ms\n";
        std::atomic<unsigned> phase{0};std::atomic<bool> stop{false};
        std::atomic<unsigned long long> walked{0},read_bytes{0},write_bytes{0};
        std::atomic<bool> failed{false};
        void* memory=VirtualAlloc(nullptr,1024*1024,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        require(memory!=nullptr,"aligned unbuffered I/O memory");
        for(unsigned i=0;i<1024*1024;++i) static_cast<unsigned char*>(memory)[i]=static_cast<unsigned char>((i*37U)^0x5aU);
        const HANDLE file=CreateFileW((root/L"controlled.bin").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,
            CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH,nullptr);
        require(file!=INVALID_HANDLE_VALUE,"exclusive controlled data file");
        // Seed only this 64 MiB file; subsequent phases reuse its fixed extent.
        for(unsigned i=0;i<64;++i) { DWORD n{};require(WriteFile(file,memory,1024*1024,&n,nullptr)&&n==1024*1024,"seed controlled file"); }
        std::thread worker([&] {
            unsigned block=0;
            try {
                while(!stop.load()) {
                    const unsigned work=phase.load();
                    if(work==1) {
                        std::error_code error;
                        fs::recursive_directory_iterator it(search,fs::directory_options::skip_permission_denied,error),end;
                        for(;it!=end&&!stop.load()&&phase.load()==1;it.increment(error)) {
                            if(error) {error.clear();continue;}
                            if(it->is_symlink(error)) it.disable_recursion_pending();
                            (void)(it->path().filename().wstring().find(L"memory.zip")!=std::wstring::npos);++walked;
                        }
                        Sleep(50);
                    } else if(work==2 || work==3) {
                        LARGE_INTEGER offset{};offset.QuadPart=static_cast<LONGLONG>(block++%64U)*1024*1024;
                        require(SetFilePointerEx(file,offset,nullptr,FILE_BEGIN)!=FALSE,"controlled seek");
                        DWORD n{};
                        if(work==2) {require(WriteFile(file,memory,1024*1024,&n,nullptr)&&n==1024*1024,"controlled write");write_bytes+=n;}
                        else {require(ReadFile(file,memory,1024*1024,&n,nullptr)&&n==1024*1024,"controlled read");read_bytes+=n;}
                        Sleep(30); // <=34 MiB/s, bounded 10-second phases
                    } else Sleep(10);
                }
            } catch(...) {failed=true;}
        });
        try {
            Query reference;
            sfx::platform::win32::WindowsControlPanelMetricSampler sampler(root);
            unsigned serial=0;
            for(unsigned stage=0;stage<5;++stage) {
                const char* names[]={"idle","search","write","read","recovery"};
                phase=stage==4?0:stage;
                for(unsigned j=0;j<(stage==0||stage==4?8U:10U);++j) {
                    Sleep(1000);
                    require(PdhCollectQueryData(reference.query)==ERROR_SUCCESS,"reference collect");
                    const auto start=std::chrono::steady_clock::now();const auto current=sampler.sample();
                    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                    output<<names[stage]<<','<<serial++<<','<<current.storage_activity_available<<','<<(current.storage_read_bytes_per_second+current.storage_write_bytes_per_second)<<','
                        <<reference.busiest()<<','<<reference.get(reference.old)<<','<<current.storage_read_bytes_per_second<<','<<reference.get(reference.reads)<<','
                        <<current.storage_write_bytes_per_second<<','<<reference.get(reference.writes)<<','<<ms<<'\n';
                    require(current.storage_activity_available,"shipping disk sample available");
                }
            }
            Sleep(3500);require(!sampler.sample().storage_activity_available,"long hidden interval re-primes");
            Sleep(1000);require(sampler.sample().storage_activity_available,"fresh visible interval resumes");
        } catch(...) {stop=true;worker.join();CloseHandle(file);VirtualFree(memory,0,MEM_RELEASE);throw;}
        stop=true;worker.join();CloseHandle(file);VirtualFree(memory,0,MEM_RELEASE);
        require(!failed && walked>0 && write_bytes>0 && read_bytes>0,"all bounded workloads completed");
        std::ofstream(root/L"workload.txt")<<"walked="<<walked<<"\nwrite_bytes="<<write_bytes<<"\nread_bytes="<<read_bytes<<'\n';
        std::cout<<"PASS shipping sampler and independent Windows counters; bounded idle/search/write/read/recovery; hidden re-prime\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
