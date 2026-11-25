// Small trusted synthetic source for writer-component tests ONLY. Does not
// qualify protected Runtime file ownership, immutable spool, ZIP or client UI.
struct ReplySource final:app::IQwkReplySource {
    std::vector<app::QwkReplyRecord> records;
    app::QwkReplySourceInfo metadata;
    unsigned rewinds{},fail_pass{},cancel_pass{},mutate_pass{};std::size_t position{};
    bool identity_valid{true};
    ReplySource(std::string board,std::vector<app::QwkReplyRecord> r):records(std::move(r)){
        metadata.board_id=std::move(board);metadata.records=records.size();metadata.compressed_bytes=512;metadata.expanded_bytes=4096*records.size();
        std::string canonical="SFX-QWK-REP-1;";
        const auto field=[&](std::string_view v){canonical+=std::to_string(v.size())+':'+std::string(v)+';';};field(metadata.board_id);
        for(const auto& record:records){const auto& m=record.message;field(std::to_string(m.conference));field(std::to_string(m.reference));field(m.private_mail?"private":"public");
            for(const auto* v:{&m.from,&m.to,&m.subject,&m.body,&m.date,&m.time,&m.message_id,&m.in_reply_to,&m.when_written})field(*v);}
        check(crypto_hash_sha256(metadata.logical_sha256.data(),reinterpret_cast<const unsigned char*>(canonical.data()),canonical.size())==0,"synthetic canonical REP identity");
    }
    app::QwkReplySourceInfo info()const override{return metadata;}
    app::StorageStatus verify_identity()const override{return identity_valid?app::StorageStatus::success():app::StorageStatus{app::StorageErrorCode::io,"injected sealed identity failure"};}
    app::StorageStatus rewind()override{++rewinds;position=0;return app::StorageStatus::success();}
    bool cancelled()const noexcept override{return cancel_pass&&rewinds==cancel_pass&&position>=1;}
    app::StorageResult<std::optional<app::QwkReplyRecord>> next()override{
        if(fail_pass&&rewinds==fail_pass&&position==1)return {{app::StorageErrorCode::io,"injected bounded source read failure"},{}};
        if(position==records.size())return {app::StorageStatus::success(),std::optional<app::QwkReplyRecord>{}};
        auto record=records[position++];if(mutate_pass&&rewinds==mutate_pass&&position==1)record.message.body+="changed";
        return {app::StorageStatus::success(),std::optional<app::QwkReplyRecord>{std::move(record)}};
    }
};
app::QwkReplyRecord reply_record(std::uint16_t conference,std::string to="All Callers",std::string subject="REP component",std::string body="Body\n"){
    app::QwkReplyRecord r;r.message.conference=conference;r.message.private_mail=conference==0;r.message.from="Bob";r.message.to=std::move(to);r.message.subject=std::move(subject);r.message.body=std::move(body);r.message.date="10-13-26";r.message.time="14:00";return r;
}
