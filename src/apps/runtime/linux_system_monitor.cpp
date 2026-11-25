#include "linux_system_monitor.hpp"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <time.h>
namespace sfx::apps::runtime {
namespace {
bool integer(std::istringstream& input,std::uint64_t& value){std::string word;if(!(input>>word))return false;const auto result=std::from_chars(word.data(),word.data()+word.size(),value);return result.ec==std::errc{}&&result.ptr==word.data()+word.size();}
bool sum(std::uint64_t& total,std::uint64_t value){if(value>UINT64_MAX-total)return false;total+=value;return true;}
std::string read_kernel(const char* path){
    std::ifstream file(path,std::ios::binary);if(!file)return {};
    std::string result;std::array<char,4096> bytes{};
    while(file){file.read(bytes.data(),bytes.size());result.append(bytes.data(),static_cast<std::size_t>(file.gcount()));if(result.size()>2*1024*1024)return {};}
    return file.bad()?std::string{}:result;
}
bool safe_name(std::string_view name){return !name.empty()&&name.size()<128&&std::all_of(name.begin(),name.end(),[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';});}
template<class Map>bool byte_rates(const Map& current,const Map& old,std::uint64_t elapsed,double& first,double& second){
    if(current.size()!=old.size()||current.empty())return false;
    std::uint64_t a{},b{};
    for(const auto& [name,value]:current){const auto found=old.find(name);if(found==old.end()||value[0]<found->second[0]||value[1]<found->second[1]||!sum(a,value[0]-found->second[0])||!sum(b,value[1]-found->second[1]))return false;}
    first=static_cast<double>(a)*1000.0/static_cast<double>(elapsed);second=static_cast<double>(b)*1000.0/static_cast<double>(elapsed);return true;
}
}
// Kernel formats: Documentation/filesystems/proc.rst and block/stat.rst.
// CPU guest time is already included in user/nice; only the first eight fields
// enter the total. I/O wait is treated as idle; a counter reset needs a new sample.
LinuxMonitorCounters parse_linux_monitor_counters(std::string_view stat,std::string_view memory,std::string_view disks,std::string_view network,const std::vector<std::string>& physical){
    LinuxMonitorCounters out;std::string line;
    std::istringstream cpu{std::string(stat)};
    while(std::getline(cpu,line)){
        std::istringstream input(line);std::string name;input>>name;if(name.rfind("cpu",0)!=0)continue;
        if(name!="cpu"&&(name.size()<4||!std::all_of(name.begin()+3,name.end(),[](char c){return c>='0'&&c<='9';})))continue;
        std::array<std::uint64_t,8> value{};bool valid=true;for(auto& field:value)valid=integer(input,field)&&valid;
        if(!valid||out.cpu.size()>=4097||!out.cpu.emplace(name,value).second){out.cpu.clear();break;}
    }
    if(out.cpu.find("cpu")==out.cpu.end())out.cpu.clear();
    std::optional<std::uint64_t> total,available;std::istringstream mem{std::string(memory)};
    while(std::getline(mem,line)){
        std::istringstream input(line);std::string name,unit;input>>name;if(name!="MemTotal:"&&name!="MemAvailable:")continue;
        std::uint64_t value{};if(!integer(input,value)||!(input>>unit)||unit!="kB"||value>UINT64_MAX/1024){total.reset();available.reset();break;}
        (name=="MemTotal:"?total:available)=value*1024;
    }
    if(total&&available&&*total&&*available<=*total)out.memory=std::array<std::uint64_t,2>{*total,*available};
    std::istringstream disk{std::string(disks)};bool disk_valid=!disks.empty()&&!physical.empty();
    while(std::getline(disk,line)){
        std::istringstream input(line);std::uint64_t major{},minor{};std::string name;if(!integer(input,major)||!integer(input,minor)||!(input>>name))continue;
        if(std::find(physical.begin(),physical.end(),name)==physical.end())continue;
        std::array<std::uint64_t,11> fields{};bool valid=true;for(auto& field:fields)valid=integer(input,field)&&valid;
        if(!valid||fields[2]>UINT64_MAX/512||fields[6]>UINT64_MAX/512||!out.disks.emplace(name,std::array<std::uint64_t,2>{fields[2]*512,fields[6]*512}).second){disk_valid=false;break;}
    }
    out.disks_available=disk_valid&&out.disks.size()==physical.size();if(!out.disks_available)out.disks.clear();
    std::istringstream net{std::string(network)};bool net_valid=!network.empty();
    while(std::getline(net,line)){
        const auto colon=line.find(':');if(colon==line.npos)continue;
        std::istringstream label(line.substr(0,colon));std::string name;label>>name;if(name=="lo")continue;
        if(!safe_name(name)||out.network.size()>=4096){net_valid=false;break;}
        std::istringstream input(line.substr(colon+1));std::array<std::uint64_t,16> fields{};bool valid=true;for(auto& field:fields)valid=integer(input,field)&&valid;
        if(!valid||!out.network.emplace(name,std::array<std::uint64_t,2>{fields[0],fields[8]}).second){net_valid=false;break;}
    }
    out.network_available=net_valid&&!out.network.empty();if(!out.network_available)out.network.clear();return out;
}
LinuxMonitorCounters read_linux_monitor_counters(){
    std::vector<std::string> physical;std::error_code error;
    try{
        for(const auto& entry:std::filesystem::directory_iterator("/sys/block",error)){
            const auto name=entry.path().filename().string();if(!safe_name(name))continue;
            std::error_code status;if(std::filesystem::exists(entry.path()/"device",status)&&!status)physical.push_back(name);
            if(physical.size()>1024){physical.clear();break;}
        }
    }catch(const std::filesystem::filesystem_error&){physical.clear();}
    if(error)physical.clear();
    return parse_linux_monitor_counters(read_kernel("/proc/stat"),read_kernel("/proc/meminfo"),read_kernel("/proc/diskstats"),read_kernel("/proc/net/dev"),physical);
}
std::optional<std::uint64_t> linux_boot_milliseconds() noexcept{
    timespec value{};if(clock_gettime(CLOCK_BOOTTIME,&value)!=0||value.tv_sec<0||static_cast<std::uint64_t>(value.tv_sec)>UINT64_MAX/1000)return {};
    return static_cast<std::uint64_t>(value.tv_sec)*1000+static_cast<std::uint64_t>(value.tv_nsec/1000000);
}
LinuxMonitorSnapshot LinuxMonitorRates::sample(const LinuxMonitorCounters& current,std::uint64_t now){
    LinuxMonitorSnapshot out;out.cpu_present=!current.cpu.empty();out.disks_present=current.disks_available;out.network_present=current.network_available;
    for(const auto& item:current.cpu)if(item.first!="cpu")out.cores.push_back(item.first);
    for(const auto& item:current.disks)out.disks.push_back(item.first);
    for(const auto& item:current.network)out.interfaces.push_back(item.first);
    if(current.memory){out.host.memory_available=true;out.host.memory_total_bytes=(*current.memory)[0];out.host.memory_available_bytes=(*current.memory)[1];out.host.memory_used_bytes=(*current.memory)[0]-(*current.memory)[1];}
    std::uint64_t received{},sent{};for(const auto& [name,value]:current.network){(void)name;if(!sum(received,value[0])||!sum(sent,value[1])){out.network_present=false;break;}}
    if(out.network_present){out.host.network_received_bytes=received;out.host.network_sent_bytes=sent;}
    if(previous_&&now>previous_ms_&&now-previous_ms_<=30000){
        const auto elapsed=now-previous_ms_;
        bool valid=out.cpu_present&&current.cpu.size()==previous_->cpu.size();
        for(const auto& [name,values]:current.cpu){
            const auto found=previous_->cpu.find(name);if(found==previous_->cpu.end()){valid=false;break;}
            std::uint64_t total{},idle{};for(std::size_t i=0;i<8;++i){if(values[i]<found->second[i]||!sum(total,values[i]-found->second[i])){valid=false;break;}if((i==3||i==4)&&!sum(idle,values[i]-found->second[i]))valid=false;}
            if(!valid||!total||idle>total){valid=false;break;}
            const auto percent=100.0*static_cast<double>(total-idle)/static_cast<double>(total);
            if(name=="cpu")out.host.total_cpu_percent=percent;else out.host.per_core_cpu_percent.push_back(percent);
        }
        out.host.cpu_available=valid;if(!valid)out.host.per_core_cpu_percent.clear();
        out.host.storage_activity_available=out.disks_present&&previous_->disks_available&&byte_rates(current.disks,previous_->disks,elapsed,out.host.storage_read_bytes_per_second,out.host.storage_write_bytes_per_second);
        out.host.network_available=out.network_present&&previous_->network_available&&byte_rates(current.network,previous_->network,elapsed,out.host.network_receive_bytes_per_second,out.host.network_send_bytes_per_second);
    }
    previous_=current;previous_ms_=now;return out;
}
}
