// TEST ONLY. No SFX installation is opened or certified by this probe.
// Writes are limited to one fresh HKLM\SOFTWARE\SFX-Receipt-Probe-<32 hex> leaf.
// ProgramData and ancestors are inspected ONLY. No filesystem writes/ACL edits.
// No external pathname/SID/receipt payload is accepted; elevated bytes are fixed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <authz.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned checks=0;
unsigned failure_exit=3; // Elevated phase is returned even when its console is hidden.
void need(bool b,const char* s) { ++checks;if(!b)throw std::runtime_error(std::string(s)+" last_error="+std::to_string(GetLastError())); }
struct Close { void operator()(void* p)const {if(p&&p!=INVALID_HANDLE_VALUE)CloseHandle(p);} };
struct Free { void operator()(void* p)const {if(p)LocalFree(p);} };
using Handle=std::unique_ptr<void,Close>;
using Local=std::unique_ptr<void,Free>;
struct Key {HKEY h=nullptr;~Key(){if(h)RegCloseKey(h);}Key()=default;Key(const Key&)=delete;Key& operator=(const Key&)=delete;};
Handle token(){HANDLE h=nullptr;need(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&h)!=FALSE,"query own token");return Handle(h);}
bool elevated(HANDLE h){TOKEN_ELEVATION e{};DWORD n=0;need(GetTokenInformation(h,TokenElevation,&e,sizeof(e),&n)!=FALSE,"query elevation");return e.TokenIsElevated!=0;}
std::wstring sid(PSID s){LPWSTR p=nullptr;need(s&&ConvertSidToStringSidW(s,&p),"format SID");Local holder(p);return p;}
std::wstring user(HANDLE t){DWORD n=0;GetTokenInformation(t,TokenUser,nullptr,0,&n);need(n>0&&n<4096,"bounded token user");std::vector<BYTE>b(n);need(GetTokenInformation(t,TokenUser,b.data(),n,&n)!=FALSE,"token user");return sid(reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid);}
Handle ordinary(HANDLE t){HANDLE h=nullptr;if(!elevated(t)){need(DuplicateHandle(GetCurrentProcess(),t,GetCurrentProcess(),&h,0,FALSE,DUPLICATE_SAME_ACCESS),"ordinary token handle");return Handle(h);}TOKEN_LINKED_TOKEN v{};DWORD n=0;need(GetTokenInformation(t,TokenLinkedToken,&v,sizeof(v),&n)!=FALSE,"same-user split-token UAC required");Handle r(v.LinkedToken);need(!elevated(r.get())&&user(t)==user(r.get()),"no alternate credential identity");return r;}
struct Auth {
 AUTHZ_RESOURCE_MANAGER_HANDLE m=nullptr;AUTHZ_CLIENT_CONTEXT_HANDLE c=nullptr;
 explicit Auth(HANDLE t){need(AuthzInitializeResourceManager(AUTHZ_RM_FLAG_NO_AUDIT,nullptr,nullptr,nullptr,L"SFX receipt read-only access audit",&m)!=FALSE,"Authz manager");LUID id{};if(!AuthzInitializeContextFromToken(0,t,m,nullptr,id,nullptr,&c)){AuthzFreeResourceManager(m);m=nullptr;need(false,"Authz context");}}
 ~Auth(){if(c)AuthzFreeContext(c);if(m)AuthzFreeResourceManager(m);}
 bool allows(void* sd,DWORD mask)const{AUTHZ_ACCESS_REQUEST q{};q.DesiredAccess=mask;DWORD granted=0,error=0,sacl=0;AUTHZ_ACCESS_REPLY r{};r.ResultListLength=1;r.GrantedAccessMask=&granted;r.Error=&error;r.SaclEvaluationResults=&sacl;need(AuthzAccessCheck(0,c,&q,nullptr,sd,nullptr,0,&r,nullptr)!=FALSE,"read-only access check");return error==ERROR_SUCCESS&&(granted&mask)==mask;}
};
bool trusted(const std::wstring&s){return s==L"S-1-5-18"||s==L"S-1-5-32-544"||s==L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";}
Local security(HANDLE h,SE_OBJECT_TYPE type){
 constexpr auto flags=OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION;
 if(type==SE_REGISTRY_KEY){
  // Query the already opened, view-bound HKEY directly; never reopen by name.
  DWORD n=0;auto e=RegGetKeySecurity(static_cast<HKEY>(h),flags,nullptr,&n);
  need(e==ERROR_INSUFFICIENT_BUFFER&&n>0&&n<=65536,"bounded registry security size");
  Local p(LocalAlloc(LMEM_FIXED,n));need(p!=nullptr,"registry security buffer");DWORD got=n;
  e=RegGetKeySecurity(static_cast<HKEY>(h),flags,p.get(),&got);
  need(e==ERROR_SUCCESS&&got<=n&&IsValidSecurityDescriptor(p.get()),"direct handle registry security read");return p;
 }
 void* p=nullptr;auto e=GetSecurityInfo(h,type,flags,nullptr,nullptr,nullptr,nullptr,&p);
 if(e!=ERROR_SUCCESS)throw std::runtime_error("read file security status="+std::to_string(e));
 need(p!=nullptr,"file security buffer");return Local(p);
}
bool anchor(void* sd,const Auth& a,bool registry){PSID owner=nullptr;BOOL def=FALSE,present=FALSE;PACL acl=nullptr;need(GetSecurityDescriptorOwner(sd,&owner,&def)&&GetSecurityDescriptorDacl(sd,&present,&acl,&def)&&present&&acl,"anchor owner and DACL");bool ok=trusted(sid(owner));DWORD danger=WRITE_DAC|WRITE_OWNER|DELETE;
 if(registry)danger|=KEY_SET_VALUE|KEY_CREATE_SUB_KEY|KEY_CREATE_LINK;
 else danger|=FILE_DELETE_CHILD|FILE_WRITE_DATA|FILE_WRITE_ATTRIBUTES|FILE_WRITE_EA;
 for(unsigned bit=0;bit<32;++bit){const DWORD mask=DWORD(1)<<bit;if(danger&mask){bool grant=a.allows(sd,mask);std::cout<<"PARENT_ACCESS "<<(registry?"registry":"filesystem")<<" mask="<<mask<<" ordinary="<<(grant?"ALLOW":"DENY")<<"\n";ok=ok&&!grant;}}
 GENERIC_MAPPING mapping=registry?GENERIC_MAPPING{KEY_READ,KEY_WRITE,KEY_EXECUTE,KEY_ALL_ACCESS}:GENERIC_MAPPING{FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};
 for(DWORD i=0;i<acl->AceCount;++i){void* p=nullptr;need(GetAce(acl,i,&p)!=FALSE,"parent ACE");auto* h=static_cast<ACE_HEADER*>(p);if(h->AceFlags&INHERIT_ONLY_ACE)continue;if(h->AceType!=ACCESS_ALLOWED_ACE_TYPE&&h->AceType!=ACCESS_DENIED_ACE_TYPE){ok=false;continue;}if(h->AceType==ACCESS_ALLOWED_ACE_TYPE){auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(p);DWORD mask=ace->Mask;MapGenericMask(&mask,&mapping);auto principal=sid(&ace->SidStart);
   // CREATOR OWNER is an inheritance placeholder, not an ordinary identity.
   // Current owner was checked above. Fresh leaf has a protected explicit SD,
   // so this parent template cannot confer creator rights on that leaf.
   const bool creator_template=principal==L"S-1-3-0"&&(h->AceFlags&CONTAINER_INHERIT_ACE);
   if((mask&danger)&&!trusted(principal)&&!creator_template)ok=false;}}
 return ok;
}
void no_link(HKEY h){DWORD type=0,n=0;need(RegQueryValueExW(h,L"SymbolicLinkValue",nullptr,&type,nullptr,&n)==ERROR_FILE_NOT_FOUND,"registry link/ambiguous reserved value refuses");}
void open_parent(Key& k,const Auth&a){need(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE",REG_OPTION_OPEN_LINK,KEY_READ|KEY_WOW64_64KEY,&k.h)==ERROR_SUCCESS,"open physical HKLM SOFTWARE explicit 64 view");no_link(k.h);auto sd=security(k.h,SE_REGISTRY_KEY);need(anchor(sd.get(),a,true),"registry parent must deny ordinary authorship/replacement");}
bool valid_id(const std::wstring&s){return s.size()==32&&std::all_of(s.begin(),s.end(),[](wchar_t c){return(c>=L'0'&&c<=L'9')||(c>=L'A'&&c<=L'F');});}
std::wstring name(const std::wstring&id){need(valid_id(id),"only 32 uppercase hex ID accepted, never a path");return L"SFX-Receipt-Probe-"+id;}
std::string narrow(const std::wstring&s){need(valid_id(s),"only validated ASCII ID is narrowed");std::string r;for(auto c:s)r+=static_cast<char>(c);return r;}
std::wstring random_id(){std::array<BYTE,16>b{};need(BCryptGenRandom(nullptr,b.data(),static_cast<ULONG>(b.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"local random probe ID");std::wstring s;for(auto v:b){s+=L"0123456789ABCDEF"[v>>4];s+=L"0123456789ABCDEF"[v&15];}return s;}
Local descriptor(const std::wstring& who,bool file=false){const auto rights=file?L"FR":L"KR";const auto full=file?L"FA":L"KA";auto s=std::wstring(L"O:BAG:BAD:P(A;;")+full+L";;;SY)(A;;"+full+L";;;BA)(A;;"+rights+L";;;"+who+L")";void*p=nullptr;need(ConvertStringSecurityDescriptorToSecurityDescriptorW(s.c_str(),SDDL_REVISION_1,&p,nullptr)!=FALSE,"fixed protected Administrators-owned descriptor");return Local(p);}
bool exact(void* got,void* want){PSID go=nullptr,wo=nullptr;BOOL d=FALSE,p=FALSE;PACL ga=nullptr,wa=nullptr;SECURITY_DESCRIPTOR_CONTROL ctl{};DWORD rev=0;if(!GetSecurityDescriptorOwner(got,&go,&d)||!GetSecurityDescriptorOwner(want,&wo,&d)||!go||!wo||!EqualSid(go,wo)||!GetSecurityDescriptorDacl(got,&p,&ga,&d)||!p||!ga||!GetSecurityDescriptorDacl(want,&p,&wa,&d)||!p||!wa||!GetSecurityDescriptorControl(got,&ctl,&rev)||!(ctl&SE_DACL_PROTECTED)||ga->AceCount!=wa->AceCount)return false;for(DWORD i=0;i<ga->AceCount;++i){void*a=nullptr,*b=nullptr;if(!GetAce(ga,i,&a)||!GetAce(wa,i,&b))return false;auto*h=static_cast<ACE_HEADER*>(a);if(h->AceSize!=static_cast<ACE_HEADER*>(b)->AceSize||memcmp(a,b,h->AceSize))return false;}return true;}
std::string payload(const std::wstring&id){return "SFX RECEIPT STORE PROBE 1\r\nNO INSTALLATION AUTHORITY\r\nID="+narrow(id)+"\r\nGENERATION=1\r\nDUMMY_BINDING=NO_REAL_TREE\r\n";}
bool absent(HKEY parent,const std::wstring&id){Key k;auto e=RegOpenKeyExW(parent,name(id).c_str(),REG_OPTION_OPEN_LINK,KEY_READ|KEY_WOW64_64KEY,&k.h);need(e==ERROR_SUCCESS||e==ERROR_FILE_NOT_FOUND,"unambiguous target existence query");return e==ERROR_FILE_NOT_FOUND;}
std::string read(HKEY h,const wchar_t* value){DWORD type=0,n=0;need(RegQueryValueExW(h,value,nullptr,&type,nullptr,&n)==ERROR_SUCCESS&&type==REG_BINARY&&n>0&&n<=4096,"bounded REG_BINARY size/type");std::string s(n,'\0');DWORD got=n;need(RegQueryValueExW(h,value,nullptr,&type,reinterpret_cast<BYTE*>(&s[0]),&got)==ERROR_SUCCESS&&type==REG_BINARY&&got==n,"exact bounded receipt read");return s;}
void put(HKEY h,const wchar_t*v,const std::string&s){need(s.size()<=4096&&RegSetValueExW(h,v,0,REG_BINARY,reinterpret_cast<const BYTE*>(s.data()),static_cast<DWORD>(s.size()))==ERROR_SUCCESS,"write fixed test value");}
void inventory(HKEY h){DWORD children=0,values=0;need(RegQueryInfoKeyW(h,nullptr,nullptr,nullptr,&children,nullptr,nullptr,&values,nullptr,nullptr,nullptr,nullptr)==ERROR_SUCCESS&&children==0&&values==2,"exact two-value leaf; unknown objects refuse cleanup");}
std::string evidence(const std::wstring&id){return "SFX RECEIPT NATIVE CREATE 1\r\nID="+narrow(id)+"\r\nFRESH_DISPOSITION=CREATED_NEW_KEY\r\nFINAL_OWNER=BUILTIN-Administrators\r\nFINAL_PROTECTED_DACL=EXACT\r\nTYPE=REG_BINARY\r\nSYNTHETIC_ONLY=1\r\n";}
void verify(HKEY parent,const std::wstring&id,const std::wstring& who,const Auth&a,bool actual_ordinary){Key k;need(RegOpenKeyExW(parent,name(id).c_str(),REG_OPTION_OPEN_LINK,KEY_READ|KEY_WOW64_64KEY,&k.h)==ERROR_SUCCESS,"independent receipt open");no_link(k.h);auto got=security(k.h,SE_REGISTRY_KEY);auto want=descriptor(who);need(exact(got.get(),want.get()),"exact Administrators owner and protected ordered ACL");inventory(k.h);need(read(k.h,L"Receipt")==payload(id),"exact synthetic receipt, not source-tree assertion");need(read(k.h,L"Evidence")==evidence(id),"fixed native completion evidence");need(a.allows(got.get(),KEY_READ),"ordinary read allowed");
 for(DWORD right:{DWORD(KEY_SET_VALUE),DWORD(KEY_CREATE_SUB_KEY),DWORD(KEY_CREATE_LINK),DWORD(DELETE),DWORD(WRITE_DAC),DWORD(WRITE_OWNER)}){need(!a.allows(got.get(),right),"ordinary receipt mutation denied by exact token ACL");if(actual_ordinary){Key denied;auto e=RegOpenKeyExW(parent,name(id).c_str(),REG_OPTION_OPEN_LINK,right|KEY_WOW64_64KEY,&denied.h);need(e==ERROR_ACCESS_DENIED,"actual ordinary mutation-capable open denied");}}
}
void program_data(const Auth&a,const std::wstring&who){PWSTR raw=nullptr;auto hr=SHGetKnownFolderPath(FOLDERID_ProgramData,KF_FLAG_DONT_VERIFY,nullptr,&raw);std::wstring path=raw?raw:L"";if(raw)CoTaskMemFree(raw);need(hr==S_OK&&path.size()>3&&path.size()<260&&path[1]==L':'&&path[2]==L'\\'&&path.find(L"..") == std::wstring::npos,"bounded local known ProgramData location");std::wcout<<L"SYSTEM_DATA_READ_ONLY "<<path<<L"\n";bool safe=true;
 // Walk components without following a final-component reparse point. If an
 // ancestor is unsafe, stop inspecting descendants; no use/creation is allowed.
 for(std::size_t end=3;;){auto part=path.substr(0,end);Handle h(CreateFileW(part.c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};need(h.get()!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(h.get(),&info),"read-only system-data ancestor");if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){safe=false;break;}auto sd=security(h.get(),SE_FILE_OBJECT);if(!anchor(sd.get(),a,false)){safe=false;break;}if(end==path.size())break;auto next=path.find(L'\\',end+1);end=next==std::wstring::npos?path.size():next;}
 auto proposed=descriptor(who,true);for(DWORD right:{DWORD(FILE_WRITE_DATA),DWORD(FILE_APPEND_DATA),DWORD(FILE_DELETE_CHILD),DWORD(DELETE),DWORD(WRITE_DAC),DWORD(WRITE_OWNER)})need(!a.allows(proposed.get(),right),"proposed new file descriptor denies ordinary mutation (model only)");
 std::cout<<"SYSTEM_DATA_CANDIDATE="<<(safe?"ANCESTOR_READ_AUDIT_ONLY_NOT_NATIVE_CREATED":"UNQUALIFIED_WRITABLE_OR_UNTRUSTED_ANCESTOR")<<"\nNo ProgramData object was created, modified, locked, or removed.\n";
}
DWORD launch(const std::wstring&mode,const std::wstring&id){std::array<wchar_t,32768>exe{};auto n=GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()));need(n&&n<exe.size(),"self executable path");auto args=mode+L" "+id;SHELLEXECUTEINFOW s{};s.cbSize=sizeof(s);s.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;s.lpVerb=L"runas";s.lpFile=exe.data();s.lpParameters=args.c_str();s.nShow=SW_HIDE;
 if(!ShellExecuteExW(&s)){auto e=GetLastError();need(e==ERROR_CANCELLED,"UAC launch error");return ERROR_CANCELLED;}
 Handle process(s.hProcess);need(process!=nullptr,"elevated child process handle");auto wait=WaitForSingleObject(process.get(),60000);need(wait==WAIT_OBJECT_0,"bounded child wait; on timeout STOP and preserve objects (do not auto-retry)");DWORD exit=0;need(GetExitCodeProcess(process.get(),&exit)!=FALSE,"child exit query");std::cout<<"ELEVATED_CHILD_EXIT="<<exit<<"\n";return exit;
}
int run(int argc,wchar_t**argv){need(argc==2||argc==3,"usage: --audit OR --create/--cancel/--verify/--cleanup HEX_ID");std::wstring mode=argv[1];bool child=mode==L"--elevated-create"||mode==L"--elevated-cleanup";auto t=token();need(elevated(t.get())==child,"launch ordinary; only explicit UAC child may elevate");DWORD virtualization=1,n=0;need(GetTokenInformation(t.get(),TokenVirtualizationEnabled,&virtualization,sizeof(virtualization),&n)&&virtualization==0,"no registry virtualization");auto o=ordinary(t.get());Auth auth(o.get());auto who=user(o.get());Key parent;open_parent(parent,auth);
 if(mode==L"--audit"){need(argc==2,"audit takes no arguments");program_data(auth,who);auto id=random_id();need(absent(parent.h,id),"unique absent target");std::wcout<<L"PROPOSED_ID="<<id<<L"\nPROPOSED_KEY=HKLM\\SOFTWARE\\"<<name(id)<<L" (64-bit view on x64)\nREAD_ONLY=1\n";return 0;}
 need(argc==3,"operation requires ID");std::wstring id=argv[2];name(id);std::wcout<<L"EXACT_KEY=HKLM\\SOFTWARE\\"<<name(id)<<L"\n";
 if(mode==L"--inspect"){
  Key k;need(RegOpenKeyExW(parent.h,name(id).c_str(),REG_OPTION_OPEN_LINK,KEY_READ|KEY_WOW64_64KEY,&k.h)==ERROR_SUCCESS,"inspect exact fixture key read-only");no_link(k.h);
  for(auto kind:{SE_REGISTRY_KEY,SE_REGISTRY_WOW64_64KEY}){void* p=nullptr;auto e=GetSecurityInfo(k.h,kind,OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,nullptr,nullptr,nullptr,nullptr,&p);Local release(p);std::cout<<"DIAGNOSTIC_GetSecurityInfo_type="<<kind<<" status="<<e<<"\n";}
  auto sd=security(k.h,SE_REGISTRY_KEY);auto want=descriptor(who);need(exact(sd.get(),want.get()),"direct handle exact owner/DACL verified");
  DWORD children=0,values=0;need(RegQueryInfoKeyW(k.h,nullptr,nullptr,nullptr,&children,nullptr,nullptr,&values,nullptr,nullptr,nullptr,nullptr)==ERROR_SUCCESS,"read-only fixture counts");std::cout<<"DIRECT_HANDLE_SECURITY=PASS children="<<children<<" values="<<values<<"\n";
 }else if(mode==L"--elevated-create"){
  failure_exit=60;
  need(absent(parent.h,id),"existing key refuses, never adopts or repairs");auto sd=descriptor(who);SECURITY_ATTRIBUTES sa{sizeof(sa),sd.get(),FALSE};Key k;DWORD disposition=0;
  auto result=RegCreateKeyExW(parent.h,name(id).c_str(),0,nullptr,REG_OPTION_NON_VOLATILE,KEY_ALL_ACCESS|KEY_WOW64_64KEY,&sa,&k.h,&disposition);
  need(result==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY,"fresh atomic creation disposition required before value writes");
  failure_exit=64;auto got=security(k.h,SE_REGISTRY_KEY);need(exact(got.get(),sd.get()),"final security at creation, never tighten an existing key");
  failure_exit=68;put(k.h,L"Receipt",payload(id));put(k.h,L"Evidence",evidence(id));
  failure_exit=72;need(RegFlushKey(k.h)==ERROR_SUCCESS,"one maintenance-only persistence flush");
  failure_exit=76;verify(parent.h,id,who,auth,false);
 }else if(mode==L"--elevated-cleanup"){
  verify(parent.h,id,who,auth,false); // Exact own ID, synthetic bytes, owner, ACL, 0 children/2 values.
  need(RegDeleteKeyExW(parent.h,name(id).c_str(),KEY_WOW64_64KEY,0)==ERROR_SUCCESS,"delete only verified exact test leaf; never recursive");need(absent(parent.h,id),"exact deleted leaf absent");
 }else if(mode==L"--verify"){verify(parent.h,id,who,auth,true);BOOL wow=FALSE;need(IsWow64Process(GetCurrentProcess(),&wow)!=FALSE,"registry view platform");if(wow){Key other;auto e=RegOpenKeyExW(HKEY_LOCAL_MACHINE,(L"SOFTWARE\\"+name(id)).c_str(),REG_OPTION_OPEN_LINK,KEY_READ|KEY_WOW64_32KEY,&other.h);need(e==ERROR_FILE_NOT_FOUND,"no duplicate in other registry view");}std::cout<<"INDEPENDENT_PROCESS_VERIFY=PASS\n";
 }else if(mode==L"--create"||mode==L"--cancel"){
  need(absent(parent.h,id),"exact target absent before UAC");auto result=launch(L"--elevated-create",id);
  if(mode==L"--cancel"){need(result==ERROR_CANCELLED,"cancellation control requires owner No");need(absent(parent.h,id),"UAC No left no receipt");std::cout<<"ACTUAL_UAC_NO=PASS\n";}else{need(result==0,"creation child success");verify(parent.h,id,who,auth,true);}
 }else if(mode==L"--cleanup"){verify(parent.h,id,who,auth,true);need(launch(L"--elevated-cleanup",id)==0,"exact cleanup child success");need(absent(parent.h,id),"ordinary post-cleanup absence");std::cout<<"EXACT_CLEANUP=PASS\n";
 }else need(false,"unknown mode refuses before writes");
 return 0;
}
}
int wmain(int argc,wchar_t**argv){try{auto r=run(argc,argv);std::cout<<"NATIVE RECEIPT PROBE PASS checks="<<checks<<" failed=0 skipped=0; synthetic store proof only\n";return r;}catch(const std::exception&e){std::cerr<<"NATIVE RECEIPT PROBE STOP checks="<<checks<<" phase="<<failure_exit<<" diagnostic="<<e.what()<<"\nNo automatic repair or cleanup. Preserve exact objects for review.\n";return static_cast<int>(failure_exit);}}
