#include "sfx/platform/win32/door_component.hpp"
#include <iostream>
#include <fstream>
#include <sddl.h>
namespace inst=sfx::platform::win32::installation;
namespace fs=std::filesystem;
unsigned checks{};void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
std::ofstream production_log;
template<class F>void refuses(F f,const char* why){bool stopped{};try{f();}catch(...){stopped=true;}check(stopped,why);}
int wmain(int argc,wchar_t** argv)try{
    check(argc==2||argc==3,"fresh disposable fixture root required");const bool production=argc==3&&std::wstring(argv[2])==L"--production-policy";
    check(argc==2||production,"known production-policy mode only");
    const fs::path root=fs::absolute(argv[1]);check(!fs::exists(root),"preserve earlier test roots");
    if(production){const auto log=root.parent_path()/L"production-policy.log";check(!fs::exists(log),"fresh production log");production_log.open(log);check(production_log.good(),"capture elevated fixture output");std::cout.rdbuf(production_log.rdbuf());std::cerr.rdbuf(production_log.rdbuf());std::cout<<std::unitbuf;std::cerr<<std::unitbuf;check(inst::elevated(),"production descriptor requires elevated disposable fixture worker");}
    const auto owner=inst::operator_sid();auto sd=production?inst::shared_component_descriptor():inst::mutable_descriptor(owner);inst::create_directory(root,sd.get());
    if(production){
        auto object=inst::open_object(root,true);check(inst::exact_security(object.get(),sd.get()),"production owner/protected DACL round trips on actual NTFS");
        PSECURITY_DESCRIPTOR raw{};check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",SDDL_REVISION_1,&raw,nullptr)!=FALSE,"old generic expected descriptor");inst::Descriptor previous(raw);
        check(!inst::exact_security(object.get(),previous.get()),"reproduces old records-security rejection against correct actual filesystem policy");
        GENERIC_MAPPING mapping{FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};DWORD rights=GENERIC_READ|GENERIC_EXECUTE;MapGenericMask(&rights,&mapping);check(rights==0x1200a9,"documented NTFS read/execute mapping");
        unsigned negative{};
        for(const auto text:{L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;BU)",L"O:BAG:BAD:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)"}){
            raw=nullptr;check(ConvertStringSecurityDescriptorToSecurityDescriptorW(text,SDDL_REVISION_1,&raw,nullptr)!=FALSE,"negative actual descriptor");inst::Descriptor invalid(raw);
            const auto path=root/(L"invalid-"+std::to_wstring(++negative));SECURITY_ATTRIBUTES sa{sizeof(sa),invalid.get(),FALSE};check(CreateDirectoryW(path.c_str(),&sa)!=FALSE,"create deliberately invalid owned ACL fixture");auto bad=inst::open_object(path,true);check(!inst::exact_security(bad.get(),sd.get()),"actual write grant or unprotected ACL remains rejected");
        }
        auto wrong_owner=inst::mutable_descriptor(owner);check(!inst::exact_security(object.get(),wrong_owner.get()),"wrong owner policy rejected");
    }
    const std::vector<std::uint8_t> image{'S','F','X','-','f','i','x','t','u','r','e'},old{'O','L','D'};
    const inst::DoorComponentPolicy policy{inst::sha256(image),{},[](const auto&){return true;}};
    auto receipt=[&](unsigned number){inst::Receipt r;r.id=std::wstring(31,L'A')+wchar_t(L'0'+number);r.root=root/(L"install"+std::to_wstring(number));r.owner=owner;r.generation=1;r.committed=true;r.volume=1;r.root_id=1+number;r.bin_id=100+number;r.program_count=inst::current_program_count;for(std::size_t n=0;n<r.program_count;++n){r.program_ids[n]=200+n;r.program_hashes[n]=std::string(64,'A');}return r;};
    auto location=[&](const wchar_t* name){const auto p=root/name;inst::create_directory(p,sd.get());inst::create_directory(p/L"System32-fixture",sd.get());return inst::DoorComponentLocation{p/L"System32-fixture"/L"SFXEXEC.DLL",p/L"records"};};
    auto fresh=location(L"fresh");auto first=receipt(1),second=receipt(2);
    // Retry the owner's exact state: a correctly protected, empty records
    // directory left by the earlier generic-mask verification failure.
    inst::create_directory(fresh.records,sd.get());
    {auto c=inst::DoorComponentChange::prepare(fresh,first,image,policy,sd.get());check(c->identity().introduced&&inst::file_sha256(fresh.image)==policy.incoming_sha256,"missing component introduced with exact bytes");
        refuses([&]{inst::DoorComponentChange::prepare(fresh,second,image,policy,sd.get());},"exclusive operation lock serializes consumers");c->rollback();check(!fs::exists(fresh.image),"rollback removes only owned introduced component");}
    {auto c=inst::DoorComponentChange::prepare(fresh,first,image,policy,sd.get());first.door_component=c->identity();
        for(const auto name:{L"operation.lock",L"component.journal",L"component.receipt"}){auto object=inst::open_object(fresh.records/name,false,READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);check(inst::exact_security(object.get(),sd.get()),"actual lock/journal/receipt retains exact policy");}c->commit();}
    const auto shared_id=first.door_component.file_id;
    {auto c=inst::DoorComponentChange::prepare(fresh,second,image,policy,sd.get());second.door_component=c->identity();check(second.door_component.file_id==shared_id,"second installation reuses exact component identity");c->commit();}
    {auto c=inst::DoorComponentChange::prepare(fresh,first,{},policy,sd.get(),true);c->commit();check(c->image_retained()&&fs::exists(fresh.image),"first uninstall retains component used by another consumer");}
    {auto c=inst::DoorComponentChange::prepare(fresh,second,{},policy,sd.get(),true);c->commit();check(!c->image_retained()&&!fs::exists(fresh.image),"last consumer retires matching introduced unused component");}
    auto existing=location(L"preexisting");inst::create_file(existing.image,image,sd.get());
    {auto c=inst::DoorComponentChange::prepare(existing,first,image,policy,sd.get());check(!c->identity().introduced,"exact preexisting image is never claimed as introduced");first.door_component=c->identity();c->commit();}
    {auto c=inst::DoorComponentChange::prepare(existing,first,{},policy,sd.get(),true);c->commit();check(c->image_retained()&&inst::file_sha256(existing.image)==policy.incoming_sha256,"uninstall preserves preexisting component");}
    auto unknown=location(L"unknown");inst::create_file(unknown.image,old,sd.get());
    refuses([&]{inst::DoorComponentChange::prepare(unknown,first,image,policy,sd.get());},"unknown System32 image refuses adoption/replacement");check(inst::read_file(unknown.image,100)==old,"unknown bytes unchanged");
    auto interrupted=location(L"interrupted");
    {auto c=inst::DoorComponentChange::prepare(interrupted,first,image,policy,sd.get());check(fs::exists(interrupted.records/L"component.journal"),"write-ahead recovery journal durable before surrounding Setup commit");}
    refuses([&]{inst::DoorComponentChange::prepare(interrupted,first,image,policy,sd.get());},"unfinished transaction cannot be silently overwritten");
    refuses([&]{inst::DoorComponentChange::recover(interrupted,sd.get(),[](const auto&,bool)->std::optional<bool>{return {};});},"unknown receipt commit state preserves recovery");
    inst::DoorComponentChange::recover(interrupted,sd.get(),[](const auto&,bool)->std::optional<bool>{return false;});check(!fs::exists(interrupted.image)&&!fs::exists(interrupted.records/L"component.journal"),"uncommitted interruption recovers exact before-state");
    {auto c=inst::DoorComponentChange::prepare(interrupted,first,image,policy,sd.get());first.door_component=c->identity();}
    inst::DoorComponentChange::recover(interrupted,sd.get(),[](const auto&,bool)->std::optional<bool>{return true;});check(inst::file_sha256(interrupted.image)==policy.incoming_sha256&&!fs::exists(interrupted.records/L"component.journal"),"committed interruption finishes without undoing installed generation");
    {auto live=inst::open_object(interrupted.image,false,GENERIC_READ,FILE_SHARE_READ);auto c=inst::DoorComponentChange::prepare(interrupted,first,{},policy,sd.get(),true);c->commit();check(c->image_retained()&&fs::exists(interrupted.image),"in-use last component retained with explicit outcome");}
    {auto c=inst::DoorComponentChange::prepare(interrupted,first,image,policy,sd.get());first.door_component=c->identity();c->commit();}
    {auto c=inst::DoorComponentChange::prepare(interrupted,first,{},policy,sd.get(),true);c->commit();check(!fs::exists(interrupted.image),"retained introduced image can be retired after use ends");}
    auto replacement=location(L"owned-predecessor");inst::DoorComponentPolicy old_policy{inst::sha256(old),{},[](const auto&){return true;}};
    {auto c=inst::DoorComponentChange::prepare(replacement,first,old,old_policy,sd.get());first.door_component=c->identity();c->commit();}
    const auto original=first.door_component;inst::DoorComponentPolicy upgrade=policy;upgrade.compatible_predecessors={inst::sha256(old)};
    {auto c=inst::DoorComponentChange::prepare(replacement,first,image,upgrade,sd.get());check(inst::file_sha256(replacement.image)==policy.incoming_sha256,"explicitly known owned stopped predecessor replaced");}
    inst::DoorComponentChange::recover(replacement,sd.get(),[](const auto&,bool)->std::optional<bool>{return false;});
    {auto f=inst::open_object(replacement.image,false);check(inst::file_id(f.get())==original.file_id&&inst::read_file(replacement.image,100)==old,"replacement rollback restores original file identity and bytes");}
    {auto c=inst::DoorComponentChange::prepare(replacement,second,old,old_policy,sd.get());c->commit();}
    auto live_policy=upgrade;live_policy.stopped_consumer=[](const auto&){return false;};
    refuses([&]{inst::DoorComponentChange::prepare(replacement,first,image,live_policy,sd.get());},"known predecessor update blocked by live consumer");check(inst::read_file(replacement.image,100)==old,"blocked shared update preserves predecessor");
    for(unsigned stage=0;stage<2;++stage){
        auto retired=location(stage?L"retirement-after-record":L"retirement-before-record");auto consumer=receipt(3);
        {auto c=inst::DoorComponentChange::prepare(retired,consumer,image,policy,sd.get());consumer.door_component=c->identity();c->commit();}
        std::vector<std::uint8_t> journal;
        {auto c=inst::DoorComponentChange::prepare(retired,consumer,{},policy,sd.get(),true);journal=inst::read_file(retired.records/L"component.journal",1024*1024);
            if(stage)c->commit();}
        if(stage)inst::create_file(retired.records/L"component.journal",journal,sd.get());
        else {check(DeleteFileW(retired.image.c_str())!=FALSE,"simulate interruption after test-owned image retirement");
            refuses([&]{inst::DoorComponentChange::recover(retired,sd.get(),[](const auto&,bool)->std::optional<bool>{return false;});},"missing retired image cannot silently recreate an old consumer receipt");}
        inst::DoorComponentChange::recover(retired,sd.get(),[](const auto&,bool)->std::optional<bool>{return true;});
        check(!fs::exists(retired.image)&&!fs::exists(retired.records/L"component.journal"),"last-consumer retirement resumes before/after empty state without phantom component");
        {auto c=inst::DoorComponentChange::prepare(retired,consumer,image,policy,sd.get());check(c->identity().introduced,"recovered retirement permits clean later introduction");c->rollback();}
    }
    std::cout<<"PASS "<<checks<<" actual isolated file/lock/journal/component checks; production_policy="<<production<<"; synthetic installation receipts; no real System32/registry writes\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}
