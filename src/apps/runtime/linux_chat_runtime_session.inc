// The caller worker owns presentation; the shared hub owns bounded routing and
// exact session identities. Storage and authority are refreshed at each entry.
const app::ChatIdentity chat_identity{ready.durable_session_id,ready.account_id,ready.node_id};
const auto chat_participant=[&]() -> std::optional<app::ChatParticipant>{
    const auto actor=message_actor();if(!actor)return {};
    const auto account=database().find_account_by_id(ready.account_id);
    const auto preferences=database().chat_preferences(*actor);const auto policy=database().security_policy();
    const auto stamp=local->format_utc_seconds(clock.unix_seconds());if(!stamp||stamp->size()<10)return {};
    const auto allowance=database().private_chat_allowance(*actor,stamp->substr(0,10));
    if(!account||!account.value||!account.value->security_level||!preferences||!preferences.value||!policy||!policy.value||!allowance||!allowance.value)return {};
    app::ChatParticipant participant;participant.identity=chat_identity;participant.name=own_name();
    const auto node=assignment->snapshot();participant.activity=node.door_id?"Door Menu ["+node.activity+"]":node.activity;
    participant.level=*account.value->security_level;participant.sysop=caller.live_sysop;participant.sysop_threshold=policy.value->sysop_threshold;
    participant.preferences=*preferences.value;participant.private_chat_minutes=allowance.value->minutes_per_chat;
    participant.private_seconds_remaining_today=allowance.value->seconds_remaining_today;return participant;
};
const auto initial_chat=chat_participant();if(!initial_chat||!chat.attach(*initial_chat))return false;
struct ChatDetach{
    app::ChatHub& hub;app::ChatIdentity identity;Caller& caller;
    ~ChatDetach(){
        const auto now=caller.owner.clock.monotonic_millis();hub.end_operator(identity,now);hub.end(identity,now);
        try{(void)caller.accounting_snapshot();}catch(...){caller.assignment->cleanup_health->failed=true;}
        hub.detach(identity,now);
    }
}chat_detach{chat,chat_identity,caller};
const auto chat_refresh=[&]{const auto value=chat_participant();return value&&chat.refresh(*value);};
const auto chat_asset=[&](std::string_view name)->std::optional<std::string>{
    const auto reader=open_native_display_reader(root/"display");if(!reader)return {};
    const auto result=reader->read_asset(name,32768);
    return result.code==app::TrustedDisplayAssetReadCode::available&&!result.bytes.empty()?std::optional<std::string>{result.bytes}:std::nullopt;
};
app::ChatSessionPorts chat_ports{terminal};
bool sysop_online_context{};
chat_ports.can_disconnect=[&]{return sysop_online_context&&sysop_actor(chat_identity).has_value();};
chat_ports.disconnect=[&](const app::ChatParticipant& target){
    const auto actor=sysop_actor(chat_identity);if(!actor)return std::string("Sysop access changed; no disconnect requested.");
    const auto authority=database().message_delete_authority(*actor);if(!authority||!authority.value||!*authority.value)return std::string("Sysop access changed.");
    if(!registry||!registry->sysop_disconnect(chat_identity,target.identity))return std::string("That caller left or access changed. No replacement caller was disconnected.");
    return std::string("Disconnect requested for ")+target.name+" on node "+std::to_string(target.identity.node)+". Normal cleanup is in progress.";
};
std::function<std::optional<app::MenuDoorCommands>(app::MenuId)> primary_door_commands;
std::function<std::optional<std::string>(const app::MenuDoorSelection&)> primary_door_launch;
chat_ports.read=[&]{struct Restore{MenuModalStream& stream;~Restore(){stream.preserve_poll_timeouts(false);}}restore{guarded};
    guarded.preserve_poll_timeouts(true);return terminal.read_editor_byte();};
chat_ports.write=[&](std::string_view text){return write_all(guarded,text);};
chat_ports.now=[&]{return clock.monotonic_millis();};chat_ports.refresh=chat_refresh;
chat_ports.ansi=[&]{return ready.graphics==app::GraphicsMode::ansi;};
chat_ports.rows=[&]{return assignment->snapshot().terminal_rows;};chat_ports.columns=[&]{return assignment->snapshot().terminal_columns;};
chat_ports.node_count=[&]{return config.logical_nodes;};chat_ports.accepted_key=[&](bool cr){terminal.finish_editor_key(cr);};
chat_ports.text_color=[&]{return caller.caller_color>0&&caller.caller_color<=15?caller.caller_color:config.menus.text_color;};
chat_ports.hotkeys=[&]{const auto saved=database().menu_preferences(ready.account_id);return saved&&saved.value&&saved.value->preferences.hotkey;};
chat_ports.emotes=[&]{const auto text=chat_asset("chat-emote.txt");return text?app::parse_chat_emotes(*text):std::vector<app::ChatEmote>{};};
chat_ports.private_art=[&](bool ansi){return chat_asset(ansi?"pchat.ans":"pchat.asc");};
chat_ports.page_accepted=[&]{journal->sound(app::SoundEvent::page_sysop);};
chat_ports.door_commands=[&]{return primary_door_commands?primary_door_commands(app::MenuId::chat):std::optional<app::MenuDoorCommands>{};};
chat_ports.door_command=[&](const app::MenuDoorSelection& selection){return primary_door_launch?primary_door_launch(selection):std::optional<std::string>{};};
app::ChatCallerSession chat_session(chat,chat_identity,std::move(chat_ports));
const auto chat_toggle=[&](app::MenuAction action)->std::optional<std::string>{
    const auto actor=message_actor();if(!actor)return {};
    const auto old=database().chat_preferences(*actor);if(!old||!old.value)return "Settings are unavailable.";
    auto next=*old.value;std::string name;
    if(action==app::MenuAction::chat_dnd){next.dnd=!next.dnd;name="Do Not Disturb";}
    else{next.notices=!next.notices;name="Logon / Logoff Notices";}
    const auto saved=database().save_chat_preferences(*actor,next);if(!saved||!saved.value)return "Not saved. Your settings may have changed.";
    if(!chat_refresh())return {};
    return name+((action==app::MenuAction::chat_dnd?next.dnd:next.notices)?": On.":": Off.");
};
