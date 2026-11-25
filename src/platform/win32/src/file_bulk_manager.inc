NativeFileOperationOutcome WindowsFileManager::scan_files(bool pending,const Progress& progress,std::vector<std::string>* added_pages){
    const auto config=storage_.file_configuration(actor_);
    if(!config||!config.value)return {false,false,false,{},"File Areas are unavailable. Nothing scanned."};
    const auto started=GetTickCount64();std::uint64_t added{},removed{},skipped{},failed{},locations{},visited{};
    std::string issues;std::uint64_t omitted{};bool stopped{},report_full{};
    std::vector<std::string> local_pages;auto& pages=added_pages?*added_pages:local_pages;pages.clear();
    const auto issue=[&](const std::string& where,const std::string& why){
        auto line=app::sanitize_file_text(where,256,false)+": "+app::sanitize_file_text(why,512,false)+"\n";
        if(issues.size()+line.size()<14000)issues+=line;else ++omitted;
    };
    const auto active=[&]{return GetTickCount64()-started<15*60*1000&&visited<200000&&(!progress||progress(0,0));};
    const auto fresh=[](){app::Identifier128 id;do{ins::require(BCryptGenRandom(nullptr,id.bytes.data(),static_cast<ULONG>(id.bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"file_scan_identity_unavailable");}while(!id.valid());return id;};
    for(const auto& configured:config.value->areas){
        if(!active()){stopped=true;break;}
        std::string label;for(const auto& group:config.value->groups)if(group.id==configured.area.group)label=group.name+" / ";label+=configured.area.name;
        try{
            auto directory=area(*config.value,configured.area.id);
            // Open/list the configured location before any absence observation.
            // A missing drive, directory, denied listing or unsafe link is an
            // area error, never evidence that its individual files were deleted.
            auto native=directory.enumerate();(void)native.next(1,active);
            if(pending){
                auto scan=find_unregistered(configured.area.id);bool finished{};
                while(!finished){
                    if(!active()){stopped=true;break;}
                    auto page=scan.next(100,active);finished=page.finished;
                    for(const auto& name:page.names){
                        if(!active()){stopped=true;break;}++visited;
                        const auto added_line="Added: "+app::sanitize_file_text(name,app::kFileNameBytes,false)+" ["+app::sanitize_file_text(label,512,false)+"]\n";
                        // Reserve reporting capacity BEFORE any import. Every
                        // confirmed addition remains visible, even on cancel.
                        const auto max_pages=added_pages?512u:1u;
                        if(pages.empty())pages.emplace_back();
                        if(pages.back().size()+added_line.size()>14000){if(pages.size()==max_pages){stopped=report_full=true;break;}pages.emplace_back();}
                        NativeFileImportChoice choice;choice.operation=fresh();choice.area=configured.area.id;
                        choice.source=directory.path()/std::filesystem::u8path(name);choice.mode=app::FileImportMode::in_place;
                        // Explicit owner bulk registration accepts identical
                        // content under different names. Native name/identity
                        // reservations still forbid replacement or overwrite.
                        choice.allow_duplicate_content=true;
                        auto preview=preview_import(choice,progress);
                        if(!preview){++failed;issue(label+" / "+name,"Could not safely read the file. Not added.");continue;}
                        if(preview->registration.descriptions.long_text.empty()&&preview->registration.descriptions.short_text==name)
                            preview->registration.descriptions.short_text="No Description";
                        const auto outcome=import_file(*preview,progress);
                        if(outcome.completed){++added;pages.back()+=added_line;}else{++failed;issue(label+" / "+name,outcome.detail);}
                    }
                    if(stopped)break;
                }
            }else{
                app::FileManagementQuery query;query.area=configured.area.id;query.state=app::FileCatalogState::available;app::FileId after;
                for(;;){
                    if(!active()){stopped=true;break;}
                    const auto page=inspect_files(query,after,100,progress);
                    for(const auto& entry:page.entries){
                        if(!active()){stopped=true;break;}++visited;
                        const auto& record=entry.record;if(record.purged||record.file.state==app::FileCatalogState::removed)continue;
                        if(entry.presence!=FilePayloadPresence::missing){
                            ++skipped;
                            if(entry.presence==FilePayloadPresence::unavailable||entry.presence==FilePayloadPresence::changed||entry.presence==FilePayloadPresence::pending)
                                issue(label+" / "+record.file.name,"Skipped: file unavailable, changed or in use. Listing kept.");
                            continue;
                        }
                        auto current=record;
                        if(current.file.state!=app::FileCatalogState::missing){
                            app::FileOperationRequest mark;mark.operation=fresh();mark.file=current.file.id;mark.expected_revision=current.file.revision;mark.action=Action::mark_missing;
                            const auto observed=execute(mark,progress);
                            if(!observed.completed){++failed;issue(label+" / "+current.file.name,observed.detail);continue;}
                            const auto refreshed=storage_.managed_file(actor_,current.file.id);
                            if(!refreshed||!refreshed.value){++failed;issue(label+" / "+current.file.name,"Could not refresh confirmed-missing listing.");continue;}current=*refreshed.value;
                        }
                        // Purging a missing row requires ANOTHER absence proof.
                        // A reappearing file is never deleted by this scan.
                        app::FileOperationRequest erase;erase.operation=fresh();erase.file=current.file.id;erase.expected_revision=current.file.revision;erase.action=Action::purge;
                        const auto outcome=execute(erase,progress);
                        if(outcome.completed)++removed;else{++failed;issue(label+" / "+current.file.name,outcome.detail);}
                    }
                    if(stopped)break;
                    if(!page.next.value){if(query.state==app::FileCatalogState::available){query.state=app::FileCatalogState::missing;after={};continue;}break;}
                    ins::require(page.next.value>after.value,"file_scan_cursor_changed");after=page.next;
                }
            }
        }catch(...){++locations;issue(label,"Location could not be safely accessed or changed during the scan. Unprocessed listings kept.");}
        if(!active())stopped=true;
        if(stopped)break;
    }
    std::string report=std::string(stopped?"Scan stopped. ":"Scan finished. ")+"Added: "+std::to_string(added)+"; missing listings removed: "+std::to_string(removed)+"; skipped: "+std::to_string(skipped)+"; failed: "+std::to_string(failed)+"; locations skipped: "+std::to_string(locations)+".\n";
    if(stopped)report+="Completed changes are saved. Run the scan again to process remaining files.\n";
    if(report_full)report+="The bounded results display is full. No additional files were imported without a report.\n";
    report+=issues;if(omitted)report+="Additional problems not displayed: "+std::to_string(omitted)+".\n";
    if(!added_pages&&!pages.empty()){if(report.size()+pages[0].size()<16384)report+=pages[0];else report+="Added filenames are available from the matched File Manager results.\n";}
    return {!stopped,stopped,true,{},std::move(report)};
}
