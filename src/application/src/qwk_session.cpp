#include "sfx/application/qwk_session.hpp"
#include "sfx/application/message_presentation.hpp"
namespace sfx::application {
namespace {
bool cancel(const TerminalKeyResult& r){return r.outcome==TerminalOutcome::cancelled||r.key=='Q';}
std::string yes(bool v){return v?"Yes":"No";}
}
bool QwkCallerSession::say(std::string_view text){
    // Inert CP437 terminals cannot represent the model's Unicode em dash.
    // Preserve the sentence with a readable dash instead of UTF8 mojibake.
    std::string visible(text);for(auto at=visible.find(u8"—");at!=std::string::npos;at=visible.find(u8"—",at+1))visible.replace(at,3,"-");
    return present_message_text(ports_.terminal,"qwk.notice",visible);
}
std::optional<std::string> QwkCallerSession::held(std::string_view text){
    if(!say(text))return {};
    const auto key=ports_.terminal.read_key({"qwk.continue","<Enter> ",{'C'},'C',false,true});return key.has_key()||key.outcome==TerminalOutcome::cancelled?std::optional<std::string>{""}:std::nullopt;
}
std::optional<std::string> QwkCallerSession::settings(){
    for(;;){
        const auto actor=ports_.actor();if(!actor)return {};const auto current=ports_.storage.qwk_preferences(*actor);if(!current||!current.value)return held("Packet settings unavailable; no changes saved.");
        auto p=current.value->preferences;const auto private_text=p.private_mail==QwkPrivateMode::unread?"Unread Only":p.private_mail==QwkPrivateMode::all_received?"All received":"None";
        const auto wrap=p.word_wrap?(*p.word_wrap?"On":"Off"):"Pending owner default; choose On/Off";
        if(!present_message_text(ports_.terminal,"qwk.options",std::string("QWK packet settings - ZIP / ZMODEM\n\n")+
            "[I] Received private mail: "+private_text+"\n[O] Own public posts: "+yes(p.own_public_posts)+
            "\n[E] Encoding: "+(p.encoding==QwkEncoding::cp437?"CP437":"UTF-8")+
            "\n[C] Colors/art: "+(p.preserve_colors?"Preserve supported colors/art":"Plain Text")+
            "\n[W] Word-wrap: "+wrap+"\n[N] Index files: "+yes(p.indexes)+"\n[H] Full headers: "+yes(p.full_headers)+
            "\n[Q] Return"))return {};
        const auto choice=ports_.terminal.read_key({"qwk.settings","Packet setting [I O E C W N H Q]: ",{'I','O','E','C','W','N','H','Q'},'Q',false,true});
        if(cancel(choice))return "";
        if(!choice.has_key())return {};
        if(choice.key=='I'){
            const auto v=ports_.terminal.read_key({"qwk.private","[U] Unread Only, [A] All received, [N] None, [Q] Cancel: ",{'U','A','N','Q'},'Q',false,true});if(cancel(v))continue;if(!v.has_key())return {};
            p.private_mail=v.key=='U'?QwkPrivateMode::unread:v.key=='A'?QwkPrivateMode::all_received:QwkPrivateMode::none;
        }else if(choice.key=='E'){
            const auto v=ports_.terminal.read_key({"qwk.encoding","[C] CP437, [U] UTF-8 (requires Full headers Yes), [Q] Cancel: ",{'C','U','Q'},'Q',false,true});if(cancel(v))continue;if(!v.has_key())return {};p.encoding=v.key=='C'?QwkEncoding::cp437:QwkEncoding::utf8;
        }else{
            const auto v=ports_.terminal.read_key({"qwk.toggle",choice.key=='C'?"[Y] Preserve supported colors/art, [N] Plain Text, [Q] Cancel: ":choice.key=='W'?"[Y] Wrap On, [N] Wrap Off, [Q] Cancel: ":"[Y] Yes, [N] No, [Q] Cancel: ",{'Y','N','Q'},'Q',false,true});if(cancel(v))continue;if(!v.has_key())return {};const bool value=v.key=='Y';
            if(choice.key=='O')p.own_public_posts=value;else if(choice.key=='C')p.preserve_colors=value;else if(choice.key=='W')p.word_wrap=value;else if(choice.key=='N')p.indexes=value;else p.full_headers=value;
        }
        if(p.encoding==QwkEncoding::utf8&&!p.full_headers){if(!say("UTF-8 requires Full headers Yes. Select CP437 before turning full headers Off. Nothing changed."))return {};continue;}
        if(!p.word_wrap){if(!say("Choose explicit Word-wrap On or Off before saving development packet settings."))return {};continue;}
        const auto live=ports_.actor();if(!live)return {};const auto saved=ports_.storage.save_qwk_preferences(*live,current.value->revision,p);
        if(!say(saved?"Packet choice saved.":"Packet settings changed or unavailable; nothing saved. Reloading current choices."))return {};
    }
}
std::optional<std::string> QwkCallerSession::pointers(){
    if(!present_message_text(ports_.terminal,"qwk.options",
        "New-message Pointers\n\n[E] Current echo\n[G] Current group\n[A] All accessible\n[Q] Cancel"))return {};
    const auto scope=ports_.terminal.read_key({"qwk.pointer.scope","Public pointers [E G A Q]? [E]: ",{'E','G','A','Q'},'E',false,true});
    if(cancel(scope))return "";
    if(!scope.has_key())return {};
    QwkPointerChange request;request.operation=ports_.operation();request.scope=scope.key=='G'?MessageQueryScope::current_group:scope.key=='A'?MessageQueryScope::all_accessible:MessageQueryScope::current_echo;
    const auto selected=ports_.selection();if(selected){if(request.scope==MessageQueryScope::current_echo)request.echo=selected->echo.id;else if(request.scope==MessageQueryScope::current_group)request.group=selected->group.id;}else if(request.scope!=MessageQueryScope::all_accessible)return held("No current accessible echo/group. Choose one from the Message Menu first.");
    if(!present_message_text(ports_.terminal,"qwk.options",
        "Set public pointers to:\n\n[A] All available\n[D] Since BBS-local date/time\n[C] Caught up\n[Q] Cancel"))return {};
    const auto mode=ports_.terminal.read_key({"qwk.pointer.mode","Pointer setting [A D C Q]? [Q]: ",{'A','D','C','Q'},'Q',false,true});
    if(cancel(mode))return "";
    if(!mode.has_key())return {};
    request.mode=mode.key=='A'?QwkPointerMode::all_available:mode.key=='D'?QwkPointerMode::since_entry_time:QwkPointerMode::caught_up;
    if(mode.key=='D'){
        const auto text=ports_.terminal.read_visible({"qwk.pointer.since","BBS-local YYYY-MM-DD HH:MM ([Q]/Escape cancels): ",16,EmptyInputPolicy::use_default,{},true});
        if(text.outcome==TerminalOutcome::cancelled||text.outcome==TerminalOutcome::empty||text.value=="Q"||text.value=="q")return "";
        if(!text.has_value())return {};
        request.since_utc=ports_.local_entry_utc?ports_.local_entry_utc(text.value):std::nullopt;if(!request.since_utc)return held("Invalid, unavailable or ambiguous BBS-local date/time. No pointers changed.");
    }
    const auto actor=ports_.actor();if(!actor)return {};const auto preview=ports_.storage.preview_qwk_public_pointers(*actor,request);
    if(!preview||!preview.value)return held("Public pointers unavailable or changed. No pointers changed.");
    if(preview.value->expected.empty())return held("No accessible public echoes in this scope. No pointers changed.");
    if(!say("Change public New-message positions for "+std::to_string(preview.value->expected.size())+" accessible echo(s).\nScan flags and private-mail Read status will not change."))return {};
    const auto confirm=ports_.terminal.read_key({"qwk.pointer.confirm","Apply this public-pointer change? [y/N] ",{'Y','N'},'N',false,true});
    if(confirm.outcome==TerminalOutcome::cancelled||confirm.key=='N')return "";
    if(!confirm.has_key())return {};
    const auto live=ports_.actor();if(!live)return {};const auto changed=ports_.storage.change_qwk_public_pointers(*live,*preview.value);
    return held(changed?"Public New-message positions changed. Scan flags/private-mail Read unchanged.":"Public pointers changed or unavailable; no success is claimed.");
}
std::optional<std::string> QwkCallerSession::action(MenuAction action){
    if(action==MenuAction::qwk_settings)return settings();
    if(action==MenuAction::qwk_pointers)return pointers();
    if(action==MenuAction::qwk_download||action==MenuAction::qwk_upload){const auto& operation=action==MenuAction::qwk_download?ports_.download:ports_.upload;if(!operation)return held("Offline packet transfer unavailable.");const auto result=operation();return result?held(*result):std::nullopt;}
    return std::string(unavailable_menu_feature);
}
} // namespace sfx::application
