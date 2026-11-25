// Small typed Config-only policy frames; no caller paths/bodies/plans/receipt
// hashes or offline administrative authority enter the ordinary caller channel.
std::vector<std::uint8_t> encode_qwk_policy_change(const QwkPolicyChange& c){
    if(!valid_qwk_policy_change(c))return {};
    Writer w;w.raw(c.operation.bytes.data(),16);w.u64(static_cast<std::uint64_t>(c.expected_revision));w.text(c.board_id,8);w.u32(c.maximum_messages);return w.take();
}
std::optional<QwkPolicyChange> decode_qwk_policy_change(const std::vector<std::uint8_t>& bytes){
    Reader r(bytes);QwkPolicyChange c;if(!r.raw(c.operation.bytes.data(),16)||!nonnegative64(r,c.expected_revision)||!r.text(c.board_id,8)||!r.u32(c.maximum_messages)||!r.complete()||!valid_qwk_policy_change(c))return {};return c;
}
std::vector<std::uint8_t> encode_qwk_policy_result(const LocalQwkPolicyResult& result){
    if(result.code>LocalAccountResultCode::system_sysop_protected)return {};
    Writer w;w.u8(static_cast<std::uint8_t>(result.code));if(result.code!=LocalAccountResultCode::ok)return w.take();const auto& p=result.policy;
    if(!valid_qwk_board_id(p.board_id)||canonical_qwk_board_id(p.board_id)!=p.board_id||!valid_qwk_message_count(p.maximum_messages)||p.revision<=0)return {};
    w.text(p.board_id,8);w.u32(p.maximum_messages);w.u64(static_cast<std::uint64_t>(p.revision));w.u8(p.word_wrap_default?(*p.word_wrap_default?2:1):0);w.u8(p.replayed?1:0);return w.take();
}
std::optional<LocalQwkPolicyResult> decode_qwk_policy_result(const std::vector<std::uint8_t>& bytes){
    Reader r(bytes);LocalQwkPolicyResult result;std::uint8_t code{},wrap{},replayed{};if(!r.u8(code)||code>static_cast<std::uint8_t>(LocalAccountResultCode::system_sysop_protected))return {};result.code=static_cast<LocalAccountResultCode>(code);
    if(result.code!=LocalAccountResultCode::ok)return r.complete()?std::optional<LocalQwkPolicyResult>{result}:std::nullopt;
    auto& p=result.policy;if(!r.text(p.board_id,8)||!r.u32(p.maximum_messages)||!nonnegative64(r,p.revision)||!r.u8(wrap)||wrap>2||!r.u8(replayed)||replayed>1||!r.complete()||p.revision<=0||!valid_qwk_board_id(p.board_id)||canonical_qwk_board_id(p.board_id)!=p.board_id||!valid_qwk_message_count(p.maximum_messages))return {};
    if(wrap)p.word_wrap_default=wrap==2;
    p.replayed=replayed!=0;return result;
}
