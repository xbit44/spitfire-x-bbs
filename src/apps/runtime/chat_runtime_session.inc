// One caller worker owns this presentation. The hub only routes bounded events.
const ChatIdentity chat_identity{ready->durable_session_id,ready->account_id,state->snapshot().number};
const auto chat_allowance=[&](const MessageActor& actor)->StorageResult<PrivateChatAllowance>{
    std::shared_ptr<TimeContext> local;
    {std::lock_guard<std::mutex> lock(owner.state_mutex);local=owner.time_context;}
    const auto stamp=local&&local->local?local->local->format_utc_seconds(owner.clock.unix_seconds()):std::nullopt;
    if(!stamp||stamp->size()<10)return {{StorageErrorCode::internal,"private Chat date unavailable"},{}};
    return owner.storage.private_chat_allowance(actor,stamp->substr(0,10));
};
const auto chat_refresh=[&] {
    if(!modal_tick())return false;
    const auto actor=message_actor();if(!actor)return false;
    const auto preference=owner.storage.chat_preferences(*actor);
    const auto policy=owner.storage.security_policy();
    const auto allowance=chat_allowance(*actor);
    if(!preference||!preference.value||!policy||!policy.value||!allowance||!allowance.value)return false;
    const auto node=state->snapshot();
    ChatParticipant caller;caller.identity=chat_identity;caller.name=own_name();
    caller.activity=node.door_id?"Door Menu ["+node.activity+"]":node.activity;
    caller.level=live_security_level.load();caller.sysop=live_sysop.load();
    caller.sysop_threshold=policy.value->sysop_threshold;caller.preferences=*preference.value;
    caller.private_chat_minutes=allowance.value->minutes_per_chat;caller.private_seconds_remaining_today=allowance.value->seconds_remaining_today;
    return owner.chat_hub.refresh(caller);
};
{
    const auto actor=message_actor();if(!actor)return false;
    const auto preference=owner.storage.chat_preferences(*actor);const auto policy=owner.storage.security_policy();
    const auto allowance=chat_allowance(*actor);
    if(!preference||!preference.value||!policy||!policy.value||!allowance||!allowance.value)return false;
    ChatParticipant caller;caller.identity=chat_identity;caller.name=own_name();caller.activity="Main Menu";
    caller.level=live_security_level.load();caller.sysop=live_sysop.load();caller.sysop_threshold=policy.value->sysop_threshold;
    caller.private_chat_minutes=allowance.value->minutes_per_chat;caller.private_seconds_remaining_today=allowance.value->seconds_remaining_today;
    caller.preferences=*preference.value;if(!owner.chat_hub.attach(std::move(caller)))return false;
}
struct ChatDetach {ChatHub& hub;ChatIdentity identity;Connection& caller;~ChatDetach(){
    hub.end_operator(identity,caller.owner.clock.monotonic_millis());
    hub.end(identity,caller.owner.clock.monotonic_millis());
    try{(void)caller.accounting_snapshot();}catch(...){caller.state->cleanup_health->failed.store(true);}
    hub.detach(identity,caller.owner.clock.monotonic_millis());
}} chat_detach{owner.chat_hub,chat_identity,*this};
const auto chat_asset=[&](std::string_view name)->std::optional<std::string>{
    const auto reader=platform::win32::Win32TrustedDisplayAssetReader::open((owner.database_path.parent_path().parent_path()/L"Display").wstring());
    if(!reader)return {};const auto result=reader->read_asset(name,32768);
    if(result.bytes.empty())return {};return result.bytes;
};
bool sysop_online_context=false;
ChatSessionPorts chat_ports{terminal};
chat_ports.hotkeys=[&]{const auto saved=owner.storage.menu_preferences(ready->account_id);return saved&&saved.value&&saved.value->preferences.hotkey;};
chat_ports.can_disconnect=[&]{return sysop_online_context&&owner.sysop_actor(chat_identity).has_value();};
chat_ports.disconnect=[&](const ChatParticipant& target){
    if(!owner.sysop_actor(chat_identity))return std::string("Sysop access changed; no disconnect requested.");
    std::lock_guard<std::mutex> lock(owner.state_mutex);
    if(!live_sysop.load()||state->stopping.load()||!target.identity.node||target.identity.node>owner.active_contexts.size())return std::string("Caller or authority changed; refresh the list.");
    const auto actor=message_actor();if(!actor)return std::string("Sysop access changed.");
    const auto authority=owner.storage.message_delete_authority(*actor);if(!authority||!authority.value||!*authority.value)return std::string("Sysop access changed.");
    const auto& selected=owner.active_contexts[target.identity.node-1];
    if(!selected||selected->state->stopping.load()||selected->published_account!=target.identity.account||selected->published_session!=target.identity.session)return std::string("That caller left or the node was reassigned. No replacement caller was disconnected.");
    selected->cancel();return std::string("Disconnect requested for ")+target.name+" on node "+std::to_string(target.identity.node)+". Normal cleanup is in progress.";
};
chat_ports.page_accepted=[&]{if(owner.journal)owner.journal->sound(SoundEvent::page_sysop);};
std::function<std::optional<MenuDoorCommands>(MenuId)> primary_door_commands;
std::function<std::optional<std::string>(const MenuDoorSelection&)> primary_door_launch;
chat_ports.read=[&]{struct Restore {MenuModalStream& stream;~Restore(){stream.preserve_poll_timeouts(false);}} restore{modal_stream};modal_stream.preserve_poll_timeouts(true);return terminal.read_editor_byte();};
chat_ports.write=[&](std::string_view text){return write_all(modal_stream,text);};
chat_ports.now=[] {return GetTickCount64();};chat_ports.refresh=chat_refresh;
chat_ports.ansi=[&]{return ready->graphics==GraphicsMode::ansi;};
chat_ports.text_color=[&]{
    const auto saved=owner.storage.menu_preferences(ready->account_id);
    if(saved&&saved.value&&saved.value->preferences.color)return saved.value->preferences.color;
    std::lock_guard<std::mutex> lock(owner.state_mutex);return owner.configuration.menus.text_color;
};
chat_ports.rows=[&]{return state->snapshot().terminal_rows;};chat_ports.columns=[&]{return state->snapshot().terminal_columns;};
chat_ports.node_count=[&]{std::lock_guard<std::mutex> lock(owner.state_mutex);return owner.configuration.logical_nodes;};
chat_ports.accepted_key=[&](bool cr){terminal.finish_editor_key(cr);};
chat_ports.emotes=[&]{const auto text=chat_asset("chat-emote.txt");return text?parse_chat_emotes(*text):std::vector<ChatEmote>{};};
chat_ports.private_art=[&](bool ansi){return chat_asset(ansi?"pchat.ans":"pchat.asc");};
chat_ports.door_commands=[&]{return primary_door_commands?primary_door_commands(MenuId::chat):std::optional<MenuDoorCommands>{};};
chat_ports.door_command=[&](const MenuDoorSelection& selection){return primary_door_launch?primary_door_launch(selection):std::optional<std::string>{};};
ChatCallerSession chat(owner.chat_hub,chat_identity,std::move(chat_ports));
const auto chat_toggle=[&](MenuAction action)->std::optional<std::string>{
    const auto actor=message_actor();if(!actor)return {};const auto old=owner.storage.chat_preferences(*actor);if(!old||!old.value)return "Settings are unavailable.";
    auto next=*old.value;std::string name;
    if(action==MenuAction::chat_dnd){next.dnd=!next.dnd;name="Do Not Disturb";}
    else{next.notices=!next.notices;name="Logon / Logoff Notices";}
    const auto saved=owner.storage.save_chat_preferences(*actor,next);if(!saved||!saved.value)return "Not saved. Your settings may have changed.";
    if(!chat_refresh())return {};return name+((action==MenuAction::chat_dnd?next.dnd:next.notices)?": On.":": Off.");
};
