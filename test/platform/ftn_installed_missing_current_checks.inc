// QA32. Closed last-row fault on the already held r4 only. No product recovery API.
// Every uncertain branch retains the preserved original and stops; no destructor undo.
struct FaultObject {
    DWORD volume{},attributes{};std::uint64_t id{},size{};
    std::string hash;std::vector<unsigned char> security;
    bool operator==(const FaultObject& other)const{return volume==other.volume&&attributes==other.attributes&&id==other.id&&size==other.size&&hash==other.hash&&security==other.security;}
};
FaultObject fault_object(const std::filesystem::path& path,bool directory){
    const auto handle=win32::installation::open_object(std::filesystem::path(path).make_preferred(),directory);
    BY_HANDLE_FILE_INFORMATION info{};check(GetFileInformationByHandle(handle.get(),&info)!=FALSE,"native fault object inspection");
    FaultObject result;result.volume=info.dwVolumeSerialNumber;result.id=(static_cast<std::uint64_t>(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
    result.attributes=info.dwFileAttributes;
    if(!directory){
        result.size=(static_cast<std::uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
        check(result.size<=128U*1024U*1024U,"bounded isolated manifest file read");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(result.size));DWORD read{};
        check(ReadFile(handle.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)&&read==bytes.size(),"hash through the exact inspected handle, sharing its retained rename lease");
        result.hash=win32::installation::sha256(bytes);
    }
    constexpr SECURITY_INFORMATION fields=OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION;
    DWORD size{};SetLastError(ERROR_SUCCESS);
    check(!GetKernelObjectSecurity(handle.get(),fields,nullptr,0,&size)&&GetLastError()==ERROR_INSUFFICIENT_BUFFER&&size>0&&size<65536,"bounded exact object security size");
    result.security.resize(size);check(GetKernelObjectSecurity(handle.get(),fields,result.security.data(),size,&size)!=FALSE,"read exact object owner and DACL without change");
    return result;
}
using FaultManifest=std::map<std::wstring,FaultObject>;
FaultManifest fault_manifest(){
    FaultManifest result;
    for(const auto* role:{L"Data",L"Config",L"FTN",L"Display",L"Doors",L"Logs",L"BotGate"}){
        const auto folder=root/role;result.emplace(role,fault_object(folder,true));
        for(const auto& entry:std::filesystem::recursive_directory_iterator(folder)){
            check(result.size()<20000,"bounded isolated fault manifest");
            // open_object refuses reparse points and multiple file links before recursion proceeds.
            result.emplace(entry.path().lexically_relative(root).generic_wstring(),fault_object(entry.path(),entry.is_directory()));
        }
    }
    return result;
}
void no_database_sidecars(){
    for(const auto* leaf:{L"SFX.DB-wal",L"SFX.DB-shm",L"SFX.DB-journal"})
        check(!std::filesystem::exists(root/L"Data"/leaf),"all normal storage connections must settle sidecars; never delete or split WAL state");
}
std::vector<std::wstring> work_names(){
    std::vector<std::wstring> names;for(const auto& entry:std::filesystem::directory_iterator(root/L"Work"))names.push_back(entry.path().filename().native());
    std::sort(names.begin(),names.end());return names;
}
void fault_rename(HANDLE handle,const std::filesystem::path& target){
    const auto original=root/L"Data"/L"SFX.DB",saved=root/L"Work"/L"FTN-Missing-Current"/L"SFX.DB";
    check(target==original||target==saved,"closed two-name rename capability inside exact r4");
    check(!std::filesystem::exists(target),"native rename never replaces a candidate");
    const auto text=std::filesystem::path(target).make_preferred().native();const auto bytes=text.size()*sizeof(wchar_t);
    check(bytes<32768,"bounded exact native target");
    std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO)+bytes);
    auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());rename->ReplaceIfExists=FALSE;
    rename->RootDirectory=nullptr;rename->FileNameLength=static_cast<DWORD>(bytes);
    std::memcpy(rename->FileName,text.data(),bytes);
    check(SetFileInformationByHandle(handle,FileRenameInfo,rename,static_cast<DWORD>(buffer.size()))!=FALSE,"same-object same-volume no-replace native rename; no fallback");
}
void save_fault_manifest(const std::filesystem::path& fixture,const FaultManifest& manifest,const std::vector<std::uint8_t>& receipt){
    std::ostringstream output;output<<"Exact pre-fault held installation. Hashes only; no account, message or credential contents.\nreceipt_sha256="<<win32::installation::sha256(receipt)<<'\n';
    for(const auto& entry:manifest){const auto& value=entry.second;
        output<<std::filesystem::path(entry.first).u8string()<<'\t'<<value.volume<<'\t'<<value.id<<'\t'<<value.attributes<<'\t'<<value.size<<'\t'<<value.hash<<'\t'<<win32::installation::sha256(value.security)<<'\n';}
    const auto text=output.str();auto security=win32::installation::mutable_descriptor(win32::current_process_user_sid_string());
    win32::installation::create_file(fixture/L"before.txt",{text.begin(),text.end()},security.get());
}
void missing_current_last(const std::filesystem::path& archive){
    const auto owner=win32::current_process_user_sid_string();win32::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner).succeeded(),"one continuous exclusive guard through last fault and settlement");
    win32::installation::verify_runtime_installation(root,owner);
    win32::WindowsLocalStoragePathPolicy paths(owner);db::SqliteStorageSettings settings;settings.data_directory=root/L"Data";
    db::SqliteStorage reader(settings,paths);
    const auto source=root/L"Data"/L"SFX.DB",fixture=root/L"Work"/L"FTN-Missing-Current",saved=fixture/L"SFX.DB";
    const auto before_history=reader.verified_ftn_serial_history(source);
    check(before_history&&before_history.value&&before_history.value->recovery_required&&!before_history.value->predecessor,"already held verified history before any fault");
    check(win32::WindowsStorageDaclPolicy(owner).assess(source,win32::StorageDaclObjectKind::file).status.succeeded(),"normal current database private owner/security policy");
    // A read-only WAL-mode connection can leave an empty WAL and SHM index.
    // Settle them using the supported writer's normal shutdown, never deletion.
    check(reader.start().succeeded(),"normal held storage opens to settle read-only sidecars");
    const auto settled_history=reader.ftn_serial_state({true,{}});
    check(settled_history&&settled_history.value&&settled_history.value->recovery_required&&settled_history.value->lineage==before_history.value->lineage&&settled_history.value->high_water==before_history.value->high_water,"normal connection settlement preserves exact held history");
    check(reader.shutdown().succeeded(),"normal storage shutdown settles empty SQLite sidecars");
    no_database_sidecars();
    const auto receipt=win32::installation::encode_receipt(win32::installation::read_receipt(root,owner));
    auto root_pin=win32::installation::open_object(root,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
    auto work_pin=win32::installation::open_object(root/L"Work",true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
    // Data must remain replaceable by the normal restore. The retained handle and manifest bind its identity.
    auto data_pin=win32::installation::open_object(root/L"Data",true);
    check(!std::filesystem::exists(fixture),"last fault uses a new private fixture, never reuses an earlier attempt");
    auto security=win32::installation::mutable_descriptor(owner);win32::installation::create_directory(fixture,security.get());
    auto fixture_pin=win32::installation::open_object(fixture,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
    const auto root_state=fault_object(root,true),work_state=fault_object(root/L"Work",true),fixture_state=fault_object(fixture,true);
    const auto before=fault_manifest();const auto original=before.at(L"Data/SFX.DB");
    check(original.volume==root_state.volume&&original.volume==work_state.volume&&original.volume==fixture_state.volume&&original.volume==before.at(L"Data").volume,"all fault parents and source are same verified volume");
    save_fault_manifest(fixture,before,receipt);const auto names=work_names();
    // This handle denies every other writer or delete opener, proving no outstanding SQLite writer can coexist.
    auto held=win32::installation::open_object(source,false,GENERIC_READ|READ_CONTROL|DELETE,FILE_SHARE_READ);
    check(fault_object(source,false)==original,"exact DB bytes identity and security before rename");no_database_sidecars();
    fault_rename(held.get(),saved);
    std::cout<<"MISSING_CURRENT original_retained="<<saved.u8string()<<" sha256="<<original.hash<<'\n'<<std::flush;
    check(!std::filesystem::exists(source)&&fault_object(saved,false)==original,"exact old held object retained and actual current DB absent");
    auto absent=before;check(absent.erase(L"Data/SFX.DB")==1,"only intended DB omitted from absent manifest");
    check(fault_manifest()==absent,"live roles unchanged except intentional DB absence");
    bool publication_called=false;Sleep(1100);
    const auto result=restore_guarded(archive,false,true,guard,publication_called);
    std::cout<<"MISSING_CURRENT result="<<static_cast<int>(result.result.error)<<" detail="<<result.result.detail<<" publication_called="<<publication_called<<" live_changed="<<result.live_state_changed<<" rollback_attempted="<<result.rollback_attempted<<" rollback_succeeded="<<result.rollback_succeeded<<'\n'<<std::flush;
    check(guard.acquired()&&fault_object(saved,false)==original,"original object remains exact under uninterrupted guard");
    check(fault_object(root,true)==root_state&&fault_object(root/L"Work",true)==work_state&&fault_object(fixture,true)==fixture_state,"native pinned parents and security unchanged");
    check(win32::installation::encode_receipt(win32::installation::read_receipt(root,owner))==receipt,"exact installation receipt unchanged");
    if(result.result.ok()){
        check(publication_called&&result.live_state_changed&&!result.rollback_attempted,"successful actual unknown-current publication");
        const auto current=reader.verified_ftn_serial_history(source);
        check(current&&current.value&&current.value->recovery_required&&!current.value->predecessor,"successful unknown-current restore remains persistently held");
        std::cout<<"MISSING_CURRENT SUCCESSFUL_UNKNOWN_HOLD; original held DB retained in Work; no return rename\n";
        return;
    }
    check(!publication_called&&!result.live_state_changed&&!result.rollback_attempted&&result.retained_previous_state.empty()&&result.retained_failed_state.empty(),"only pre-publication refusal can permit controlled return; uncertainty stops");
    no_database_sidecars();check(!std::filesystem::exists(source)&&fault_manifest()==absent&&work_names()==names,"no candidate, changed live object or restore staging before controlled return");
    fault_rename(held.get(),source);
    check(!std::filesystem::exists(saved)&&fault_object(source,false)==original&&fault_manifest()==before,"same held object returned with exact full live manifest; no overwrite or hold release");
    held.reset(); // Release DELETE lease before normal SQLite read-only open; guard remains held.
    const auto returned=reader.verified_ftn_serial_history(source);
    check(returned&&returned.value&&returned.value->recovery_required&&returned.value->lineage==before_history.value->lineage&&returned.value->high_water==before_history.value->high_water,"normal guarded read-only history verifies exact still-held returned state");
    std::cout<<"MISSING_CURRENT PREPUBLICATION_REFUSAL_SETTLED; successful unknown-history publication NOT QUALIFIED; original divergent hold preserved\n";
}
