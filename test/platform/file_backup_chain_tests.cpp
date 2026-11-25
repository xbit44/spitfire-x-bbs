// Pure dependency decisions using verified-inventory DTOs. No native archive,
// installation, retention mutation or installed-system claim.
#include "sfx/platform/win32/backup_collections.hpp"
#include <iostream>
#include <algorithm>
namespace win=sfx::platform::win32;namespace arc=sfx::archive;
namespace {
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
std::vector<win::VerifiedBackupMember> fixture(){
    std::vector<win::VerifiedBackupMember> members(4);
    const std::string payload(64,'A');
    for(std::size_t i=0;i<members.size();++i){auto& m=members[i];m.path=std::filesystem::path(L"C:\\synthetic-not-opened")/(std::to_wstring(i)+L".zip");m.sequence=i+1;m.sha256=std::string(64,static_cast<char>('B'+i));}
    members[0].contents.areas.push_back({1,1,"General","Files/General"});
    members[0].contents.files.push_back({1,"original.txt",4,payload,{},{},"FileContents/Objects/"+payload});
    members[0].inspection.files.push_back({"FileContents/Objects/"+payload,4,payload});
    members[1].contents=members[0].contents;members[1].contents.files[0].archive="0.zip";members[1].contents.files[0].archive_sha256=members[0].sha256;
    members[2].contents=members[1].contents; // flattened baseline dependency
    return members;
}
bool refused(const std::vector<win::VerifiedBackupMember>& m,std::vector<std::size_t> selected={2}){try{win::verify_content_dependencies(m,selected);return false;}catch(...){return true;}}
}
int main()try{
    auto members=fixture();check(win::content_dependency_closure(members,{2})==std::vector<std::size_t>({0,2}),"retention keeps exact baseline, not redundant increment");
    check(win::content_dependency_closure(members,{1,2})==std::vector<std::size_t>({0,1,2}),"multiple retained restore points share dependency");
    check(win::content_dependency_closure(members,{3})==std::vector<std::size_t>({3}),"later all-Off point independent of former contents");
    check(win::content_dependency_closure(members,{2,2})==std::vector<std::size_t>({0,2}),"duplicate selected index cannot duplicate deletion decisions");
    check(!refused(members,{}),"all healthy points verify");check(refused(members,{4}),"out-of-range restore point refused");
    members=fixture();members[0].path=L"C:\\synthetic-not-opened\\missing.zip";check(refused(members),"missing dependency refused");
    members=fixture();members[0].sha256[0]='F';check(refused(members),"whole archive hash mismatch refused");
    members=fixture();members[0].sequence=3;check(refused(members),"same sequence cannot be a predecessor");
    members=fixture();members[0].sequence=99;check(refused(members),"future sequence cannot be a predecessor");
    members=fixture();members[0].inspection.files.clear();check(refused(members),"referenced payload must actually exist in verified manifest");
    members=fixture();++members[0].inspection.files[0].size;check(refused(members),"payload length mismatch refused");
    members=fixture();members[0].inspection.files[0].sha256[0]='F';check(refused(members),"payload hash mismatch refused");
    members=fixture();members[2].contents.files[0].archive="2.zip";members[2].contents.files[0].archive_sha256=members[2].sha256;check(refused(members),"self-reference cannot form cycle");
    members=fixture();members[0].contents.files[0].archive="2.zip";members[0].contents.files[0].archive_sha256=members[2].sha256;check(refused(members),"baseline cycle refused even when newer payload points to baseline");
    members=fixture();members[2].contents.files[0].archive="1.zip";members[2].contents.files[0].archive_sha256=members[1].sha256;check(refused(members),"intermediate index is not a substitute for an actual payload member");
    members[1].inspection.files=members[0].inspection.files;check(win::content_dependency_closure(members,{2})==std::vector<std::size_t>({0,1,2}),"dependency archive's own restore point dependencies also retained");
    members=fixture();members[2].contents.files[0].archive="missing.zip";check(!refused(members,{3})&&refused(members,{}),"independent Off point valid without claiming all older broken points are valid");
    members.clear();check(!refused(members,{}),"empty managed pool valid");
    std::cout<<"PASS file-content dependency model "<<checks<<" checks; no disk/retention/installed claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL file-content dependency model after "<<checks<<": "<<e.what()<<'\n';return 1;}
