// All methods use the same identity and lock as public/private Chat. Operator
// windows hold a bounded, ephemeral view lease; no transcript is persisted.
ChatOperatorSnapshot ChatHub::operator_snapshot(const Operator& op) const {
    ChatOperatorSnapshot out;out.code=ChatResult::ok;out.available=operator_available_;
    out.token=op.token;out.name=op.name;out.pending=op.pending;out.active=op.active;
    out.paused=op.active&&op.pause;out.lines.assign(op.lines.begin(),op.lines.end());return out;
}
bool ChatHub::operator_line(Operator& op,std::string text){
    if(op.lines.size()>=64||op.next_sequence==UINT64_MAX)return false;
    op.lines.push_back({op.next_sequence++,std::move(text)});return true;
}
ChatOperatorSnapshot ChatHub::operator_status() const {
    std::lock_guard<std::mutex> lock(mutex_);ChatOperatorSnapshot out;
    out.code=ChatResult::ok;out.available=operator_available_;
    for(const auto& entry:sessions_)if(entry.second.page)
        out.pages.push_back({entry.second.page,entry.second.caller.identity,entry.second.caller.name});
    return out;
}
void ChatHub::operator_available(bool value){
    std::lock_guard<std::mutex> lock(mutex_);operator_available_=value;
    if(!value)for(auto& entry:sessions_)entry.second.page=entry.second.page_expires=0;
}
ChatRequestResult ChatHub::page(const ChatIdentity& id,std::uint64_t now){
    std::lock_guard<std::mutex> lock(mutex_);auto* s=find(id);
    if(!s||!operator_available_||next_request_==UINT64_MAX)return {};
    if(s->page||s->peer||s->operator_token)return {ChatResult::busy,0};
    for(const auto& entry:invites_)if(entry.second.from==id||entry.second.to==id)return {ChatResult::busy,0};
    s->page=next_request_++;s->page_expires=now+30000;
    return {ChatResult::ok,s->page};
}
bool ChatHub::page_pending(const ChatIdentity& id,std::uint64_t request) const {
    std::lock_guard<std::mutex> lock(mutex_);const auto* s=find(id);return s&&s->page==request;
}
void ChatHub::cancel_page(const ChatIdentity& id,std::uint64_t request){
    std::lock_guard<std::mutex> lock(mutex_);if(auto* s=find(id))if(s->page==request)s->page=s->page_expires=0;
}
ChatOperatorSnapshot ChatHub::operator_start(const ChatIdentity& id,Identifier128 view,
    std::uint64_t page_id,bool override_dnd,std::uint64_t now){
    std::lock_guard<std::mutex> lock(mutex_);
    const auto failure=[](ChatResult code){ChatOperatorSnapshot out;out.code=code;return out;};
    if(!view.valid())return failure(ChatResult::invalid);
    for(const auto& entry:operators_)if(entry.second.view==view){
        const auto& op=entry.second;return op.caller==id?operator_snapshot(op):failure(ChatResult::invalid);
    }
    auto* s=find(id);if(!s)return failure(ChatResult::unavailable);
    if(page_id&&(!operator_available_||s->page!=page_id||now>=s->page_expires))return failure(ChatResult::unavailable);
    if(s->caller.preferences.dnd&&!override_dnd)return failure(ChatResult::dnd);
    if(!s->caller.safe||s->peer||s->operator_token)return failure(ChatResult::busy);
    for(const auto& entry:invites_)if(entry.second.from==id||entry.second.to==id)return failure(ChatResult::busy);
    if(operators_.size()>=128||next_request_==UINT64_MAX)return failure(ChatResult::full);
    Operator op;op.token=next_request_++;op.view=view;op.caller=id;op.name=s->caller.name;
    op.pending=true;op.pause=!page_id;op.override_dnd=override_dnd;op.lease=now+10000;op.claim_by=now+2000;op.started=now;
    s->operator_token=op.token;s->page=s->page_expires=0;
    operators_.emplace(op.token,op);return operator_snapshot(op);
}
bool ChatHub::claim_operator(const ChatIdentity& id,std::uint64_t now){
    std::lock_guard<std::mutex> lock(mutex_);auto* s=find(id);if(!s||!s->operator_token)return false;
    auto it=operators_.find(s->operator_token);if(it==operators_.end()||!it->second.pending)return false;
    auto& op=it->second;
    if(now>=op.lease||now>=op.claim_by||!s->caller.safe||s->peer||(s->caller.preferences.dnd&&!op.override_dnd)){
        end_operator_locked(op,now,"Caller is busy or no longer available.");return false;
    }
    op.pending=false;op.active=true;op.started=now;s->suspended_room=s->caller.room;leave_locked(*s);
    operator_line(op,"Chat connected to "+s->caller.name+".");return true;
}
bool ChatHub::operator_active(const ChatIdentity& id) const {
    std::lock_guard<std::mutex> lock(mutex_);const auto* s=find(id);if(!s)return false;
    const auto it=operators_.find(s->operator_token);return it!=operators_.end()&&it->second.active;
}
std::optional<std::uint64_t> ChatHub::operator_paused_millis(const ChatIdentity& id,std::uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);const auto* s=find(id);if(!s)return {};
    auto paused=s->paused_millis;const auto it=operators_.find(s->operator_token);
    if(it!=operators_.end()&&it->second.active&&it->second.pause&&now>=it->second.started)
        paused+=std::min(now,it->second.lease)-it->second.started;
    return paused;
}
void ChatHub::end_operator_locked(Operator& op,std::uint64_t now,std::string_view message){
    if(!op.pending&&!op.active)return;
    if(auto* s=find(op.caller)){
        if(op.active&&op.pause&&now>=op.started)s->paused_millis+=std::min(now,op.lease)-op.started;
        s->operator_token=0;if(op.active)restore_room(*s);
        push(*s,{ChatEventKind::ended,{}, {},std::string(message),op.token});
    }
    op.pending=op.active=false;op.lease=now+60000;
    // A final control result must remain visible even if the text queue is full.
    if(op.lines.size()<65&&op.next_sequence!=UINT64_MAX)op.lines.push_back({op.next_sequence++,std::string(message)});
}
ChatOperatorSnapshot ChatHub::operator_poll(std::uint64_t token,Identifier128 view,
    std::uint64_t acknowledged,std::uint64_t now){
    std::lock_guard<std::mutex> lock(mutex_);const auto it=operators_.find(token);
    if(it==operators_.end()||it->second.view!=view)return {};
    auto& op=it->second;if(acknowledged>=op.next_sequence)return {};
    if(op.pending&&now>=op.claim_by)end_operator_locked(op,now,"Caller is busy; retry later.");
    if((op.active||op.pending)&&now>=op.lease)end_operator_locked(op,op.lease,"Operator connection lost. Chat ended.");
    while(!op.lines.empty()&&op.lines.front().sequence<=acknowledged)op.lines.pop_front();
    if(op.active||op.pending)op.lease=now+10000;
    return operator_snapshot(op);
}
ChatResult ChatHub::operator_send(std::uint64_t token,Identifier128 view,
    std::uint64_t sequence,std::string_view text,std::uint64_t now){
    std::lock_guard<std::mutex> lock(mutex_);const auto it=operators_.find(token);
    if(it==operators_.end()||it->second.view!=view)return ChatResult::unavailable;
    auto& op=it->second;auto* s=find(op.caller);
    if(!s||!op.active||now>=op.lease)return ChatResult::unavailable;
    if(text.empty()||text.size()>512||chat_safe_text(text)!=text||!sequence)return ChatResult::invalid;
    if(sequence==op.last_send)return text==op.last_send_text?ChatResult::ok:ChatResult::invalid;
    if(sequence!=op.last_send+1)return ChatResult::conflict;
    if(s->events.size()>=64||op.lines.size()>=64)return ChatResult::full;
    const auto line="Sysop: "+std::string(text);
    push(*s,{ChatEventKind::public_text,{},"Sysop",line});operator_line(op,line);
    op.last_send=sequence;op.last_send_text=text;op.lease=now+10000;return ChatResult::ok;
}
void ChatHub::operator_end(std::uint64_t token,Identifier128 view,std::uint64_t now){
    std::lock_guard<std::mutex> lock(mutex_);const auto it=operators_.find(token);
    if(it!=operators_.end()&&it->second.view==view)end_operator_locked(it->second,now,"Chat ended.");
}
void ChatHub::end_operator(const ChatIdentity& id,std::uint64_t now){
    std::lock_guard<std::mutex> lock(mutex_);auto* s=find(id);if(!s)return;const auto it=operators_.find(s->operator_token);
    if(it!=operators_.end())end_operator_locked(it->second,now,"Chat ended.");
}
