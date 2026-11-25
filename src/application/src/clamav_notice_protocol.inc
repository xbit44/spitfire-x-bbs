namespace {
bool valid_clamav_notice(const ManualClamAvNotice& value){
    if(std::all_of(value.operation.bytes.begin(),value.operation.bytes.end(),[](auto b){return b==0;})||
       value.phase>ManualClamAvPhase::cancelled||value.attempt_utc<0)return false;
    if(value.phase==ManualClamAvPhase::started)return value.attempt_utc==0&&value.diagnostic.empty();
    if(value.diagnostic.size()<7||value.diagnostic.size()>64)return false;
    // The supervised updater also returns shared scan-admission/file errors.
    // They may report FAILURE only; success still requires the one exact
    // verified diagnostic plus Runtime's independent durable-status check.
    const bool failure_namespace=value.phase==ManualClamAvPhase::failed&&
        (value.diagnostic.rfind("scan_",0)==0||value.diagnostic.rfind("file_",0)==0);
    if(value.diagnostic.rfind("clamav_",0)!=0&&!failure_namespace)return false;
    if(!std::all_of(value.diagnostic.begin(),value.diagnostic.end(),[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';}))return false;
    return value.phase!=ManualClamAvPhase::verified||(value.attempt_utc>0&&value.diagnostic=="clamav_definitions_verified");
}
}
std::vector<std::uint8_t> encode_manual_clamav_notice(const ManualClamAvNotice& value){
    if(!valid_clamav_notice(value))return {};
    Writer w;w.raw(value.operation.bytes.data(),value.operation.bytes.size());w.u8(static_cast<std::uint8_t>(value.phase));
    w.u64(static_cast<std::uint64_t>(value.attempt_utc));if(!w.text(value.diagnostic,64))return {};return w.take();
}
std::optional<ManualClamAvNotice> decode_manual_clamav_notice(const std::vector<std::uint8_t>& bytes){
    Reader r(bytes);ManualClamAvNotice value;std::uint8_t phase{};
    if(!r.raw(value.operation.bytes.data(),value.operation.bytes.size())||!r.u8(phase)||
       !nonnegative64(r,value.attempt_utc)||!r.text(value.diagnostic,64)||!r.complete())return {};
    value.phase=static_cast<ManualClamAvPhase>(phase);if(!valid_clamav_notice(value))return {};return value;
}
