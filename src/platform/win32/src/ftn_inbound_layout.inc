namespace {
FilePayloadDirectory inbound_child(const std::filesystem::path& path,const std::wstring& owner){
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){require(GetLastError()==ERROR_FILE_NOT_FOUND,"ftn_inbound_directory_unavailable");auto sd=installation::mutable_descriptor(owner);installation::create_directory(path,sd.get());}
    return FilePayloadDirectory::private_store(path,owner);
}
bool same_native(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b){return a.volume==b.volume&&a.file_id==b.file_id&&a.bytes==b.bytes&&a.sha256==b.sha256;}
}
std::string ftn_bad_original_name(const app::FtnIngressReceipt& receipt){
    require(app::valid_ftn_leaf_name(receipt.file.name)&&receipt.id.valid(),"ftn_bad_original_name_invalid");
    const auto dot=receipt.file.name.rfind('.');const auto extension=dot==std::string::npos?std::string{}:receipt.file.name.substr(dot);
    const auto stem=receipt.file.name.substr(0,dot);require(extension.size()<=16,"ftn_bad_original_extension_invalid");return stem.substr(0,128-33-extension.size())+"-"+filename(receipt.id).substr(0,32)+extension;
}
FilePayloadLease open_ftn_received(app::IFtnMailStorage& storage,const installation::Receipt& installation,const app::FtnIngressReceipt& receipt,const std::function<bool()>& running,bool mutable_file,std::string_view resolved_network){
    require(running&&running()&&receipt.id.valid()&&(receipt.network.empty()||app::valid_ftn_code(receipt.network)),"ftn_inbound_context_invalid");
    const auto saved=storage.ftn_ingress_checkpoint(host,receipt.id);require(saved&&saved.value&&saved.value->has_value(),"ftn_inbound_checkpoint_missing");
    const auto base=installation.root/L"FTN",code=std::filesystem::u8path(receipt.network);const auto artifact=filename(receipt.id);
    std::vector<std::pair<std::filesystem::path,std::string>> candidates;
    if(!receipt.network.empty())candidates.push_back({base/L"Inbound",receipt.file.name});
    if(!receipt.network.empty()){candidates.push_back({base/L"Bad",receipt.file.name});candidates.push_back({base/L"Bad",ftn_bad_original_name(receipt)});}
    candidates.push_back({receipt.network.empty()?base/L"Nonsecure":base/L"Inbound"/code,artifact});
    if(!receipt.network.empty()){
        candidates.push_back({installation.root/L"FTN"/L"Conflicts"/code,artifact});
        candidates.push_back({base/L"Processed"/code,receipt.file.name});candidates.push_back({base/L"Processed"/code,ftn_bad_original_name(receipt)});candidates.push_back({base/L"Processed"/code,artifact});candidates.push_back({base/L"Bad"/code,artifact});candidates.push_back({base/L"Bad"/code,ftn_bad_original_name(receipt)});
    }
    if(receipt.network.empty()&&!resolved_network.empty()){require(app::valid_ftn_code(resolved_network),"ftn_inbound_resolved_network_invalid");candidates.push_back({base/L"Processed"/std::filesystem::u8path(resolved_network),artifact});}
    for(const auto& candidate:candidates){
        const auto attributes=GetFileAttributesW(candidate.first.c_str());if(attributes==INVALID_FILE_ATTRIBUTES){require(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND,"ftn_inbound_directory_unavailable");continue;}
        auto directory=FilePayloadDirectory::private_store(candidate.first,installation.owner);if(directory.absent(candidate.second))continue;
        app::FileObjectIdentity object;{auto probe=directory.read(candidate.second,false);object=probe.object_identity();}const auto& expected=**saved.value;
        // A same-name file from another network is never this receipt's source.
        if(object.volume!=expected.volume||object.file_id!=expected.file_id)continue;
        auto file=directory.read(candidate.second,mutable_file);
        const auto actual=file.identity([&](auto,auto){return running();});require(same_native(actual,expected)&&actual.bytes==receipt.file.size&&actual.sha256==receipt.digest,"ftn_inbound_content_changed");return file;
    }
    throw std::runtime_error("ftn_inbound_file_missing_or_changed");
}
void migrate_ftn_inbound(app::IFtnMailStorage& storage,const installation::Receipt& installation,const std::function<bool()>& running){
    installation::verify_ftn_directory(installation);auto flat=FilePayloadDirectory::private_store(installation.root/L"FTN"/L"Inbound",installation.owner);
    const auto partial=inbound_child(installation.root/L"FTN"/L"Partial",installation.owner);
    const auto legacy=inbound_child(installation.root/L"FTN"/L"Conflicts",installation.owner);
    // Old folders are bounded native capabilities, never arbitrary recursive
    // moves/deletions. Unknown or changed objects are retained and stop migration.
    for(const auto& entry:std::filesystem::directory_iterator(flat.path())){
        require(running(),"ftn_inbound_migration_cancelled");const auto attributes=GetFileAttributesW(entry.path().c_str());require(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"ftn_inbound_redirect_refused");
        if(!(attributes&FILE_ATTRIBUTE_DIRECTORY))continue;
        const auto code=entry.path().filename().u8string();require(app::valid_ftn_code(code),"ftn_inbound_unknown_directory_retained");
        {
            auto old=FilePayloadDirectory::private_store(entry.path(),installation.owner);auto pending=inbound_child(partial.path()/entry.path().filename(),installation.owner);auto conflict=inbound_child(legacy.path()/entry.path().filename(),installation.owner);
            auto cursor=old.enumerate();bool finished=false;
            while(!finished){const auto page=cursor.next(100,running);finished=page.finished;for(const auto& name:page.names){
                require(name.size()==36&&name.substr(32)==".dat","ftn_inbound_unknown_file_retained");app::Identifier128 id;
                for(std::size_t i=0;i<16;++i){const auto digit=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};const auto hi=digit(name[i*2]),lo=digit(name[i*2+1]);require(hi>=0&&lo>=0,"ftn_inbound_unknown_file_retained");id.bytes[i]=static_cast<unsigned char>(hi*16+lo);}
                const auto receipt=storage.ftn_ingress_receipt(host,id);const auto saved=storage.ftn_ingress_checkpoint(host,id);require(receipt&&receipt.value&&receipt.value->network==code&&saved&&saved.value,"ftn_inbound_legacy_binding_missing");
                auto file=old.read(name,true);const auto actual=file.identity([&](auto,auto){return running();});
                if(!saved.value->has_value()){require(receipt.value->state==app::FtnIngressState::partial&&!receipt.value->received&&!actual.bytes&&pending.absent(name),"ftn_inbound_unbound_nonempty_retained");file.rename_to(pending,name);continue;}
                const auto& expected=**saved.value;
                // A crash can leave an uncommitted partial tail. Move that same
                // object without modifying it; normal resume verifies/truncates it.
                require(actual.volume==expected.volume&&actual.file_id==expected.file_id&&actual.bytes>=expected.bytes,"ftn_inbound_legacy_object_changed");
                if(receipt.value->state==app::FtnIngressState::partial){
                    crypto_hash_sha256_state prefix_hash{};crypto_hash_sha256_init(&prefix_hash);for(std::uint64_t at=0;at<expected.bytes;){require(running(),"ftn_inbound_migration_cancelled");const auto bytes=file.read(at,static_cast<std::size_t>(std::min<std::uint64_t>(65536,expected.bytes-at)));require(!bytes.empty(),"ftn_inbound_partial_read_failed");crypto_hash_sha256_update(&prefix_hash,bytes.data(),bytes.size());at+=bytes.size();}app::FtnDigest prefix_digest{};crypto_hash_sha256_final(&prefix_hash,prefix_digest.data());require(prefix_digest==expected.sha256,"ftn_inbound_partial_prefix_changed");
                    require(pending.absent(name),"ftn_inbound_partial_collision");file.rename_to(pending,name);continue;
                }
                if(receipt.value->state==app::FtnIngressState::held&&expected.bytes<receipt.value->file.size){require(conflict.absent(name),"ftn_inbound_legacy_collision");file.rename_to(conflict,name);continue;}
                require(same_native(actual,expected)&&actual.bytes==receipt.value->file.size&&actual.sha256==receipt.value->digest,"ftn_inbound_legacy_content_changed");
                if(flat.absent(receipt.value->file.name))file.rename_to(flat,receipt.value->file.name);
                else {require(conflict.absent(name),"ftn_inbound_legacy_collision");file.rename_to(conflict,name);require(storage.advance_ftn_ingress(host,id,receipt.value->received,receipt.value->received,receipt.value->digest,app::FtnIngressState::held,"inbound_filename_conflict_original_preserved").succeeded(),"ftn_inbound_conflict_checkpoint_failed");}
            }}
        }
        require(RemoveDirectoryW(entry.path().c_str())!=FALSE,"ftn_inbound_legacy_directory_retained");
    }
    // Reconcile a crash after conflict rename but before its HOLD transaction.
    // The deterministic ID destination and original checkpoint bind the intent.
    app::Identifier128 after;
    for(;;){
        require(running(),"ftn_inbound_migration_cancelled");const auto page=storage.pending_ftn_ingress_after(host,64,after);require(page&&page.value,"ftn_inbound_conflict_recovery_unavailable");
        for(const auto& receipt:*page.value){after=receipt.id;if(receipt.network.empty())continue;require(app::valid_ftn_code(receipt.network),"ftn_inbound_context_invalid");const auto path=legacy.path()/std::filesystem::u8path(receipt.network);const auto attributes=GetFileAttributesW(path.c_str());if(attributes==INVALID_FILE_ATTRIBUTES){require(GetLastError()==ERROR_FILE_NOT_FOUND,"ftn_inbound_directory_unavailable");continue;}auto directory=FilePayloadDirectory::private_store(path,installation.owner);if(directory.absent(filename(receipt.id)))continue;
            auto file=open_ftn_received(storage,installation,receipt,running);require(file.path().parent_path()==directory.path(),"ftn_inbound_conflict_source_ambiguous");require(storage.advance_ftn_ingress(host,receipt.id,receipt.received,receipt.received,receipt.digest,app::FtnIngressState::held,"inbound_filename_conflict_original_preserved").succeeded(),"ftn_inbound_conflict_checkpoint_failed");
        }
        if(page.value->size()<64)break;
    }
}
