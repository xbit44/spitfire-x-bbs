// Engineering-only cleanup of exactly the three keys created in this recorded
// investigation. NOT shipped in the owner fixture. No arbitrary IDs or paths.
// Reuse the reviewed read/identity/token checks without changing the qualified
// owner-probe translation unit or executable. Its entry point is never called.
#define wmain unused_receipt_probe_entry
#include "native.cpp"
#undef wmain

namespace {
const std::array<std::wstring,3> known_ids{
    L"52873E3CFAEBD2F61593B0A0DDA67D67", // Recorded fresh creation stopped before any value write.
    L"2BC0F0A6FC88B52BD2DE1F4FAA07522E", // Recorded successful fresh receipt.
    L"998BCAFE570544199DDEE84834EC4C0C"  // Recorded successful retry after actual UAC No.
};
void inspect_known(HKEY parent,const Auth& auth,const std::wstring& who,Key (&retained)[3]) {
    for(std::size_t i=0;i<known_ids.size();++i) {
        need(RegOpenKeyExW(parent,name(known_ids[i]).c_str(),REG_OPTION_OPEN_LINK,
            KEY_READ|KEY_WOW64_64KEY,&retained[i].h)==ERROR_SUCCESS,"all exact recorded keys must exist");
        no_link(retained[i].h);
        auto got=security(retained[i].h,SE_REGISTRY_KEY);auto want=descriptor(who);
        need(exact(got.get(),want.get()),"recorded owner and complete protected ACL must match");
        if(i==0) {
            DWORD children=0,values=0;
            need(RegQueryInfoKeyW(retained[i].h,nullptr,nullptr,nullptr,&children,nullptr,nullptr,
                &values,nullptr,nullptr,nullptr,nullptr)==ERROR_SUCCESS&&children==0&&values==0,
                "only the recorded empty interrupted key; unexpected content refuses");
        } else verify(parent,known_ids[i],who,auth,false);
        std::wcout<<L"VERIFIED_EXACT_CLEANUP_TARGET=HKLM\\SOFTWARE\\"<<name(known_ids[i])<<L"\n";
    }
}
DWORD request_cleanup() {
    std::array<wchar_t,32768> executable{};
    auto n=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    need(n&&n<executable.size(),"cleanup self path");
    SHELLEXECUTEINFOW launch_info{};launch_info.cbSize=sizeof(launch_info);
    launch_info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
    launch_info.lpVerb=L"runas";launch_info.lpFile=executable.data();
    launch_info.lpParameters=L"--perform-known-cleanup";launch_info.nShow=SW_HIDE;
    if(!ShellExecuteExW(&launch_info)){auto e=GetLastError();need(e==ERROR_CANCELLED,"cleanup UAC failure");return ERROR_CANCELLED;}
    Handle process(launch_info.hProcess);need(process!=nullptr,"cleanup process handle");
    need(WaitForSingleObject(process.get(),60000)==WAIT_OBJECT_0,"cleanup child bounded wait; timeout stops without retry");
    DWORD result=0;need(GetExitCodeProcess(process.get(),&result)!=FALSE,"cleanup child result");
    std::cout<<"CLEANUP_CHILD_EXIT="<<result<<"\n";return result;
}
int cleanup_run(int argc,wchar_t** argv) {
    need(argc==2,"only --audit or --request-known-cleanup supported for ordinary launch");
    std::wstring mode=argv[1];const bool child=mode==L"--perform-known-cleanup";
    need(child||mode==L"--audit"||mode==L"--request-known-cleanup","unknown cleanup mode refuses");
    auto t=token();need(elevated(t.get())==child,"cleanup must start ordinary except explicit UAC child");
    auto o=ordinary(t.get());Auth auth(o.get());auto who=user(o.get());Key parent;open_parent(parent,auth);
    {
        Key retained[3];inspect_known(parent.h,auth,who,retained);
        if(child) {
            // All three complete identities/inventories were checked BEFORE
            // deleting any. No creation, ACL repair, recursion, or broad path.
            for(std::size_t i=0;i<known_ids.size();++i) {
                failure_exit=static_cast<unsigned>(80+i);
                need(RegDeleteKeyExW(parent.h,name(known_ids[i]).c_str(),KEY_WOW64_64KEY,0)==ERROR_SUCCESS,
                    "delete only the exact verified recorded leaf");
            }
        }
    } // Close retained keys before checking final disappearance.
    if(mode==L"--audit") {std::cout<<"CLEANUP_AUDIT_READ_ONLY=PASS; no prompt or mutation\n";return 0;}
    if(!child) need(request_cleanup()==0,"cleanup child must report success; no automatic retry");
    for(const auto& id:known_ids)need(absent(parent.h,id),"recorded key absent after exact cleanup");
    std::cout<<"EXACT_THREE_KEY_CLEANUP=PASS; no other object targeted\n";return 0;
}
}
int wmain(int argc,wchar_t**argv) {
    try {const auto r=cleanup_run(argc,argv);std::cout<<"ENGINEERING CLEANUP PASS checks="<<checks<<" failed=0 skipped=0\n";return r;}
    catch(const std::exception&e){std::cerr<<"ENGINEERING CLEANUP STOP phase="<<failure_exit<<" checks="<<checks<<" diagnostic="<<e.what()<<"\nPreserve remaining keys. No repair or automatic retry.\n";return static_cast<int>(failure_exit);}
}
