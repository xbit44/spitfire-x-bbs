// Executable-only link seam. The single-purpose clamav_host.obj is not pulled
// from the library when this executable supplies it. NO override in shipping
// programs, environment variables, registry values or product command lines.
#include "sfx/platform/win32/clamav_update.hpp"
#include "clamav_download.hpp"
#include <iostream>
namespace win=sfx::platform::win32;namespace ins=win::installation;
namespace sfx::platform::win32 {ClamAvHost clamav_host() noexcept{return ClamAvHost::windows7_no_av;}}
int main(int argc,char** argv){try{
    unsigned n{};const auto check=[&](bool x,const char* why){++n;ins::require(x,why);std::cout<<"PASS "<<why<<'\n';};
    if(argc==3&&std::string(argv[1])=="--retire"){
        const auto root=ins::canonical_root(std::filesystem::absolute(argv[2]));
        check(root==ins::Path(L"C:\\Program Files\\SFX-Section9-Test-20261017-r29"),"exact disposable migration fixture only");
        const auto owner=ins::operator_sid();const auto receipt=ins::read_receipt(root,owner);ins::verify_structure(receipt);
        check(ins::elevated(),"normal administrator consent for protected component removal");
        auto retirement=win::ClamAvRetirement::prepare(root,owner,receipt.id);retirement->retire();
        for(const auto* role:{L"Bin/Tools/ClamAV",L"Config/ClamAV",L"Data/ClamAV",L"Logs/ClamAV",L"Work/ClamAV"})check(GetFileAttributesW((root/role).c_str())==INVALID_FILE_ATTRIBUTES,"exact owned AV role absent");
        std::cout<<"PASS native migration "<<n<<" checks; simulated Win7 policy on actual disposable host installation\n";return 0;
    }
    check(argc==1,"closed guard test arguments");
    check(win::windows7_no_av()&&!win::clamav_available(),"Win7 has no AV capability");
    const ins::Path absent_root=L"C:\\SFX-noav-must-not-exist";
    check(GetFileAttributesW(absent_root.c_str())==INVALID_FILE_ATTRIBUTES,"negative test root absent");
    const auto status=win::inspect_clamav_status(absent_root,L"",L"");check(!status.installed&&!status.verified&&!status.definitions_verified&&status.diagnostic=="clamav_host_unavailable","status does not inspect or claim an engine");
    for(const auto mode:{win::ClamAvUpdateMode::automatic,win::ClamAvUpdateMode::manual}){const auto r=win::update_clamav_definitions(absent_root,L"",L"",1,{},{},1,mode);check(!r.attempted&&!r.succeeded&&r.process_reaped&&r.cleanup_complete&&r.diagnostic=="clamav_host_unavailable","automatic/manual update stops before files or vendor child");}
    bool refused{};try{win::initialize_clamav_data(absent_root,L"");}catch(...){refused=true;}check(refused,"data initialization refused without creating roles");
    refused=false;try{win::provision_clamav_engine(absent_root,L"",L"",L"absent.zip");}catch(...){refused=true;}check(refused,"engine provisioning refused before package/files");
    bool progress{};refused=false;try{(void)sfx::apps::setup::DownloadedClamAv::fetch([&](const auto&){progress=true;},{});}catch(...){refused=true;}check(refused&&!progress,"download refused before network or installer AV progress");
    const auto owner=ins::operator_sid();wchar_t temp[MAX_PATH]{};check(GetTempPathW(MAX_PATH,temp)>0,"native fixture parent");const auto root=ins::Path(temp)/(L"SFX-NoAV-"+ins::unique_id());ins::create_directory(root,ins::mutable_descriptor(owner).get());
    {auto directory=win::FilePayloadDirectory::private_store(root,owner);auto input=directory.create("input.bin",1);const std::uint8_t byte=7;input.append(&byte,1);win::WindowsFileScanner scanner(absent_root,owner);const auto r=scanner.scan(input,directory,directory,1);check(r.result.verdict==sfx::application::FileScanVerdict::error&&r.diagnostic=="scan_host_unavailable"&&r.exit_code==0xffffffffU&&r.process_reaped,"scanner refuses before engine, scratch, admission or child");input.remove_exact();}
    check(RemoveDirectoryW(root.c_str())!=FALSE,"exact harmless guard fixture removed");
    check(GetFileAttributesW(absent_root.c_str())==INVALID_FILE_ATTRIBUTES,"guard calls created no component directory");
    std::cout<<"PASS Win7 no-AV guards "<<n<<" checks; simulated host policy, no physical Win7 claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
