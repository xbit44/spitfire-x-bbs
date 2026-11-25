// Legacy loose backup extraction artifacts only. No catalog/file payload is
// adopted and no existing destination is overwritten. Normal restore already
// excludes FileContents; actual library restoration streams directly to areas.
void rename_restore_object(HANDLE object,const Path& destination){
    auto path=destination;path.make_preferred();const auto absolute=L"\\\\?\\"+path.native();const auto bytes=absolute.size()*sizeof(wchar_t);
    std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO)+bytes,0);
    auto* info=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());info->ReplaceIfExists=FALSE;
    info->FileNameLength=static_cast<DWORD>(bytes);std::memcpy(info->FileName,absolute.c_str(),bytes);
    if(SetFileInformationByHandle(object,FileRenameInfo,info,static_cast<DWORD>(buffer.size()))==FALSE)throw std::runtime_error("setup_restore_artifacts_move_failed_"+std::to_string(GetLastError())+"_preserved");
}
std::vector<std::uint8_t> read_restore_object(HANDLE object,std::size_t maximum){
    LARGE_INTEGER size{},start{};require(GetFileSizeEx(object,&size)&&size.QuadPart>=0&&static_cast<unsigned long long>(size.QuadPart)<=maximum,"setup_restore_artifact_size_invalid");
    require(SetFilePointerEx(object,start,nullptr,FILE_BEGIN)!=FALSE,"setup_restore_artifact_seek_failed");
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size.QuadPart));DWORD read{};
    require(ReadFile(object,result.data(),static_cast<DWORD>(result.size()),&read,nullptr)&&read==result.size(),"setup_restore_artifact_read_failed");return result;
}
void retain_restore_artifact_set(const Plan& plan,const std::wstring& owner,const Path& set_root,bool already_retained,
    const std::function<void(std::size_t)>& after_access={}){
    namespace access=platform::win32;
    const auto source=set_root/L"FileContents";
    if(absent(source))return;
    // A recognizable INDEX plus matching original manifest/control file is
    // required; an arbitrary similarly named directory is left untouched.
    if(absent(source/L"INDEX.txt")||absent(set_root/L"SFX-BACKUP-INFO.txt")||absent(set_root/L"SFX-BACKUP-MANIFEST.txt"))return;
    auto source_pin=open_object(source,true,GENERIC_READ|READ_CONTROL|WRITE_DAC|DELETE,FILE_SHARE_READ);
    auto index_pin=open_object(source/L"INDEX.txt",false,GENERIC_READ|READ_CONTROL|WRITE_DAC,FILE_SHARE_READ|FILE_SHARE_DELETE);
    std::map<std::string,std::pair<DWORD,std::uint64_t>> identities;
    identities.emplace("INDEX.txt",std::make_pair(volume_serial(index_pin.get()),file_id(index_pin.get())));
    const auto index_bytes=read_file(source/L"INDEX.txt",32U*1024U*1024U);
    const auto index=archive_engine::decode_content_index(index_bytes);
    if(!index)return;
    if(!absent(plan.root/L"Data"/L"SFX.DB")){
        const auto config=platform::win32::read_backup_file_configuration(plan.root,owner);
        const auto overlaps=[](const Path& first,const Path& second){auto a=first.lexically_normal().native(),b=second.lexically_normal().native();if(a.empty()||b.empty())return true;if(a.back()!=L'\\')a+=L'\\';if(b.back()!=L'\\')b+=L'\\';return _wcsnicmp(a.c_str(),b.c_str(),std::min(a.size(),b.size()))==0;};
        for(const auto& area:config.areas){auto path=fs::u8path(area.storage_path);if(path.is_relative())path=plan.root/path;if(overlaps(source,path))return;}
    }
    auto manifest_pin=open_object(set_root/L"SFX-BACKUP-MANIFEST.txt",false,GENERIC_READ|READ_CONTROL|WRITE_DAC|DELETE,FILE_SHARE_READ);
    auto info_pin=open_object(set_root/L"SFX-BACKUP-INFO.txt",false,GENERIC_READ|READ_CONTROL|WRITE_DAC|DELETE,FILE_SHARE_READ);
    const auto manifest=read_restore_object(manifest_pin.get(),32U*1024U*1024U);
    const auto info_bytes=read_restore_object(info_pin.get(),1024U*1024U);
    std::vector<archive_engine::FileRecord> records;
    if(!archive_engine::decode_backup_manifest(std::string(manifest.begin(),manifest.end()),records))return;
    const auto matches=[&](const std::string& name,const Path& path){const auto found=std::find_if(records.begin(),records.end(),[&](const auto& row){return row.path==name;});return found!=records.end()&&fs::file_size(path)==found->size&&file_sha256(path)==found->sha256;};
    const auto info_record=std::find_if(records.begin(),records.end(),[](const auto& row){return row.path=="SFX-BACKUP-INFO.txt";});
    if(!matches("FileContents/INDEX.txt",source/L"INDEX.txt")||info_record==records.end()||info_record->size!=info_bytes.size()||info_record->sha256!=sha256(info_bytes))return;
    std::map<std::string,std::pair<std::uint64_t,std::string>> expected;
    for(const auto& file:index->files)if(file.archive.empty())expected.emplace(file.sha256,std::make_pair(file.bytes,file.sha256));
    std::vector<Handle> pinned;
    std::vector<std::pair<Path,HANDLE>> secured{{L"FileContents",source_pin.get()},
        {L"FileContents/INDEX.txt",index_pin.get()},{L"SFX-BACKUP-INFO.txt",info_pin.get()},
        {L"SFX-BACKUP-MANIFEST.txt",manifest_pin.get()}};
    for(const auto& entry:fs::recursive_directory_iterator(source)){
        const auto attributes=GetFileAttributesW(entry.path().c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return;
        const auto relative=entry.path().lexically_relative(source).generic_u8string();
        if(attributes&FILE_ATTRIBUTE_DIRECTORY){if(relative!="Objects")return;
            pinned.push_back(open_object(entry.path(),true,GENERIC_READ|READ_CONTROL|WRITE_DAC,FILE_SHARE_READ|FILE_SHARE_DELETE));
            secured.push_back({Path(L"FileContents/Objects"),pinned.back().get()});continue;}
        if(relative=="INDEX.txt")continue;
        if(relative.rfind("Objects/",0)!=0)return;const auto found=expected.find(relative.substr(8));if(found==expected.end())return;
        pinned.push_back(open_object(entry.path(),false,GENERIC_READ|READ_CONTROL|WRITE_DAC,FILE_SHARE_READ|FILE_SHARE_DELETE));
        secured.push_back({Path(L"FileContents")/entry.path().lexically_relative(source),pinned.back().get()});
        identities.emplace(relative,std::make_pair(volume_serial(pinned.back().get()),file_id(pinned.back().get())));
        if(fs::file_size(entry.path())!=found->second.first||file_sha256(entry.path())!=found->second.second||!matches("FileContents/"+relative,entry.path()))return;
        expected.erase(found);
    }
    if(!expected.empty())return;
    auto backup_pin=open_object(plan.root/L"Backup",true,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
    auto sd=mutable_descriptor(owner);const auto destination=plan.root/L"Backup"/L"RestoreFiles";
    if(absent(destination))create_directory(destination,sd.get());
    auto destination_pin=open_object(destination,true,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
    require(access::backup_boundary_with_readers(destination_pin.get(),sd.get(),owner),"setup_restore_artifacts_destination_not_private");
    const auto retained=already_retained?set_root:destination/(L"Restore-"+plan.transaction);
    if(!already_retained){create_directory(retained,sd.get());
        require(access::WindowsStorageDaclPolicy(owner).apply(retained,access::StorageDaclObjectKind::directory,true,true).succeeded(),"setup_retained_reader_inheritance_failed");}
    auto retained_pin=open_object(retained,true,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
    const std::array<HANDLE,3> handles{source_pin.get(),info_pin.get(),manifest_pin.get()};
    const std::array<Path,3> names{L"FileContents",L"SFX-BACKUP-INFO.txt",L"SFX-BACKUP-MANIFEST.txt"};
    struct SavedAccess{Path relative;DWORD volume;std::uint64_t id;bool directory;access::BackupDaclSnapshot before;};
    std::vector<SavedAccess> saved;bool needs_access{};
    for(const auto& object:secured){BY_HANDLE_FILE_INFORMATION identity{};
        require(GetFileInformationByHandle(object.second,&identity)&&!(identity.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
            ((identity.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||identity.nNumberOfLinks==1),"setup_restore_artifact_access_identity_invalid");
        const bool directory=(identity.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        saved.push_back({object.first,volume_serial(object.second),file_id(object.second),directory,access::BackupDaclSnapshot(object.second)});
        needs_access=(!access::backup_owner_has_read(object.second,owner,directory))||needs_access;
    }
    if(needs_access){
        // Durable, private before-images survive process loss. No original
        // owner, custom ACE, protection state or payload byte is discarded.
        std::string journal="SFX-RETAINED-ACL-BEFORE-1\nsource="+set_root.u8string()+"\ndestination="+retained.u8string()+"\n";
        for(const auto& row:saved){const auto text=row.before.sddl();std::string encoded;
            for(const auto character:text){require(character<=127,"setup_restore_acl_journal_encoding_invalid");encoded.push_back(static_cast<char>(character));}
            journal+=row.relative.generic_u8string()+"|"+std::to_string(row.volume)+"|"+std::to_string(row.id)+"|"+encoded+"\n";}
        const auto journal_path=retained/(L"SFX-RETAINED-ACL-BEFORE-"+unique_id()+L".txt");
        create_file(journal_path,{journal.begin(),journal.end()},sd.get());
        require(access::WindowsStorageDaclPolicy(owner).apply(journal_path,access::StorageDaclObjectKind::file,true,true).succeeded(),"setup_retained_acl_journal_readers_failed");
        std::size_t changed{};try{while(changed<secured.size()){const auto current=changed++;
            access::ensure_backup_owner_read(secured[current].second,owner,saved[current].directory,&saved[current].before);
            if(after_access)after_access(current);}
        }catch(...){for(std::size_t current=0;current<saved.size();++current)
            saved[current].before.restore(secured[current].second);throw;}
    }
    if(already_retained)return;
    // Windows refuses renaming a directory containing open files. Release the
    // leaf leases only at this boundary; retain the exact DELETE directory
    // lease, then recheck every original object/hash before moving controls.
    pinned.clear();index_pin.reset();
    std::size_t moved{};try{while(moved<handles.size()){
        const auto current=moved;rename_restore_object(handles[current],retained/names[current]);++moved;
        if(current==0){std::size_t observed{};const auto placed=retained/L"FileContents";
            for(const auto& entry:fs::recursive_directory_iterator(placed)){
                const auto attributes=GetFileAttributesW(entry.path().c_str());require(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"setup_restore_artifact_changed_after_move");
                const auto relative=entry.path().lexically_relative(placed).generic_u8string();if(attributes&FILE_ATTRIBUTE_DIRECTORY){require(relative=="Objects","setup_restore_artifact_membership_changed");continue;}const auto expected_object=identities.find(relative);
                require(expected_object!=identities.end(),"setup_restore_artifact_membership_changed");auto held=open_object(entry.path(),false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
                require(volume_serial(held.get())==expected_object->second.first&&file_id(held.get())==expected_object->second.second&&matches("FileContents/"+relative,entry.path()),"setup_restore_artifact_identity_changed_after_move");++observed;
            }
            require(observed==identities.size(),"setup_restore_artifact_membership_changed");
        }
    }}
    catch(...){while(moved){--moved;rename_restore_object(handles[moved],set_root/names[moved]);}
        if(needs_access)for(const auto& row:saved){HANDLE survivor{};
            for(std::size_t n=0;n<names.size();++n)if(row.relative==names[n])survivor=handles[n];
            if(survivor){require(volume_serial(survivor)==row.volume&&file_id(survivor)==row.id,"setup_restore_acl_rollback_identity_changed_journal_preserved");row.before.restore(survivor);}
            else{auto path=set_root/row.relative;path.make_preferred();
                auto original=open_object(path,row.directory,READ_CONTROL|WRITE_DAC,FILE_SHARE_READ|FILE_SHARE_DELETE);
                require(volume_serial(original.get())==row.volume&&file_id(original.get())==row.id,"setup_restore_acl_rollback_identity_changed_journal_preserved");row.before.restore(original.get());}}
        throw;}
}
void relocate_restore_artifacts(const Plan& plan,const std::wstring& owner){
    // Upgrade existing retained sets as well as the legacy loose-root case.
    // Only recognized, fully verified sets are candidates; never adopt a
    // similarly named directory or follow a reparse point into another tree.
    const auto destination=plan.root/L"Backup"/L"RestoreFiles";
    if(!absent(destination)){
        auto parent=open_object(destination,true,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        const auto expected=mutable_descriptor(owner);
        require(platform::win32::backup_boundary_with_readers(parent.get(),expected.get(),owner),"setup_restore_artifacts_destination_not_private");
        std::vector<Path> retained;
        for(const auto& entry:fs::directory_iterator(destination)){
            const auto name=entry.path().filename().native();
            if(name.size()!=40||name.compare(0,8,L"Restore-")!=0||
                name.find_first_not_of(L"0123456789ABCDEFabcdef",8)!=std::wstring::npos)continue;
            const auto attributes=GetFileAttributesW(entry.path().c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))continue;
            retained.push_back(entry.path());
        }
        for(const auto& path:retained){auto anchor=open_object(path,true,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
            retain_restore_artifact_set(plan,owner,path,true);}
    }
    retain_restore_artifact_set(plan,owner,plan.root,false);
}
