// Actual classifier/scanner/updater code and actual startup-policy include.
// Only the receipt reader and startup storage/OS ports are synthetic. No vendor
// process, definition download, installed board, service or host AV changes.
#include "sfx/platform/win32/clamav_update.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include "sfx/application/file_storage.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "alpha_operational_journal.hpp"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <thread>
namespace clam_policy_test {
std::filesystem::path root;std::wstring identity=L"0123456789abcdef0123456789abcdef";
bool receipt_allowed=true,win7{},installed=true;unsigned receipt_reads{},component_probes{},checks{};
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
}
namespace sfx::platform::win32::installation {
Receipt beta7_receipt(const Path& root,const std::wstring& owner){
    require(root==clam_policy_test::root&&clam_policy_test::receipt_allowed,"synthetic_receipt_refused");
    ++clam_policy_test::receipt_reads;Receipt value;value.root=root;value.owner=owner;value.id=clam_policy_test::identity;return value;
}
}
#define read_receipt beta7_receipt
#include "../../src/platform/win32/src/clamav_installation.cpp"
#undef read_receipt
namespace sfx::platform::win32 {
bool beta7_windows7_no_av()noexcept{return clam_policy_test::win7;}
bool beta7_clamav_available()noexcept{return !clam_policy_test::win7;}
bool beta7_clamav_installed(const std::filesystem::path&){++clam_policy_test::component_probes;return clam_policy_test::installed;}
}
namespace sfx::apps::runtime {
struct StartupPolicyStorage {
    application::FileConfiguration configuration;bool readable=true,writable=true;unsigned changes{};
    application::StorageResult<application::FileConfiguration> file_configuration(const application::FileConfigurationActor&){
        if(!readable)return {{application::StorageErrorCode::permission_denied,"synthetic_read_failure"},std::nullopt};
        return {application::StorageStatus::success(),configuration};
    }
    application::StorageStatus change_file_configuration(const application::FileConfigurationActor&,const application::FileConfigurationChange& change){
        ++changes;if(!writable)return {application::StorageErrorCode::permission_denied,"synthetic_save_failure"};
        configuration.policy=change.policy;++configuration.policy.revision;return {};
    }
};
class StartupPolicyHost {
public:
    application::AlphaControlResponse manual_clamav_notice(const application::ManualClamAvNotice&);
    bool initialize_clamav_locked();void clamav_loop()noexcept;void stop_clamav()noexcept;
    std::int64_t now_utc_seconds(){return 1794672000;}
    std::filesystem::path installation_root_;std::wstring owner_sid_;
    std::unique_ptr<StartupPolicyStorage> storage_=std::make_unique<StartupPolicyStorage>();
    platform::win32::WindowsSecureIdentifierGenerator identifiers_;
    std::shared_ptr<AlphaOperationalJournal> journal_;
    std::mutex clamav_notice_mutex_,clamav_mutex_;std::condition_variable clamav_wake_;
    std::vector<application::ManualClamAvNotice> clamav_notices_;
    std::thread clamav_thread_;std::atomic_bool clamav_stop_{false},scheduler_paused_{false};std::wstring clamav_installation_id_;
    ~StartupPolicyHost(){stop_clamav();}
};
#define AlphaRuntimeHost StartupPolicyHost
#define windows7_no_av beta7_windows7_no_av
#define clamav_available beta7_clamav_available
#define clamav_installed beta7_clamav_installed
#include "../../src/apps/runtime/alpha_clamav.inc"
#undef clamav_installed
#undef clamav_available
#undef windows7_no_av
#undef AlphaRuntimeHost
}
int main(int argc,char** argv){try{
    namespace native=sfx::platform::win32;namespace ins=native::installation;namespace app=sfx::application;
    using clam_policy_test::check;check(argc==2,"fresh isolated ClamAV fixture required");
    const auto root=std::filesystem::absolute(argv[1]);clam_policy_test::root=root;check(!std::filesystem::exists(root),"preserve all earlier ClamAV fixtures");
    const auto owner=ins::operator_sid();const auto mutable_sd=ins::mutable_descriptor(owner);const auto structural=ins::structural_descriptor(owner);
    for(const auto& path:{root,root/L"Bin",root/L"Bin"/L"Tools",root/L"Bin"/L"Tools"/L"ClamAV",root/L"Config",root/L"Config"/L"ClamAV",root/L"Data",root/L"Data"/L"ClamAV",root/L"Logs",root/L"Logs"/L"ClamAV",root/L"Intake",root/L"Scratch"})ins::create_directory(path,mutable_sd.get());
    const auto engine=root/L"Bin"/L"Tools"/L"ClamAV",marker=engine/L"SFX-component.receipt";
    const std::string marker_text="SFX-CLAMAV-1\n0123456789abcdef0123456789abcdef\n"+
        std::string(native::kClamAvPortableVersion)+"-win32\n"+native::kClamAvPortableSha256+"\n";
    ins::create_file(marker,{marker_text.begin(),marker_text.end()},mutable_sd.get());
    for(const auto& path:{marker,engine})check(SetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,static_cast<PSECURITY_DESCRIPTOR>(structural.get()))!=FALSE,"protect only synthetic engine/marker with actual structural DACL");
    const auto config=native::clamav_update_configuration(root);const auto configuration=root/L"Config"/L"ClamAV"/L"freshclam.conf";
    ins::create_file(configuration,{config.begin(),config.end()},mutable_sd.get());
    for(const auto* leaf:{L"Config",L"Data",L"Logs"})check(native::clamav_bootstrap_metadata(root,root/leaf/L"ClamAV",owner),"exact private metadata classifies without reading absent engine members");
    bool unverified{};try{native::verify_clamav_installation(root,owner,clam_policy_test::identity);}catch(const std::exception&){unverified=true;}
    check(unverified,"metadata classification never substitutes for full engine trust");
    auto intake=native::FilePayloadDirectory::private_store(root/L"Intake",owner);{auto file=intake.create("upload.bin",4);const std::array<std::uint8_t,4> bytes{{1,2,3,4}};file.append(bytes.data(),bytes.size());}
    auto upload=intake.read("upload.bin");const auto before=upload.identity();
    auto definitions=native::FilePayloadDirectory::private_store(root/L"Data"/L"ClamAV",owner),scratch=native::FilePayloadDirectory::private_store(root/L"Scratch",owner);
    native::WindowsFileScanner scanner(engine,owner);const auto scan=scanner.scan(upload,definitions,scratch,4,{}, {},1000);
    check(scan.result.verdict!=app::FileScanVerdict::clean&&scan.process_reaped,"real unverified/missing engine scanner refuses without claiming clean");
    check(app::incoming_file_action(true,scan.result)==app::IncomingFileAction::durable_private_hold,"required scan refusal retains upload in private hold");
    check(upload.identity().sha256==before.sha256,"scanner refusal preserves exact private input");
    const auto update=native::update_clamav_definitions(root,owner,clam_policy_test::identity,1794672000,{}, {},1000);
    check(!update.attempted&&!update.succeeded&&update.process_reaped&&update.cleanup_complete,"real updater rejects unverified engine before vendor/network attempt");
    check(ins::read_file(configuration,65536)==std::vector<std::uint8_t>(config.begin(),config.end()),"refused update preserves private configuration");
    clam_policy_test::receipt_allowed=false;check(!native::clamav_bootstrap_metadata(root,root/L"Config"/L"ClamAV",owner),"receipt failure is not a bootstrap metadata exemption");clam_policy_test::receipt_allowed=true;
    clam_policy_test::identity=L"fedcba9876543210fedcba9876543210";check(!native::clamav_bootstrap_metadata(root,root/L"Config"/L"ClamAV",owner),"component marker stays bound to installation identity");clam_policy_test::identity=L"0123456789abcdef0123456789abcdef";
    {std::ofstream changed(configuration,std::ios::binary|std::ios::trunc);changed<<"not the generated configuration\n";}
    check(!native::clamav_bootstrap_metadata(root,root/L"Config"/L"ClamAV",owner),"edited metadata configuration remains conservative persistent-state evidence");
    auto data=native::FilePayloadDirectory::private_store(root/L"Data"/L"ClamAV",owner);{auto unknown=data.create("unknown.bin",1);const std::uint8_t byte=1;unknown.append(&byte,1);}
    check(!native::clamav_bootstrap_metadata(root,root/L"Data"/L"ClamAV",owner),"unknown metadata child remains conservative persistent-state evidence");
    const auto policy_case=[&](std::int64_t revision,bool scanning,bool win7,bool present,bool readable,bool writable,bool expected,unsigned changes,bool after){
        sfx::apps::runtime::StartupPolicyHost host;host.installation_root_=root;host.owner_sid_=owner;
        host.storage_->configuration.policy.revision=revision;host.storage_->configuration.policy.scan_uploads=scanning;host.storage_->readable=readable;host.storage_->writable=writable;
        app::Identifier128 epoch;epoch.bytes[0]=1;host.journal_=std::make_shared<sfx::apps::runtime::AlphaOperationalJournal>(root/L"Logs"/L"policy.log",owner,epoch);
        clam_policy_test::win7=win7;clam_policy_test::installed=present;clam_policy_test::component_probes=0;const auto receipts=clam_policy_test::receipt_reads;
        check(host.initialize_clamav_locked()==expected,"actual startup policy include retains essential read/save failure result");
        check(host.storage_->changes==changes&&host.storage_->configuration.policy.scan_uploads==after,"seed/saved/Win7 policy semantics preserved");
        check(!host.clamav_thread_.joinable()&&clam_policy_test::receipt_reads==receipts&&host.journal_->snapshot().empty(),"initial policy path performs no engine receipt/hash/definition/thread work");
        if(win7)check(clam_policy_test::component_probes==0,"Win7 no-AV policy performs no optional component probe");
    };
    policy_case(1,false,false,true,true,true,true,1,true);
    policy_case(5,false,false,true,true,true,true,0,false);
    policy_case(5,true,false,true,true,true,true,0,true);
    policy_case(1,false,false,false,true,true,true,0,false);
    policy_case(1,false,false,true,false,true,false,0,false);
    policy_case(1,false,false,true,true,false,false,1,false);
    policy_case(5,true,true,true,true,true,true,1,false);
    std::cout<<"PASS "<<clam_policy_test::checks<<" actual classifier/refusal and startup-policy checks; synthetic receipt/storage/OS ports, no vendor/installed/core-start/physical Win7 claim\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<clam_policy_test::checks<<": "<<error.what()<<" win32="<<GetLastError()<<'\n';return 1;}}
