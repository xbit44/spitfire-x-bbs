#include "sfx/platform/win32/file_scanner.hpp"
#include "sfx/platform/win32/clamav_resources.hpp"
#include <iostream>
namespace win=sfx::platform::win32;namespace ins=win::installation;namespace app=sfx::application;
int wmain(int argc,wchar_t** argv)try{
    ins::require(argc==2,"exact diagnostic install required");
    const auto root=ins::canonical_root(argv[1]);const auto owner=ins::operator_sid();
    ins::require(root==ins::Path(L"C:\\Program Files\\SFX-Section9-Test-20261017-r4")&&!ins::elevated(),"owned fixture only");
    auto work=win::FilePayloadDirectory::private_store(root/L"Work",owner);
    auto definitions=win::FilePayloadDirectory::private_store(root/L"Data"/L"ClamAV",owner);
    const auto wide=ins::unique_id();std::string token;for(const auto ch:wide)token+=static_cast<char>(ch);const auto name="scanner-diagnostic-"+token+".txt";
    win::WindowsFileScanner scanner(root/L"Bin"/L"Tools"/L"ClamAV",owner);
    app::FilePayloadIdentity original;
    const auto scan=[&](win::FilePayloadLease& payload,const char* label){
        auto job=win::ClamAvScratch::create(root/L"Work"/L"ClamAV",owner);
        win::NativeFileScanResult result;
        {auto scratch=win::FilePayloadDirectory::private_store(job->path(),owner);result=scanner.scan(payload,definitions,scratch,app::kDefaultCallerFileBytes);}
        ins::require(job->cleanup(),"exact diagnostic scratch cleanup");
        std::cout<<label<<" verdict="<<unsigned(result.result.verdict)<<" exit="<<result.exit_code<<" reaped="<<result.process_reaped<<" phase="<<result.diagnostic<<" detail="<<result.result.detail<<std::endl;
        ins::require(result.process_reaped,"diagnostic child reaped");return result;
    };
    {auto payload=work.create(name,100);const std::string body="Harmless scanner handle comparison.\r\n";payload.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());original=payload.identity();
        (void)scan(payload,"sealed-receive");}
    {auto payload=work.read(name);ins::require(payload.identity().sha256==original.sha256,"same exact diagnostic bytes");(void)scan(payload,"read-only");}
    {auto payload=work.read(name,true);ins::require(payload.identity().sha256==original.sha256,"diagnostic bytes unchanged");payload.remove_exact();}
    std::cout<<"PASS comparison complete; original upload untouched\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
