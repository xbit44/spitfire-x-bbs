    {
        std::string bytes(65536,'\0');std::size_t used{};
        auto* archive=archive_write_new();check(archive&&archive_write_set_format_zip(archive)==ARCHIVE_OK&&archive_write_open_memory(archive,bytes.data(),bytes.size(),&used)==ARCHIVE_OK,"ordinary independent ZIP payload fixture");
        auto* entry=archive_entry_new();archive_entry_set_pathname(entry,"readme.txt");archive_entry_set_filetype(entry,AE_IFREG);archive_entry_set_perm(entry,0600);archive_entry_set_size(entry,12);
        check(archive_write_header(archive,entry)==ARCHIVE_OK&&archive_write_data(archive,"hello files!",12)==12,"ordinary ZIP has a non-packet member");archive_entry_free(entry);check(archive_write_close(archive)==ARCHIVE_OK,"ordinary ZIP fixture closes");archive_write_free(archive);bytes.resize(used);
        for(const bool tic_first:{false,true}){
            const std::string name=tic_first?"tic-first.zip":"payload-first.zip";
            app::FtnTic control;control.area="TEST.FILES";control.file=name;control.origin=*app::parse_ftn_address("2:100/9@test");control.from=*app::parse_ftn_address(peer.address);control.to=*app::parse_ftn_address(net.address);control.size=bytes.size();control.crc=app::ftn_crc32(bytes);control.password="tic-key";control.descriptions={"Unchanged ordinary ZIP"};control.path.push_back({control.from,std::uint64_t{1700000000},{}});
            const auto text=app::encode_ftn_tic(control);check(text.has_value(),"independent TIC pairing fixture");Input pair;pair.bytes=bytes;
            if(tic_first){pair.tic=receive(*text,"order.tic");const auto waiting=publish(pair);check(waiting.deferred&&waiting.reason=="ftn_tic_payload_pending","TIC-first waits without claiming missing bytes");}
            else{pair.payload=receive(bytes,name);const auto waiting=native::toss_ftn_native_receipt(db,c,installed,pair.payload,live);check(waiting.deferred&&!waiting.held&&!waiting.processed,"payload-first real ZIP remains intact without mail decoding");}
            check(db.shutdown().succeeded()&&db.start().succeeded(),"actual restart between separately arriving TIC and payload");
            if(tic_first)pair.payload=receive(bytes,name);else pair.tic=receive(*text,"order.tic");
            const auto result=publish(pair);if(!result.processed)std::cerr<<result.reason<<'\n';check(result.processed&&general.read(name).identity().sha256==tic_hash(bytes),"both arrival orders publish original ZIP bytes through TIC after restart");
            check(publish(pair).processed,"exact completed TIC replay reconciles without double publication");
            check(db.claim_ftn_message(owner,pair.payload.id,c.revision).value==std::optional<bool>{false},"completed ordinary file receipt cannot acquire message ownership");
        }
        auto conflict=input("reserved.mo0",'X',false);const auto held=publish(conflict);
        check(held.deferred&&held.reason=="ftn_tic_message_name_ambiguous"&&general.absent("reserved.mo0"),"actual admitted TIC referring to reserved bundle name cannot publish a file");
        check(native::toss_ftn_native_receipt(db,c,installed,conflict.payload,live).deferred,"same exact native custody also cannot enter message decoder");
    }
