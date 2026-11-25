#include "sfx/platform/win32/portable_installation.hpp"
#include <algorithm>
#include <string_view>
#include <sstream>
namespace sfx::platform::win32::installation {
namespace {constexpr std::string_view magic="SFX-PORTABLE-INSTALLATION-1\n";constexpr auto leaf=L"portable-installation.v1";}
Receipt read_portable_installation_input(const Path& root){
    require(canonical_root(root)==root,"portable_installation_root_unsupported");
    auto current=root.root_path();std::vector<Handle> pins;for(const auto& part:root.relative_path()){current/=part;pins.push_back(open_object(current,true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE));}
    pins.push_back(open_object(root/L"Bin",true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ));
    const auto bytes=read_file(root/L"Bin"/leaf,maximum_receipt_bytes+64);
    require(bytes.size()>magic.size()&&std::equal(magic.begin(),magic.end(),bytes.begin()),"portable_installation_input_invalid_preserved");
    auto receipt=decode_receipt({bytes.begin()+magic.size(),bytes.end()});
    require(receipt.committed&&receipt.program_count==current_program_count&&receipt.ftn_layout,"portable_installation_input_incomplete_run_matching_setup_repair");return receipt;
}
void write_portable_installation_input(const Receipt& receipt){
    require(elevated(),"portable_installation_input_write_requires_authorized_worker");
    auto bytes=encode_receipt(receipt);bytes.insert(bytes.begin(),magic.begin(),magic.end());
    const auto target=receipt.root/L"Bin"/leaf;const auto staging=receipt.root/L"Bin"/(L"portable-install-"+unique_id()+L".pending");auto descriptor=structural_descriptor(receipt.owner);
    create_file(staging,bytes,descriptor.get());
    require(MoveFileExW(staging.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"portable_installation_input_publication_incomplete_retry_setup");
    require(read_file(target,maximum_receipt_bytes+64)==bytes,"portable_installation_input_publication_unconfirmed");
}
Receipt verify_portable_entry(const Path& root,std::size_t role,DWORD pid){
    require(role==control_panel_program_index||role==configuration_program_index,"portable_entry_role_invalid");if(!pid)pid=GetCurrentProcessId();
    auto receipt=read_portable_installation_input(root);const auto expected=root/L"Bin"/programs[role];auto pin=open_object(expected,false,GENERIC_READ|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));require(process!=nullptr,"portable_entry_process_unavailable");std::vector<wchar_t> path(32768);DWORD length=static_cast<DWORD>(path.size());
    require(QueryFullProcessImageNameW(process.get(),0,path.data(),&length)!=FALSE&&length&&_wcsicmp(std::wstring(path.data(),length).c_str(),expected.c_str())==0,"portable_entry_current_image_mismatch");
    require(file_sha256(expected)==receipt.program_hashes[role],"portable_entry_current_image_changed_run_matching_setup_repair");return receipt;
}
std::optional<PreparedFirstStart> read_prepared_first_start(const Path& root,const std::wstring& owner){
    const auto path=root/L"Config"/L"first-start.required";
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();require(error==ERROR_FILE_NOT_FOUND,"first_start_marker_unavailable_preserved");return {};}
    const auto bytes=read_file(path,256);std::istringstream input(std::string(bytes.begin(),bytes.end()));std::string marker_magic,id,operation,extra;
    const auto hex=[](const std::string& value){return value.size()==32&&value.find_first_not_of("0123456789ABCDEF")==value.npos;};
    require(std::getline(input,marker_magic)&&marker_magic=="SFX-FIRST-START-1"&&std::getline(input,id)&&hex(id)&&std::getline(input,operation)&&hex(operation)&&!std::getline(input,extra),"first_start_marker_invalid_preserved");
    const auto receipt=read_receipt(root,owner);require(receipt.committed&&std::wstring(id.begin(),id.end())==receipt.id,"first_start_marker_host_mismatch_preserved");
    return PreparedFirstStart{{id.begin(),id.end()},{operation.begin(),operation.end()}};
}
void require_prepared_first_start(const Receipt& receipt,const std::wstring& operation){
    require(elevated()&&receipt.committed&&operation.size()==32&&operation.find_first_not_of(L"0123456789ABCDEF")==operation.npos,"first_start_fence_not_authorized");
    const auto ascii=[](std::wstring_view value){std::string result;for(const auto c:value){require(c>=32&&c<127,"first_start_text_invalid");result+=static_cast<char>(c);}return result;};
    const auto text=std::string("SFX-FIRST-START-1\n")+ascii(receipt.id)+'\n'+ascii(operation)+'\n';
    const std::vector<std::uint8_t> bytes(text.begin(),text.end());
    const auto target=receipt.root/L"Config"/L"first-start.required";
    if(GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES&&read_file(target,256)==bytes)return;
    auto descriptor=mutable_descriptor(receipt.owner);const auto staging=receipt.root/L"Config"/(L"first-start-"+unique_id()+L".pending");create_file(staging,bytes,descriptor.get());
    require(MoveFileExW(staging.c_str(),(receipt.root/L"Config"/L"first-start.required").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"first_start_fence_publication_failed_stay_stopped");
}
void acknowledge_prepared_first_start(const Path& root,const std::wstring& owner,const std::wstring& operation){
    const auto marker=read_prepared_first_start(root,owner);if(!marker)return;
    require(operation.empty()||operation==marker->operation,"first_start_acknowledgment_operation_changed");
    const auto source=root/L"Config"/L"first-start.required",target=root/L"Config"/(L"first-start-"+marker->host_id+L"-"+marker->operation+L".confirmed");
    require(MoveFileExW(source.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"first_start_acknowledgment_unconfirmed_marker_preserved");
    require(!read_prepared_first_start(root,owner),"first_start_acknowledgment_not_confirmed");
}
}
