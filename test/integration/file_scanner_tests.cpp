#include "sfx/platform/win32/file_scanner.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <iostream>
#include <algorithm>

namespace native=sfx::platform::win32;
namespace app=sfx::application;
namespace ins=native::installation;
namespace {
unsigned checks{};
void check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
void write(native::FilePayloadDirectory& destination,const std::string& name,const std::vector<std::uint8_t>& bytes){
    auto file=destination.create(name,std::max<std::uint64_t>(1,bytes.size()));
    for(std::size_t at=0;at<bytes.size();){const auto n=std::min<std::size_t>(65536,bytes.size()-at);file.append(bytes.data()+at,n);at+=n;}
    file.identity();
}
void describe(const char* label,const native::NativeFileScanResult& result){
    std::cout<<label<<": verdict="<<static_cast<unsigned>(result.result.verdict)<<" exit="<<result.exit_code<<" elapsed_ms="<<result.elapsed_milliseconds<<" phase="<<result.diagnostic<<" detail="<<result.result.detail<<'\n';
}
}
int main(int argc,char** argv){try{
    check(argc==4,"new isolated root, verified engine source and official definitions source required");
    const auto root=ins::canonical_root(std::filesystem::absolute(argv[1]));
    check(!std::filesystem::exists(root),"fresh fixture only");
    const auto engine_source=ins::canonical_root(std::filesystem::absolute(argv[2]));
    const auto definitions_source=ins::canonical_root(std::filesystem::absolute(argv[3]));
    const auto owner=ins::operator_sid();const auto security=ins::mutable_descriptor(owner);
    for(const auto& path:{root,root/L"engine",root/L"definitions",root/L"empty-definitions",root/L"input",root/L"scratch-clean",root/L"scratch-receive",root/L"scratch-timeout",root/L"scratch-cancel",root/L"scratch-error",root/L"scratch-ignored"})ins::create_directory(path,security.get());
    auto engine=native::FilePayloadDirectory::private_store(root/L"engine",owner);
    for(const auto& member:native::clamav_program_members()){
        const auto bytes=ins::read_file(engine_source/member.name,32U*1024U*1024U);
        check(bytes.size()==member.bytes&&ins::sha256(bytes)==member.sha256,"verified official engine dependency before copying");
        // This isolated component engine is executable test input, not an
        // uploaded payload (whose file policy deliberately denies execution).
        // Installed Setup's separate protected-Bin receipt test is still due.
        ins::create_file(engine.path()/member.name,bytes,security.get());
    }
    auto definitions=native::FilePayloadDirectory::private_store(root/L"definitions",owner);
    for(const auto* name:{"main.cvd","daily.cvd","bytecode.cvd"})write(definitions,name,ins::read_file(definitions_source/name,128U*1024U*1024U));
    auto inputs=native::FilePayloadDirectory::private_store(root/L"input",owner);
    const std::string text="Harmless SFX Section9 native supervised clean scan.\r\n";write(inputs,"clean.txt",{text.begin(),text.end()});
    native::WindowsFileScanner scanner(engine.path(),owner);const auto original=inputs.read("clean.txt").identity();
    {
        auto payload=inputs.read("clean.txt");auto scratch=native::FilePayloadDirectory::private_store(root/L"scratch-clean",owner);
        unsigned pending{};bool locked{};
        const auto result=scanner.scan(payload,definitions,scratch,app::kDefaultCallerFileBytes,{},[&](std::uint64_t){
            ++pending;const auto h=CreateFileW(payload.path().c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
            locked=h==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION;if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
        });describe("actual clean",result);
        check(result.result.verdict==app::FileScanVerdict::clean&&result.exit_code==0&&result.process_reaped,"actual supervised clean scan and positive child reap");
        check(pending>0&&locked,"activity while pending and payload write denied");
        check(payload.identity().sha256==original.sha256,"clean scan leaves payload exact");
    }
    {
        auto payload=inputs.read("clean.txt");auto scratch=native::FilePayloadDirectory::private_store(root/L"scratch-timeout",owner);
        const auto result=scanner.scan(payload,definitions,scratch,app::kDefaultCallerFileBytes,{}, {},1);describe("actual timeout",result);
        check(result.result.verdict==app::FileScanVerdict::timeout&&result.process_reaped,"bounded whole-scan deadline and no surviving child");
        check(payload.identity().sha256==original.sha256,"timeout retains exact payload");
    }
    {
        auto payload=inputs.read("clean.txt");auto scratch=native::FilePayloadDirectory::private_store(root/L"scratch-cancel",owner);
        unsigned calls{};bool running{};const auto result=scanner.scan(payload,definitions,scratch,app::kDefaultCallerFileBytes,[&]{++calls;return running;},[&](std::uint64_t){running=true;});describe("actual cancel",result);
        check(result.result.verdict==app::FileScanVerdict::incomplete&&calls>2&&result.process_reaped,"actual in-flight cancellation positively reaps and retains file");
        check(payload.identity().sha256==original.sha256,"cancel retains exact payload");
    }
    {
        auto payload=inputs.read("clean.txt");auto scratch=native::FilePayloadDirectory::private_store(root/L"scratch-error",owner);
        const auto empty=native::FilePayloadDirectory::private_store(root/L"empty-definitions",owner);
        const auto result=scanner.scan(payload,empty,scratch,app::kDefaultCallerFileBytes);describe("missing definitions",result);
        check(result.result.verdict==app::FileScanVerdict::error&&result.diagnostic=="scan_database_missing","missing definitions fails closed");
        check(payload.identity().sha256==original.sha256,"failed preflight leaves payload exact");
    }
    {
        // Synthetic inert filename only: no signature suppression content.
        write(definitions,"local.ign2",{});auto payload=inputs.read("clean.txt");auto scratch=native::FilePayloadDirectory::private_store(root/L"scratch-ignored",owner);
        const auto result=scanner.scan(payload,definitions,scratch,app::kDefaultCallerFileBytes);describe("unapproved definitions sidecar",result);
        check(result.result.verdict==app::FileScanVerdict::error&&result.diagnostic=="scan_unexpected_database_file","unapproved signature filter never loaded");
    }
    {
        auto received=inputs.create("received.txt",100);const std::string body="Harmless received upload.\r\n";received.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());const auto before=received.identity();
        auto scratch=native::FilePayloadDirectory::private_store(root/L"scratch-receive",owner);
        const auto result=scanner.scan(received,definitions,scratch,app::kDefaultCallerFileBytes);describe("sealed receive handle",result);
        check(result.result.verdict==app::FileScanVerdict::error,"unapproved sidecar still refuses sealed-handle scan");
        {auto sidecar=definitions.read("local.ign2",true);sidecar.remove_exact();}
        {auto exact=scratch.read("SFX-scan-input.bin",true);exact.remove_exact();}
        const auto clean=scanner.scan(received,definitions,scratch,app::kDefaultCallerFileBytes);describe("sealed receive clean",clean);
        check(clean.result.verdict==app::FileScanVerdict::clean&&clean.exit_code==0&&clean.process_reaped,"sealed receive scans exact read-only copy successfully");
        check(received.identity().sha256==before.sha256,"sealed receive source unchanged");
    }
    check(inputs.read("clean.txt").identity().sha256==original.sha256,"all paths leave original input unchanged");
    std::cout<<"PASS native scanner "<<checks<<" checks; real ClamAV clean/deadline/cancel, no malware detection, not installed Setup/intake/client qualification\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native scanner after "<<checks<<": "<<e.what()<<" win32="<<GetLastError()<<'\n';return 1;}}
