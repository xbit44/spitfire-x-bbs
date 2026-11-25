#include "alpha_operational_journal.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;namespace rt=sfx::apps::runtime;namespace win=sfx::platform::win32;
namespace {unsigned checks{};void check(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}}
int main(int argc,char** argv)try{
    check(argc==2,"new bounded fixture required");const auto root=std::filesystem::absolute(argv[1]).lexically_normal();
    const auto repo=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    check(root!=repo/L"build"&&root.lexically_relative(repo/L"build").native().find(L"..")==std::wstring::npos&&!std::filesystem::exists(root),"fresh test root under own build");
    std::filesystem::create_directories(root/L"Logs");const auto sid=win::current_process_user_sid_string();win::WindowsStorageDaclPolicy policy(sid);
    check(policy.apply(root/L"Logs",win::StorageDaclObjectKind::directory).succeeded(),"private log directory");
    win::WindowsSecureIdentifierGenerator ids;const auto epoch=ids.generate();rt::AlphaOperationalJournal journal(root/L"Logs"/L"SFXRuntime.log",sid,epoch);
    app::SoundPollRequest cursor{ids.generate(),{},0,false};app::EventSoundConfiguration paths;
    const auto poll=[&]{auto events=journal.sound_poll(cursor,paths);cursor.epoch=events.epoch;cursor.after=events.through;return events.events;};
    check(poll().empty(),"no old sound history on attach");
    journal.emit({app::OperationalKind::backup_completed,{},{}});journal.flush();check(poll().empty(),"real journal success remains silent");
    journal.emit({app::OperationalKind::backup_failed,{},{}});journal.flush();auto events=poll();
    check(events.size()==1&&events[0].event==app::SoundEvent::operational_alert,"real typed failed backup routes Operational once");
    app::AttentionRecord finding;finding.kind=app::AttentionKind::clamav_finding;finding.identity=app::AttentionIdentity::verified;finding.utc_seconds=1'800'000'000;finding.node=1;finding.account_name="Synthetic Caller";finding.filename="sample.bin";finding.signatures={"Synthetic.Test.Signature"};
    journal.clamav_finding(finding);journal.flush();events=poll();
    check(events.size()==1&&events[0].event==app::SoundEvent::security_alert,"confirmed-finding sink routes Security without Operational duplicate");
    (void)journal.snapshot();(void)journal.reporting();check(poll().empty(),"reading log/history does not replay confirmed finding");
    const auto fault_root=root/L"fault";std::filesystem::create_directories(fault_root/L"Logs");
    check(policy.apply(fault_root/L"Logs",win::StorageDaclObjectKind::directory).succeeded(),"separate private fault log directory");
    {std::ofstream file(fault_root/L"Logs"/L"SFXRuntime.log");}
    HANDLE held=CreateFileW((fault_root/L"Logs"/L"SFXRuntime.log").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(held!=INVALID_HANDLE_VALUE,"hold only owned test log against writes");
    struct Release {HANDLE handle;~Release(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}} release{held};
    rt::AlphaOperationalJournal fault(fault_root/L"Logs"/L"SFXRuntime.log",sid,ids.generate());
    app::SoundPollRequest fault_cursor{ids.generate(),{},0,false};
    const auto fault_poll=[&]{auto result=fault.sound_poll(fault_cursor,paths);fault_cursor.epoch=result.epoch;fault_cursor.after=result.through;return result.events;};
    check(fault_poll().empty(),"fault consumer attaches before the failed write");
    fault.emit({app::OperationalKind::configuration_saved,{},{}});(void)fault.flush();events=fault_poll();
    check((fault.health()&app::operational_log_write_failed)&&events.size()==1&&events[0].event==app::SoundEvent::operational_alert,"actual denied log write produces one degradation event");
    fault.emit({app::OperationalKind::configuration_saved,{},{}});(void)fault.flush();check(fault_poll().empty(),"ongoing degraded log does not loop alerts");
    std::cout<<"PASS "<<checks<<" real journal sound checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
