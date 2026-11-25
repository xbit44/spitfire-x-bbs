#include "fixture_paths.hpp"
#include "sfx/platform/win32/portable_key_store.hpp"
#include <windows.h>
#include <sddl.h>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace win=sfx::platform::win32;
namespace fs=std::filesystem;
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
std::wstring user_sid(){HANDLE token{};check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)!=FALSE,"ordinary token");DWORD n{};GetTokenInformation(token,TokenUser,nullptr,0,&n);std::vector<unsigned char> bytes(n);const auto ok=GetTokenInformation(token,TokenUser,bytes.data(),n,&n);CloseHandle(token);check(ok!=FALSE,"current token SID");LPWSTR sid{};check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid)!=FALSE,"SID text");std::wstring result(sid);LocalFree(sid);return result;}
std::vector<std::uint8_t> read(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void write(const fs::path& p,const std::vector<std::uint8_t>& bytes){std::ofstream out(p,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());check(bool(out),"owned synthetic fault write");}
int main(int argc,char** argv)try{
    check(argc==2,"new owned key-test fixture required");auto root=fs::absolute(argv[1]).lexically_normal();root.make_preferred();const auto prefix=(sfx::test::fixture_checkout() / L"build/section16-portable-r1/key-fixtures").make_preferred().native();
    check(root.native().rfind(prefix+L"\\",0)==0&&!fs::exists(root),"only a new contained synthetic key fixture");fs::create_directories(root/L"source/Config");const auto source=root/L"source";const auto sid=user_sid();
    check(win::load_portable_board_key(source).status==win::PortableKeyStatus::missing&&!fs::exists(source/L"Config/board.key"),"missing-key read does not create files");
    auto first=win::create_portable_board_key(source,sid);if(!first)std::cerr<<first.diagnostic<<" error="<<first.native_error<<'\n';check(bool(first),"explicit fresh-key creation and marker publication");const auto key_bytes=read(source/L"Config/board.key"),meta_bytes=read(source/L"Config/portable-credentials.v1");
    check(key_bytes.size()==88&&!meta_bytes.empty(),"complete bounded key/marker records");
    auto again=win::create_portable_board_key(source,sid);check(again&&again.value->board==first.value->board&&read(source/L"Config/board.key")==key_bytes,"repeat explicit initialization preserves original key/identity");
    constexpr win::PortableCredentialFamily family{'S','F','X','F'};const std::string text="synthetic copied credential";
    const auto envelope=win::seal_portable_credential(first.value->key,first.value->board,family,"FTN/TEST/LOCAL/session",text);check(bool(envelope),"synthetic portable credential");
    const auto copy_to=[&](const wchar_t* leaf){const auto path=root/leaf;fs::create_directory(path);fs::copy(source/L"Config",path/L"Config",fs::copy_options::recursive);return path;};
    const auto copied=copy_to(L"another-root");auto loaded=win::load_portable_board_key(copied);check(loaded&&loaded.value->board==first.value->board,"copied folder carries the same portable key identity");
    const auto clear=win::open_portable_credential(loaded.value->key,loaded.value->board,family,"FTN/TEST/LOCAL/session",*envelope);check(clear&&clear->view()==text,"actual copied key decodes without original path or DPAPI parameters");
    const auto missing=copy_to(L"missing-key");fs::rename(missing/L"Config/board.key",missing/L"Config/board.key.saved-fault");
    check(win::load_portable_board_key(missing).status==win::PortableKeyStatus::missing,"missing established key reported");
    check(!win::create_portable_board_key(missing,sid)&&!fs::exists(missing/L"Config/board.key")&&read(missing/L"Config/portable-credentials.v1")==meta_bytes,"established marker prevents key regeneration; original evidence retained");
    const auto corrupt=copy_to(L"corrupt-key");fs::copy_file(corrupt/L"Config/board.key",corrupt/L"Config/board.key.before-fault");auto changed=key_bytes;changed[32]^=1;write(corrupt/L"Config/board.key",changed);
    check(win::load_portable_board_key(corrupt).status==win::PortableKeyStatus::invalid&&!win::create_portable_board_key(corrupt,sid)&&read(corrupt/L"Config/board.key")==changed,"corrupt key is never replaced or regenerated");
    const auto mismatch=copy_to(L"mismatched-marker");fs::copy_file(mismatch/L"Config/portable-credentials.v1",mismatch/L"Config/marker.before-fault");auto wrong=meta_bytes;const auto newline=std::find(wrong.begin(),wrong.end(),'\n');check(newline!=wrong.end()&&newline+1!=wrong.end(),"synthetic marker framing");*(newline+1)^=1;write(mismatch/L"Config/portable-credentials.v1",wrong);
    check(win::load_portable_board_key(mismatch).status==win::PortableKeyStatus::invalid&&!win::create_portable_board_key(mismatch,sid)&&read(mismatch/L"Config/board.key")==key_bytes,"mismatched board metadata refused without key mutation");
    const auto linked=copy_to(L"linked-key");check(CreateHardLinkW((linked/L"Config/board.key.second-link").c_str(),(linked/L"Config/board.key").c_str(),nullptr)!=FALSE,"owned hardlink fault fixture");check(win::load_portable_board_key(linked).status==win::PortableKeyStatus::invalid,"linked private-key object refused");
    check(read(source/L"Config/board.key")==key_bytes&&read(source/L"Config/portable-credentials.v1")==meta_bytes,"all original key/marker bytes retained");
    std::cout<<"PASS "<<checks<<" native portable key-store checks; same-host copy only, migration/replacement context separate\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}
