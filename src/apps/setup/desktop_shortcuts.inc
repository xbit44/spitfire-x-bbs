// Desktop captions are independent of the protected installation identity.
// Do not resolve links or overwrite a caption occupied by an unrelated link.
constexpr unsigned desktop_caption_limit=1000;
std::wstring desktop_caption(unsigned number){
    return number==1?L"Spitfire-X BBS.lnk":L"Spitfire-X BBS ("+std::to_wstring(number)+L").lnk";
}
std::wstring desktop_marker(const std::wstring& identity){return L"SpitfireX.BBS."+identity;}
std::vector<std::uint8_t> shortcut_bytes(HANDLE file){
    LARGE_INTEGER size{},zero{};require(GetFileSizeEx(file,&size)&&size.QuadPart>=0&&size.QuadPart<=1024*1024,"setup_shortcut_size_invalid");
    require(SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)!=FALSE,"setup_shortcut_read_failed");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));DWORD got{};
    require(bytes.empty()||(ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)&&got==bytes.size()),"setup_shortcut_read_failed");return bytes;
}
std::vector<Path> desktop_candidates(const Path& folder,const wchar_t* pattern=L"Spitfire-X BBS*.lnk"){
    std::vector<Path> paths;WIN32_FIND_DATAW data{};const auto find=FindFirstFileW((folder/pattern).c_str(),&data);
    if(find==INVALID_HANDLE_VALUE){require(GetLastError()==ERROR_FILE_NOT_FOUND,"setup_desktop_enumeration_failed");return paths;}
    struct Close{HANDLE find;~Close(){FindClose(find);}} close{find};
    do{require(paths.size()<4096,"setup_desktop_candidates_exhausted");paths.push_back(folder/data.cFileName);}while(FindNextFileW(find,&data));
    require(GetLastError()==ERROR_NO_MORE_FILES,"setup_desktop_enumeration_failed");return paths;
}
bool owned_desktop_shortcut(const Path& path,const Path& target,const std::wstring& identity,bool legacy=false,
    std::vector<std::uint8_t>* proof=nullptr,bool reusable=false){
    try{
        auto held=open_object(path,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(held.get(),&info)||info.nNumberOfLinks!=1)return false;
        IShellLinkW* link{};
        if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,reinterpret_cast<void**>(&link))))return false;
        struct Release{IShellLinkW* link;~Release(){link->Release();}} release{link};
        IPersistFile* file{};if(FAILED(link->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&file))))return false;
        const auto loaded=file->Load(path.c_str(),STGM_READ);file->Release();
        std::array<wchar_t,32768> actual{},directory{},description{},arguments{};
        if(FAILED(loaded)||FAILED(link->GetPath(actual.data(),static_cast<int>(actual.size()),nullptr,SLGP_RAWPATH))||
           _wcsicmp(actual.data(),target.c_str())||FAILED(link->GetWorkingDirectory(directory.data(),static_cast<int>(directory.size())))||
           ((!reusable||directory[0])&&_wcsicmp(directory.data(),target.parent_path().c_str()))||
           FAILED(link->GetArguments(arguments.data(),static_cast<int>(arguments.size())))||arguments[0])return false;
        if(legacy){
            if(FAILED(link->GetDescription(description.data(),static_cast<int>(description.size()))))return false;
            const std::wstring text=description.data(),prefix=L"Spitfire-X BBS v";
            const bool owned=text.size()>prefix.size()&&text.size()<=prefix.size()+64&&text.rfind(prefix,0)==0&&
                text.substr(prefix.size()).find_first_not_of(L"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-")==std::wstring::npos;
            if(owned&&proof)*proof=shortcut_bytes(held.get());return owned;
        }
        IPropertyStore* properties{};
        if(FAILED(link->QueryInterface(IID_IPropertyStore,reinterpret_cast<void**>(&properties))))return false;
        PROPVARIANT value{};const auto got=properties->GetValue(PKEY_AppUserModel_ID,&value);properties->Release();
        const bool marked=SUCCEEDED(got)&&value.vt==VT_LPWSTR&&value.pwszVal&&
            std::wstring_view(value.pwszVal).rfind(L"SpitfireX.BBS.",0)==0;
        const bool owned=marked?desktop_marker(identity)==value.pwszVal:reusable&&SUCCEEDED(got);
        PropVariantClear(&value);if(owned&&proof)*proof=shortcut_bytes(held.get());return owned;
    }catch(...){return false;}
}
bool legacy_shortcut_name(const Path& path,std::wstring_view caption,const Path& root){
    const auto name=path.filename().wstring();
    const auto prefix=std::wstring(caption)+L" ("+root.filename().wstring()+L"-";
    if(name.size()!=prefix.size()+13||_wcsnicmp(name.c_str(),prefix.c_str(),prefix.size())||
        _wcsicmp(name.c_str()+prefix.size()+8,L").lnk"))return false;
    return std::wstring_view(name).substr(prefix.size(),8).find_first_not_of(L"0123456789abcdefABCDEF")==std::wstring_view::npos;
}
void create_desktop_shortcut(const Path& path,const Path& target,const std::wstring& identity){
    require(absent(path),"setup_desktop_shortcut_collision");
    const Path temporary=path.parent_path()/(L"SFX-shortcut-"+unique_id()+L".tmp");
    // CREATE_NEW establishes ownership of the temporary file before ShellLink
    // writes it; publication never uses MOVEFILE_REPLACE_EXISTING.
    create_file(temporary,{},mutable_descriptor(operator_sid()).get());
    struct Cleanup{Path path;~Cleanup(){if(!path.empty())DeleteFileW(path.c_str());}} cleanup{temporary};
    IShellLinkW* link{};
    require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,reinterpret_cast<void**>(&link))),"setup_shortcut_create_failed");
    struct Release{IShellLinkW* link;~Release(){link->Release();}} release{link};
    require(SUCCEEDED(link->SetPath(target.c_str()))&&SUCCEEDED(link->SetWorkingDirectory(target.parent_path().c_str()))&&
        SUCCEEDED(link->SetDescription(SFX_PRODUCT_CONTROL_PANEL_DISPLAY_VERSION_WIDE)),"setup_shortcut_target_failed");
    IPropertyStore* properties{};
    require(SUCCEEDED(link->QueryInterface(IID_IPropertyStore,reinterpret_cast<void**>(&properties))),"setup_shortcut_identity_unavailable");
    const auto marker=desktop_marker(identity);PROPVARIANT value{};value.vt=VT_LPWSTR;value.pwszVal=const_cast<wchar_t*>(marker.c_str());
    const auto marked=properties->SetValue(PKEY_AppUserModel_ID,value);const auto committed=SUCCEEDED(marked)?properties->Commit():marked;properties->Release();
    require(SUCCEEDED(committed),"setup_shortcut_identity_failed");
    IPersistFile* file{};require(SUCCEEDED(link->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&file))),"setup_shortcut_persistence_failed");
    const auto saved=file->Save(temporary.c_str(),TRUE);file->Release();require(SUCCEEDED(saved),"setup_shortcut_save_failed");
    require(owned_desktop_shortcut(temporary,target,identity),"setup_shortcut_identity_verification_failed");
    require(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"setup_desktop_shortcut_collision_preserved");cleanup.path.clear();
    require(owned_desktop_shortcut(path,target,identity),"setup_shortcut_publication_verification_failed");
}
void retire_owned_desktop_shortcut(const Path& path,const Path& target,const std::wstring& identity,bool legacy=false){
    std::vector<std::uint8_t> proof;if(!owned_desktop_shortcut(path,target,identity,legacy,&proof))return;
    // COM has released its readers before the DELETE lease. Recheck the exact
    // proven bytes through that lease; no second COM load/share conflict.
    auto held=open_object(path,false,GENERIC_READ|READ_CONTROL|DELETE,FILE_SHARE_READ);
    if(shortcut_bytes(held.get())!=proof)return;
    FILE_DISPOSITION_INFO disposition{TRUE};
    require(SetFileInformationByHandle(held.get(),FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE,"setup_shortcut_removal_failed");
}
void desktop_shortcut_changes(const Path& folder,const Path& target,const std::wstring& legacy_name,const std::wstring& identity,bool uninstall,bool selected){
    const auto root=target.parent_path().parent_path();
    if(uninstall){
        for(const auto& path:desktop_candidates(folder)){
            if(path.filename()==legacy_name||legacy_shortcut_name(path,L"Spitfire-X BBS",root))
                retire_owned_desktop_shortcut(path,target,identity,true);
            else for(unsigned n=1;n<=desktop_caption_limit;++n)if(path.filename()==desktop_caption(n)){retire_owned_desktop_shortcut(path,target,identity);break;}
        }return;
    }
    if(!selected)return;
    const auto existing=desktop_candidates(folder,L"*.lnk");
    Path reuse;
    for(const auto& path:existing){
        if(path.filename()==legacy_name||legacy_shortcut_name(path,L"Spitfire-X BBS",root))continue;
        if(!owned_desktop_shortcut(path,target,identity,false,nullptr,true))continue;
        if(!owned_desktop_shortcut(path,target,identity)){reuse=path;break;}
        if(reuse.empty()||path.filename()==desktop_caption(1))reuse=path;
    }
    if(!reuse.empty()){
        for(const auto& path:existing){
            if(path==reuse)continue;
            if(path.filename()==legacy_name||legacy_shortcut_name(path,L"Spitfire-X BBS",root))
                retire_owned_desktop_shortcut(path,target,identity,true);
            else if(owned_desktop_shortcut(path,target,identity))retire_owned_desktop_shortcut(path,target,identity);
        }
        return;
    }
    unsigned chosen=1;
    const auto plain=folder/desktop_caption(1);
    if(!absent(plain)&&!owned_desktop_shortcut(plain,target,identity)){
        chosen=0;const auto candidates=desktop_candidates(folder);
        for(unsigned n=2;n<=desktop_caption_limit;++n){const auto path=folder/desktop_caption(n);
            if(std::find(candidates.begin(),candidates.end(),path)!=candidates.end()&&owned_desktop_shortcut(path,target,identity)){chosen=n;break;}
        }
        if(!chosen)for(unsigned n=2;n<=desktop_caption_limit;++n)if(absent(folder/desktop_caption(n))){chosen=n;break;}
        require(chosen!=0,"setup_desktop_shortcut_captions_exhausted");
    }
    const auto destination=folder/desktop_caption(chosen);
    if(absent(destination))create_desktop_shortcut(destination,target,identity);
    require(owned_desktop_shortcut(destination,target,identity),"setup_desktop_shortcut_collision_preserved");
    // Only retire previous owned captions after safe new-link publication.
    for(const auto& path:desktop_candidates(folder)){
        if(path.filename()==legacy_name||legacy_shortcut_name(path,L"Spitfire-X BBS",root))retire_owned_desktop_shortcut(path,target,identity,true);
        else for(unsigned n=1;n<=desktop_caption_limit;++n)if(n!=chosen&&path.filename()==desktop_caption(n)){retire_owned_desktop_shortcut(path,target,identity);break;}
    }
}
