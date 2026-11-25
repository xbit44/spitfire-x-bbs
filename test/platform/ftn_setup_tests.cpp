// Actual normal Setup coordinator/worker and shipped payload. Only the three
// named disposable Section14 fixtures are accepted. No System32 component work.
#include "setup_engine.hpp"
#include "sfx/platform/win32/file_payload_store.hpp"
#include <shellapi.h>
#include <objbase.h>
#include <iostream>
#include <fstream>
#include <map>
#include <algorithm>
#include <array>
namespace ins=sfx::platform::win32::installation;
namespace setup=sfx::apps::setup;
namespace arc=sfx::archive;
namespace {
#ifdef SFX_BETA1_SETUP_QUALIFICATION
#include "sysop_docs_payload.inc"
#endif
unsigned checks{};
void check(bool yes,const char* reason){++checks;ins::require(yes,reason);std::cout<<"PASS "<<reason<<std::endl;}
bool fixture(const ins::Path& root){
#ifdef SFX_NETWORK_SETUP_QUALIFICATION
#ifdef SFX_BETA4_SETUP_QUALIFICATION
return root==network_setup_qualification_root()||root==ins::Path(L"C:\\Program Files\\SFX-Beta4-Upgrade-Test-20261113-r1");
#elif defined(SFX_BETA3_SETUP_QUALIFICATION)
return root==network_setup_qualification_root()||root==ins::Path(L"C:\\Program Files\\SFX-Beta2-Test-20261112-r1");
#else
return root==network_setup_qualification_root();
#endif
#else
return root==ins::Path(L"C:\\Program Files\\SFX-OwnerFixes-Test-20261031-r3")||root==ins::Path(L"C:\\Program Files\\SFX-OwnerFixes-Test-20261031-r2")||root==ins::Path(L"C:\\Program Files\\SFX-OwnerFixes-Test-20261031-r1")||root==ins::Path(L"C:\\Program Files\\SFX-Section14-Test-20261029-r1")||root==ins::Path(L"C:\\Program Files\\SFX-Section14-Test-20261029-r2")||root==ins::Path(L"C:\\Program Files\\SFX-Section14-Test-20261030-r4");
#endif
}
std::map<std::string,std::string> durable(const ins::Path& root){
    std::map<std::string,std::string> result;
    for(const auto* role:{L"Data",L"Config",L"Display",L"Node",L"FTN"})if(std::filesystem::exists(root/role))for(const auto& entry:std::filesystem::recursive_directory_iterator(root/role)){
        const auto attr=GetFileAttributesW(entry.path().c_str());check(attr!=INVALID_FILE_ATTRIBUTES&&!(attr&FILE_ATTRIBUTE_REPARSE_POINT),"owned durable fixture has no redirects");
#ifdef SFX_BETA1_SETUP_QUALIFICATION
        // This one internal stock hash receipt changes with the shipping Docs.
        // Validate it and every actual stock asset separately below.
        if(entry.path().lexically_relative(root).generic_u8string()=="Config/stock-docs.v1")continue;
#endif
        // SQLite's read-only backup query may rewrite the transient WAL index.
        // Keep the DB and WAL bytes in the comparison; exclude only shared-memory
        // indexing and the process-lifetime lock, neither is durable BBS content.
        if(entry.is_regular_file()&&entry.path().filename()!=L"SFX.DB-shm"&&entry.path().filename()!=L"SFXRuntime.lock")result.emplace(entry.path().lexically_relative(root).generic_u8string(),ins::file_sha256(entry.path()));
    }
    if(std::filesystem::exists(root/L"Bin"/L"sfxexec.ini"))result.emplace("Bin/sfxexec.ini",ins::file_sha256(root/L"Bin"/L"sfxexec.ini"));return result;
}
void save(const ins::Path& path,const std::string& bytes){check(!std::filesystem::exists(path),"preserve prior evidence file");std::ofstream out(path,std::ios::binary);out<<bytes;check(bool(out),"write bounded evidence");}
void fault(const ins::Path& root,const std::wstring& transaction,bool layout=false){
    const auto exe=ins::module_path();const auto args=std::wstring(layout?L"--seed-layout \"":L"--seed-fault \"")+root.native()+L"\" "+transaction;
    SHELLEXECUTEINFOW launch{sizeof(launch)};launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;launch.lpVerb=L"runas";launch.lpFile=exe.c_str();launch.lpParameters=args.c_str();launch.nShow=SW_HIDE;
    check(ShellExecuteExW(&launch)!=FALSE&&launch.hProcess,"launch exact owned setup-fault worker");ins::Handle child(launch.hProcess);check(WaitForSingleObject(child.get(),30000)==WAIT_OBJECT_0,"bounded fault worker reaped");DWORD code{};check(GetExitCodeProcess(child.get(),&code)&&!code,"exact fault seed verified");
}
}
int wmain(int argc,wchar_t** argv)try{
    if(argc==4&&std::wstring_view(argv[1])==L"--worker")return setup::worker(argv[2],static_cast<DWORD>(std::stoul(argv[3])));
    SYSTEM_INFO machine{};GetNativeSystemInfo(&machine);check(machine.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64,"x64 VM only; no real System32 DOS component path");
#ifdef SFX_RC16_QUALIFICATION
    if(argc==4&&std::wstring_view(argv[1])==L"--seed-layout"){
        const auto root=ins::canonical_root(argv[2]);check(ins::elevated()&&fixture(root),"only RC16 legacy-layout seed");const auto receipt=ins::read_receipt(root,ins::operator_sid());ins::verify_structure(receipt);
        auto sd=ins::structural_descriptor(receipt.owner);check(!std::filesystem::exists(root/L"Recovery"),"legacy seed never overwrites an existing recovery tree");ins::create_directory(root/L"Recovery",sd.get());return 0;
    }
#endif
    if(argc==4&&std::wstring_view(argv[1])==L"--seed-fault"){
        const auto root=ins::canonical_root(argv[2]);const std::wstring transaction=argv[3];check(ins::elevated()&&fixture(root)&&transaction.size()==32&&transaction.find_first_not_of(L"0123456789ABCDEF")==std::wstring::npos,"exact disposable protected-write fault only");
        const auto receipt=ins::read_receipt(root,ins::operator_sid());ins::verify_structure(receipt);check(receipt.committed&&!receipt.door_component.registered,"verified own fixture without shared component");auto sd=ins::structural_descriptor(receipt.owner);ins::create_file(root/L"Bin"/(L"SFXRuntime.exe.new-"+transaction),{'f','a','u','l','t'},sd.get());return 0;
    }
    check(argc==4&&!ins::elevated(),"ordinary bounded action/root/new-evidence invocation");const std::wstring action=argv[1];const auto root=ins::canonical_root(argv[2]);check(fixture(root),"only exact disposable Section14 installation");const auto evidence=std::filesystem::absolute(argv[3]);check(!std::filesystem::exists(evidence),"new evidence directory");std::filesystem::create_directories(evidence);
    if(action==L"--seed-mail") {
        const auto receipt=ins::read_receipt(root,ins::operator_sid());ins::verify_runtime_installation(root,receipt.owner);ins::verify_ftn_directory(receipt);auto sd=ins::mutable_descriptor(receipt.owner);
        for(const auto* role:{L"Inbound",L"Outbound",L"Bad",L"Processed"}){
            const auto parent=root/L"FTN"/role;if(!std::filesystem::exists(parent))ins::create_directory(parent,sd.get());
            const auto path=parent/L"QA12";check(!std::filesystem::exists(path),"fresh synthetic spool child only");ins::create_directory(path,sd.get());
            const auto directory=sfx::platform::win32::FilePayloadDirectory::private_store(path,receipt.owner);auto file=directory.create("preserve.dat",64);const std::string content="QA12 synthetic durable mail and receipt bytes";file.append(reinterpret_cast<const std::uint8_t*>(content.data()),content.size());check(file.identity().bytes==content.size(),"native synthetic durable mail published");
        }
        save(evidence/L"scope.txt","Synthetic QA12 preservation files only; no real packet, job, session or delivered-mail claim.\n");return 0;
    }
    const bool fresh=action==L"--fresh",cancel=action==L"--cancel",rollback=action==L"--rollback";
#ifdef SFX_RC16_QUALIFICATION
    const bool layout=action==L"--layout-update";
#else
    const bool layout=false;
#endif
    check(fresh||cancel||rollback||layout||action==L"--update","closed Setup action");check(!fresh||!std::filesystem::exists(root),"fresh target cannot adopt existing directory");
    setup::verify_payload();auto requested_root=root;
#ifdef SFX_BETA4_SETUP_QUALIFICATION
    if(!fresh){auto alternate=root.native();for(auto& c:alternate)c=static_cast<wchar_t>(towlower(c));alternate[0]=static_cast<wchar_t>(towupper(alternate[0]));requested_root=alternate;}
#endif
    const auto plan=setup::inspect_plan(requested_root,false);check(plan.operation==(fresh?setup::Operation::fresh:setup::Operation::maintenance),"normal Setup reviewed plan");
#ifdef SFX_BETA4_SETUP_QUALIFICATION
    if(!fresh){check(plan.previous&&requested_root!=plan.previous->root,"alternate-case request differs from authenticated receipt spelling");check(plan.root==plan.previous->root,"normal Setup preserves exact authenticated receipt root spelling");check(plan.reviewed_root_id==plan.previous->root_id&&plan.reviewed_volume==plan.previous->volume,"alternate-case request retains verified native root identity");}
#endif
    std::map<std::string,std::string> before;if(!fresh){check(plan.previous&&plan.previous->committed&&!plan.previous->door_component.registered,"verified previous fixture has no System32 registration");before=durable(root);const auto bytes=ins::encode_receipt(*plan.previous);save(evidence/L"receipt-before.txt",{bytes.begin(),bytes.end()});}
    std::string prior_inventory;for(const auto& entry:before)prior_inventory+=entry.first+" "+entry.second+"\n";save(evidence/L"durable-before.txt",prior_inventory);
    if(rollback)fault(root,plan.transaction);
    if(layout)fault(root,plan.transaction,true);
    check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"ordinary COM initialization");bool cancelled{},complete{};std::string failure;const auto started=GetTickCount64();
    try{complete=setup::execute(plan,{false,false,{},false},nullptr,[&](const std::wstring& message){std::wcout<<message<<std::endl;if(cancel&&message.find(L"Writing backup ZIP")!=std::wstring::npos)cancelled=true;},[&]{return !cancelled&&GetTickCount64()-started<300000;});}catch(const std::exception& error){failure=error.what();std::cout<<"RETAINED EXPECTED/FAILURE "<<failure<<std::endl;}CoUninitialize();
    const auto receipt=ins::read_receipt(root,ins::operator_sid());ins::verify_runtime_installation(root,receipt.owner);const auto bytes=ins::encode_receipt(receipt);save(evidence/L"receipt-after.txt",{bytes.begin(),bytes.end()});
    if(cancel||rollback){check(!complete&&!failure.empty(),"precommit failure never reports success");check(receipt.committed&&receipt.program_hashes==plan.previous->program_hashes&&receipt.ftn_layout==plan.previous->ftn_layout,"exact previous payload/capability restored after interruption");check(!cancel||cancelled,"actual backup cancellation triggered");check(!rollback||failure=="setup_update_failed_previous_generation_restored","actual protected write collision rolled back");check(plan.previous->ftn_layout||!std::filesystem::exists(root/L"FTN"),"empty new FTN role removed on rollback without adoption");}
    else{check(complete&&failure.empty()&&receipt.ftn_layout&&receipt.program_count==12,"normal Setup committed explicit9 with unchanged twelve programs");ins::verify_ftn_directory(receipt);check(true,"actual receipt-bound private FTN root verified unelevated");check(fresh||receipt.root_id==plan.previous->root_id,"update retains protected root identity");}
    if(layout){check(!std::filesystem::exists(root/L"Recovery")&&std::filesystem::is_directory(root/L"Backup/Recovery"),"normal Setup retires exact empty legacy root and uses Backup/Recovery");check(std::filesystem::is_empty(root/L"Backup/Recovery"),"verified committed transaction files removed from new recovery location");}
#ifdef SFX_BETA1_SETUP_QUALIFICATION
    if(!cancel&&!rollback){
        const auto doc_bytes=ins::read_file(root/L"Config"/L"stock-docs.v1",16384);const std::string tracking(doc_bytes.begin(),doc_bytes.end());
        check(tracking.rfind("SFX-DOCS-1\n",0)==0,"current internal stock Docs receipt format");
        for(const auto& asset:sysop_doc_assets){
            std::string name;for(const auto* c=asset.filename;*c;++c){check(*c>0&&*c<128,"stock documentation path is ASCII");name+=static_cast<char>(*c);}
            check(ins::file_sha256((root/asset.filename).make_preferred())==asset.sha256,"every actual installed stock guide/notice matches final payload");
            check(tracking.find(name+"|"+asset.sha256+"\n")!=std::string::npos,"updated internal metadata binds every installed stock guide/notice");
        }
    }
#endif
    if(!fresh){const auto after=durable(root);for(const auto& entry:before){const auto found=after.find(entry.first);if(found==after.end()||found->second!=entry.second)std::cout<<"CHANGED "<<entry.first<<std::endl;}auto comparable=after;const auto journal=comparable.find("Data/SFX.DB-wal");
        if(!before.count("Data/SFX.DB-wal")&&journal!=comparable.end()&&journal->second=="E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"){
            check(std::filesystem::file_size(root/L"Data/SFX.DB-wal")==0,"sole allowed new SQLite journal is empty");
            std::cout<<"OBSERVED new empty SQLite WAL; all preexisting durable bytes still compared exactly\n";comparable.erase(journal);
        }
        check(comparable==before,"every existing durable fixture byte preserved; only a new empty WAL allowed");}
    if(!fresh&&!cancel){const auto backup=arc::inspect_backup(plan.backup);check(backup.result.ok(),"actual normal Setup optional data archive verifies");if(plan.previous->ftn_layout)for(const auto& entry:before)if(entry.first.rfind("FTN/",0)==0)check(std::any_of(backup.files.begin(),backup.files.end(),[&](const auto& f){return f.path==entry.first;}),"actual Setup backup includes capable durable FTN file");}
    std::string inventory;for(const auto& entry:durable(root))inventory+=entry.first+" "+entry.second+"\n";save(evidence/L"durable-after.txt",inventory);
    std::cout<<"PASS "<<checks<<" actual ordinary Setup/worker receipt9/cancel/rollback/preservation checks; no Runtime/tosser/owner-platform acceptance\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}
