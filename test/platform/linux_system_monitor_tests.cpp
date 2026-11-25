#include "linux_system_monitor.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace rt=sfx::apps::runtime;
namespace {unsigned checks{};void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}}
int main(){try{
    const std::string cpu="cpu 100 20 30 100 10 2 3 5 60 10\ncpu0 100 20 30 100 10 2 3 5 60 10\n";
    const std::string disk="8 0 sda 1 2 100 4 5 6 200 8 0 10 11\n8 1 sda1 1 2 90 4 5 6 180 8 0 10 11\n7 0 loop0 1 2 500 4 5 6 500 8 0 10 11\n";
    const std::string net="Inter-| Receive | Transmit\n eth0: 1000 2 0 0 0 0 0 0 2000 3 0 0 0 0 0 0\n lo: 9000 0 0 0 0 0 0 0 9000 0 0 0 0 0 0 0\n";
    auto first=rt::parse_linux_monitor_counters(cpu,"MemTotal: 1000 kB\nMemAvailable: 400 kB\n",disk,net,{"sda"});
    check(first.cpu.size()==2&&first.cpu.at("cpu")[0]==100,"CPU counters parsed without duplicate guest fields");
    check(first.memory&&(*first.memory)[0]==1024000&&(*first.memory)[1]==409600,"memory uses MemAvailable and KiB");
    check(first.disks_available&&first.disks.size()==1&&first.disks.at("sda")[0]==51200,"whole physical disk sectors use 512 bytes without partitions or loop duplication");
    check(first.network_available&&first.network.size()==1&&first.network.at("eth0")[1]==2000,"network excludes loopback");
    rt::LinuxMonitorRates rates;auto sample=rates.sample(first,1000);check(sample.cpu_present&&!sample.host.cpu_available&&!sample.host.network_available&&!sample.host.storage_activity_available,"first sample is present but no invented rates");
    check(sample.host.memory_used_bytes==614400,"used memory exact");
    auto second=first;for(auto& item:second.cpu){item.second[0]+=20;item.second[3]+=80;}second.disks.at("sda")[0]+=1024;second.disks.at("sda")[1]+=2048;second.network.at("eth0")[0]+=600;second.network.at("eth0")[1]+=800;
    sample=rates.sample(second,3000);check(sample.host.cpu_available&&std::abs(sample.host.total_cpu_percent-20.0)<.001&&sample.host.per_core_cpu_percent.size()==1,"CPU deltas and per-core rate");
    check(sample.host.storage_activity_available&&sample.host.storage_read_bytes_per_second==512&&sample.host.storage_write_bytes_per_second==1024,"disk rates use actual elapsed milliseconds");
    check(sample.host.network_available&&sample.host.network_receive_bytes_per_second==300&&sample.host.network_send_bytes_per_second==400,"network interval rates");
    check(!rates.sample(first,4000).host.cpu_available,"counter regression rebases instead of underflow");
    check(!rates.sample(second,4000).host.network_available,"same-time sample has no interval");
    check(!rates.sample(second,35001).host.storage_activity_available,"long idle interval rebases");
    auto changed=second;changed.cpu.erase("cpu0");changed.disks.emplace("sdb",std::array<std::uint64_t,2>{1,1});changed.network.emplace("eth1",std::array<std::uint64_t,2>{1,1});
    sample=rates.sample(changed,36001);check(!sample.host.cpu_available&&!sample.host.storage_activity_available&&!sample.host.network_available,"hotplug rebases device sets");
    const auto absent=rt::parse_linux_monitor_counters("","","","",{});sample=rates.sample(absent,38000);check(!sample.cpu_present&&!sample.host.memory_available&&!sample.disks_present&&!sample.network_present,"unavailable observations never invented zero");
    check(!rt::parse_linux_monitor_counters(cpu,"MemTotal: 100 kB\nMemAvailable: 101 kB\n",disk,net,{"sda"}).memory,"invalid memory refused");
    check(!rt::parse_linux_monitor_counters(cpu,"",disk,net,{"sda","missing"}).disks_available,"partial disk set unavailable");
    check(rt::parse_linux_monitor_counters("cpu -1 2 3 4 5 6 7 8\n","","","",{}).cpu.empty(),"negative counter refused");
    check(!rt::parse_linux_monitor_counters(cpu,"","","eth0: invalid\n",{}).network_available,"malformed network unavailable");
    const auto live=rt::read_linux_monitor_counters();check(live.memory&&live.cpu.find("cpu")!=live.cpu.end(),"actual host CPU and memory readable");check(rt::linux_boot_milliseconds().value_or(0)>0,"actual OS suspend-inclusive uptime");
    std::cout<<"PASS "<<checks<<" Linux system monitor checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
