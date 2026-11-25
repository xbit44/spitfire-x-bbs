#include "fixture_paths.hpp"
#include "sfx/platform/win32/portable_credentials.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include <windows.h>
#include <sddl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace win=sfx::platform::win32;namespace app=sfx::application;namespace fs=std::filesystem;
unsigned checks{};void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
std::wstring sid(){HANDLE token{};check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)!=FALSE,"ordinary native token");DWORD size{};GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<unsigned char> b(size);check(GetTokenInformation(token,TokenUser,b.data(),size,&size)!=FALSE,"native user SID");CloseHandle(token);LPWSTR text{};check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid,&text)!=FALSE,"SID text");std::wstring result(text);LocalFree(text);return result;}
std::vector<std::uint8_t> read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void write(const fs::path& path,const std::vector<std::uint8_t>& bytes){std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());check(bool(out),"owned fault written");}
template<class F>bool refused(F f){try{f();return false;}catch(const std::exception&){return true;}}
int main(int argc,char** argv)try{
    check(argc==2,"new migration fixture required");auto root=fs::absolute(argv[1]).lexically_normal();root.make_preferred();const auto prefix=(sfx::test::fixture_checkout() / L"build/section16-portable-r1/migration-fixtures").make_preferred().native();
    check(root.native().rfind(prefix+L"\\",0)==0&&!fs::exists(root),"new contained fixture only");const auto owner=sid();const auto source=root/L"original";fs::create_directories(source/L"Config");
    app::FtnConfiguration ftn;app::FtnLink link;link.network="TESTNET";link.code="PEER";const std::string secret="Test7";
    for(const auto item:{std::pair{win::FtnCredentialKind::session,&link.credentials.session},std::pair{win::FtnCredentialKind::packet,&link.credentials.packet},std::pair{win::FtnCredentialKind::areafix,&link.credentials.areafix},std::pair{win::FtnCredentialKind::tic,&link.credentials.tic}}){const auto encrypted=win::protect_legacy_ftn_secret(source.u8string(),link.network,link.code,item.first,secret);check(bool(encrypted),"real Windows legacy FTN fixture");*item.second=*encrypted;}
    ftn.links.push_back(link);app::DoorCatalog doors;app::DoorDefinition door;const auto credential=win::protect_rlogin_secret("remote-only"),pepper=win::protect_rlogin_secret("pepper-only");check(credential&&pepper,"real Windows legacy RLogin fixtures");door.rlogin.protected_credential=*credential;door.rlogin.protected_pepper=*pepper;doors.doors.push_back(door);
    const auto ftn_bytes=app::encode_ftn_configuration(ftn);const auto original_tic=link.credentials.tic;const auto original_generation=win::legacy_credential_generation(original_tic);
    win::prepare_portable_credentials(source,owner,ftn,doors);check(win::load_portable_board_key(source)&&win::read_portable_credential_aliases(source).size()==6,"all six verified values converted once");
    check(app::encode_ftn_configuration(ftn)==ftn_bytes&&doors.doors.front().rlogin.protected_credential==*credential,"migration leaves catalog bytes/revisions and pending original ciphertext exact");
    const auto alias=win::portable_credential_alias(source,original_tic);check(alias&&app::retained_portable_ftn_generation(*alias)==original_generation,"pending TIC generation retained in authenticated alias");
    const auto key_before=read(source/L"Config/board.key"),aliases_before=read(source/L"Config/portable-secrets.v1");win::prepare_portable_credentials(source,owner,ftn,doors);
    check(read(source/L"Config/board.key")==key_before&&read(source/L"Config/portable-secrets.v1")==aliases_before,"ordinary retry is byte-exact and does not reencrypt");
    const auto copy=[&](const wchar_t* leaf){const auto target=root/leaf;fs::create_directory(target);fs::copy(source/L"Config",target/L"Config",fs::copy_options::recursive);return target;};
    const auto copied=copy(L"different-root");
    check(!win::unprotect_legacy_ftn_secret(copied.u8string(),link.network,link.code,win::FtnCredentialKind::session,link.credentials.session),"original FTN DPAPI path purpose unavailable at copied root");
    for(const auto item:{std::pair{win::FtnCredentialKind::session,&link.credentials.session},std::pair{win::FtnCredentialKind::packet,&link.credentials.packet},std::pair{win::FtnCredentialKind::areafix,&link.credentials.areafix},std::pair{win::FtnCredentialKind::tic,&link.credentials.tic}}){const auto clear=win::unprotect_ftn_secret(copied.u8string(),link.network,link.code,item.first,*item.second);check(clear&&clear->view()==secret,"copied alias actually decodes without old FTN DPAPI purpose");}
    const auto remote=win::unprotect_rlogin_secret(copied,*credential),salt=win::unprotect_rlogin_secret(copied,*pepper);check(remote&&remote->view()=="remote-only"&&salt&&salt->view()=="pepper-only","both copied RLogin portable aliases decode");
    const auto saved=win::protect_ftn_secret(source.u8string(),link.network,link.code,win::FtnCredentialKind::tic,secret);check(saved&&(*saved)[4]==3&&app::retained_portable_ftn_generation(*saved)!=original_generation,"new FTN save uses portable encryption and a new generation");
    const auto saved_remote=win::protect_rlogin_secret(source,"new-remote");check(saved_remote&&(*saved_remote)[4]==3&&win::unprotect_rlogin_secret(copied,*saved_remote)->view()=="new-remote","new RLogin save decodes from a copied board");
    auto missing=copy(L"missing-key");fs::rename(missing/L"Config/board.key",missing/L"Config/key.before-fault");
    check(!win::unprotect_rlogin_secret(missing,*credential)&&!win::unprotect_ftn_secret(missing.u8string(),link.network,link.code,win::FtnCredentialKind::tic,original_tic),"established board never falls back to still-readable local DPAPI when key missing");
    check(refused([&]{win::prepare_portable_credentials(missing,owner,ftn,doors);})&&!fs::exists(missing/L"Config/board.key"),"missing key cannot be regenerated by migration retry");
    const auto absent=root/L"all-key-metadata-absent";fs::create_directories(absent/L"Config");auto modern=ftn;modern.links.front().credentials={};modern.links.front().credentials.tic=*saved;
    check(refused([&]{win::prepare_portable_credentials(absent,owner,modern,{});})&&!fs::exists(absent/L"Config/board.key"),"v3 catalog with all key metadata absent prevents silent new-key generation");
    const auto bad=root/L"unreadable-legacy";fs::create_directories(bad/L"Config");auto unreadable=ftn;
    auto& bad_values=unreadable.links.front().credentials;
    for(const auto entry:{std::pair{win::FtnCredentialKind::session,&bad_values.session},std::pair{win::FtnCredentialKind::packet,&bad_values.packet},std::pair{win::FtnCredentialKind::areafix,&bad_values.areafix},std::pair{win::FtnCredentialKind::tic,&bad_values.tic}})*entry.second=*win::protect_legacy_ftn_secret(bad.u8string(),link.network,link.code,entry.first,secret);
    unreadable.links.front().credentials.session.back()^=1;const auto unreadable_before=app::encode_ftn_configuration(unreadable);
    const auto partial=win::prepare_portable_credentials(bad,owner,unreadable,doors);
    check(partial.issues.size()==1&&partial.converted==5&&win::load_portable_board_key(bad)&&win::read_portable_credential_aliases(bad).size()==5&&app::encode_ftn_configuration(unreadable)==unreadable_before,"one unreadable unused legacy value is identified and exact; other values convert and new saves remain possible");
    check(!win::valid_ftn_protected_credentials(bad.u8string(),unreadable)&&win::valid_changed_ftn_credentials(bad.u8string(),unreadable,unreadable),"unreadable retained value is unavailable for use but does not trap an unrelated configuration save");
    auto edited=unreadable;edited.links.front().credentials.packet.back()^=1;
    check(!win::valid_changed_ftn_credentials(bad.u8string(),unreadable,edited),"new damaged credential is refused");
    edited=unreadable;edited.links.front().credentials.session=*win::protect_ftn_secret(bad.u8string(),link.network,link.code,win::FtnCredentialKind::session,secret);
    check(win::valid_changed_ftn_credentials(bad.u8string(),unreadable,edited)&&win::valid_ftn_protected_credentials(bad.u8string(),edited),"new portable save corrects just the affected credential");
    edited=unreadable;edited.links.front().code="OTHER";check(!win::valid_changed_ftn_credentials(bad.u8string(),unreadable,edited),"unchanged bytes cannot be transferred to another connection purpose");
    auto changed=aliases_before;changed[changed.size()-3]=changed[changed.size()-3]=='A'?'B':'A';const auto corrupted=copy(L"corrupt-alias");write(corrupted/L"Config/portable-secrets.v1",changed);
    const auto damaged=win::prepare_portable_credentials(corrupted,owner,ftn,doors);
    check(!damaged.issues.empty()&&read(corrupted/L"Config/portable-secrets.v1")==changed,"damaged alias is reported and never overwritten using local DPAPI fallback");
    auto extended=doors;auto additional=door;additional.rlogin.protected_pepper=*win::protect_rlogin_secret("extra-pepper");extended.doors.push_back(additional);
    HANDLE pin=CreateFileW((source/L"Config/portable-secrets.v1").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);check(pin!=INVALID_HANDLE_VALUE,"owned publication-fault pin");
    const auto publication_failed=refused([&]{win::prepare_portable_credentials(source,owner,ftn,extended);});CloseHandle(pin);
    check(publication_failed&&read(source/L"Config/portable-secrets.v1")==aliases_before&&read(source/L"Config/board.key")==key_before,"failed atomic publication retains complete original aliases/key");
    win::prepare_portable_credentials(source,owner,ftn,extended);check(win::read_portable_credential_aliases(source).size()==7,"publication retry adds new alias without replacing retained history");
    check(read(source/L"Config/board.key")==key_before&&app::encode_ftn_configuration(ftn)==ftn_bytes,"original board key/catalog unchanged after all faults");
    std::cout<<"PASS "<<checks<<" native portable migration checks; same Windows profile, copied FTN entropy path denied; installed Service/replacement context separate\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}
