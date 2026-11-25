#include "sfx/application/caller_utilities.hpp"
#include "sfx/application/menu_session.hpp"
#include <algorithm>
#include <cctype>

namespace sfx::application {
namespace {
bool valid_email(std::string_view value) {
    if(value.empty() || value.size()>kEmailMaximum || value.front()=='@' || value.back()=='@' ||
        value.front()=='.' || value.back()=='.' || value.find(' ')!=std::string_view::npos) return false;
    const auto at=value.find('@');const auto dot=value.find('.',at==std::string_view::npos?value.size():at+2U);
    return at!=std::string_view::npos && at==value.rfind('@') && at>0 && dot!=std::string_view::npos && dot+1<value.size();
}
std::string cell(std::string value,std::size_t width) {
    // Values are inert, never display-token source. Clip only directory columns;
    // callers can inspect their own full profile value separately.
    for(char& ch:value) if(static_cast<unsigned char>(ch)<32 || ch==127) ch=' ';
    if(value.size()>width) value=value.substr(0,width-1)+"~";
    return value+std::string(width-value.size(),' ');
}
std::string value(const std::optional<std::string>& text) {return text.value_or("Not supplied");}
std::string timestamp(const CallerUtilityPorts& p,const std::optional<std::int64_t>& instant) {
    if(!instant) return "Never";
    const auto local=p.local_time(*instant);
    return local && local->size()>=19?local->substr(0,19):"Unavailable";
}
bool show(const CallerUtilityPorts& p,std::string text) {
    return p.terminal.present({"caller.utilities",std::move(text)})==TerminalOutcome::completed;
}
bool finish(const CallerUtilityPorts& p) {
    return show(p,"Press <ENTER> to continue.") && p.terminal.pause({"caller.utilities.return"})==TerminalOutcome::completed;
}
bool field(const CallerUtilityPorts& p,std::string label,std::string data) {
    return show(p,std::move(label)) &&
        p.terminal.present({"caller.utilities.value",std::move(data),TerminalPresentationRole::caller_value})==TerminalOutcome::completed;
}
bool pair(const CallerUtilityPorts& p,std::string_view left_label,std::string left,std::string_view right_label,std::string right) {
    return field(p,cell(std::string(left_label),14),cell(std::move(left),24)) &&
        field(p,"  "+cell(std::string(right_label),14),std::move(right)) && show(p,"\r\n");
}
std::optional<std::string> history(const CallerUtilityPorts& p) {
    HistoryQuery query;query.account_id=p.account_id;query.at_or_before_utc_seconds=p.utc_now();
    query.limit=std::clamp<unsigned>(p.page_rows,5,20);
    for(;;) {
        const auto page=p.history(query);if(!page || !page.value) return "Call history is unavailable. Please try again later.";
        std::string text="\r\nYOUR CALL HISTORY\r\nLogon date/time     Node  Time online      Outcome\r\n";
        for(const auto& row:*page.value) {
            if(row.account_id!=p.account_id) return std::nullopt;
            text+=cell(timestamp(p,row.started_utc_seconds),20)+cell(std::to_string(row.node_id),6)+
                cell(menu_duration_words(static_cast<std::uint64_t>(std::max<std::int64_t>(0,row.persisted_seconds))),17)+
                (row.outcome?std::string(storage_code(*row.outcome)):"Online")+"\r\n";
        }
        if(page.value->empty()) return show(p,query.after_session?"End of call history.\r\n":"No calls recorded.\r\n") && finish(p)
            ?std::optional<std::string>{""}:std::nullopt;
        if(!show(p,text)) return std::nullopt;
        if(page.value->size()<query.limit) return finish(p)?std::optional<std::string>{""}:std::nullopt;
        const auto next=p.terminal.read_key({"caller.history.page","[N]ext page [Q]uit: ",{'N','n','Q','q'},'Q'});
        if(!next.has_key()) return std::nullopt;
        if(*next.key=='Q' || *next.key=='q') return std::string{};
        query.after_session={{page.value->back().started_utc_seconds,page.value->back().session_id}};
    }
}
std::optional<std::string> profile(const CallerUtilityPorts& p) {
    for(;;) {
        const auto record=p.account();
        if(!record || record->account_id!=p.account_id || record->state!=AccountState::active) return std::nullopt;
        const auto& a=*record;
        const auto& f=a.profile;
        if(!show(p,"\r\n>>>>>> JUST FOR YOUR INFORMATION <<<<<<\r\n\r\n") ||
            !pair(p,"Caller name:",value(a.display_name),"Security:",a.security_level?std::to_string(*a.security_level):"Unavailable") ||
            !pair(p,"DOB:",value(f.birth_date),"Logons:",std::to_string(a.statistics.successful_login_count)) ||
            !pair(p,"Last logon:",timestamp(p,a.statistics.last_successful_login_utc_seconds),"Total time:",
                menu_duration_words(static_cast<std::uint64_t>(std::max<std::int64_t>(0,a.statistics.authenticated_session_seconds)))) ||
            !show(p,"\r\n>>>>>>>> THINGS YOU CAN CHANGE <<<<<<<<\r\n\r\n") ||
            !pair(p,p.rules.aliases_allowed?"[R] Real Name:":"Real Name:",value(f.real_name),"[L] Location:",value(f.location)) ||
            !pair(p,"[P] Phone:",value(f.phone),"[E] Email:",value(f.email)) ||
            !pair(p,"[S] Sex:",f.sex?std::string(1,*f.sex):"Not supplied","[H] History:","Your individual calls"))return std::nullopt;
        const auto ghost=p.ghost?p.ghost():std::nullopt;
        if(ghost && (!field(p,"[G] Ghost Mode: ",*ghost?"On":"Off")||!show(p,"\r\n")))return std::nullopt;
        auto keys=std::vector<char>{'L','l','P','p','E','e','S','s','H','h','Q','q'};
        if(p.rules.aliases_allowed) keys.insert(keys.end(),{'R','r'});
        if(ghost)keys.insert(keys.end(),{'G','g'});
        const auto choice=p.terminal.read_key({"caller.profile.choice","Choose a listed key, [Q]uit: ",keys,'Q'});
        if(!choice.has_key()) return std::nullopt;
        const char key=static_cast<char>(std::toupper(static_cast<unsigned char>(*choice.key)));
        if(key=='Q') return std::string{};
        if(key=='G'&&ghost){if(!show(p,p.toggle_ghost&&p.toggle_ghost()?"Saved.\r\n":"Not saved.\r\n"))return std::nullopt;continue;}
        if(key=='H') {const auto result=history(p);if(!result) return std::nullopt;if(!result->empty() && !show(p,*result+"\r\n")) return std::nullopt;continue;}
        AccountProfile changed=f;
        const auto maximum=key=='E'?kEmailMaximum:key=='P'?kPhoneMaximum:key=='S'?1U:key=='L'?kLocationMaximum:kRealNameMaximum;
        const auto prompt=key=='S'?"Sex (M/F/X)":key=='E'?"Email":key=='P'?"Phone":key=='L'?"Location":"Real Name";
        const auto current=key=='S'?(f.sex?std::string(1,*f.sex):"Not supplied"):key=='E'?value(f.email):key=='P'?value(f.phone):key=='L'?value(f.location):value(f.real_name);
        if(!field(p,"\r\nCurrent "+std::string(prompt)+": ",current)||!show(p,"\r\n")) return std::nullopt;
        const auto entry=p.terminal.read_visible({"caller.profile.value",std::string(prompt)+" (Enter unchanged, - clears optional field): ",maximum,EmptyInputPolicy::use_default});
        if(entry.outcome==TerminalOutcome::cancelled || entry.outcome==TerminalOutcome::empty) continue;
        if(entry.outcome==TerminalOutcome::invalid || entry.outcome==TerminalOutcome::overlength) {
            if(!show(p,"Invalid value. Nothing changed.\r\n")) return std::nullopt;
            continue;
        }
        if(!entry.has_value()) return std::nullopt;
        const auto normalized=entry.value=="-"?std::optional<std::string>{}:
            key=='R'?normalize_real_name(entry.value):normalize_optional_profile_text(entry.value,maximum);
        if(entry.value!="-" && (!normalized || *normalized!=entry.value)) {
            if(!show(p,"Invalid value. Nothing changed.\r\n")) return std::nullopt;
            continue;
        }
        switch(key) {
            case 'R':changed.real_name=normalized;break;
            case 'L':changed.location=normalized;break;
            case 'P':changed.phone=normalized;break;
            case 'E':changed.email=normalized;break;
            case 'S':changed.sex=normalized?std::optional<char>{static_cast<char>(std::toupper(static_cast<unsigned char>((*normalized)[0])))}:std::nullopt;break;
        }
        if(!valid_caller_profile_change(f,changed,p.rules)) {
            if(!show(p,"Invalid or required field. Nothing changed.\r\n")) return std::nullopt;
            continue;
        }
        if(!show(p,p.save_profile(a,changed)?"Saved.\r\n":"Not saved. Your record may have changed; please choose again.\r\n")) return std::nullopt;
    }
}
}
bool valid_caller_profile_change(const AccountProfile& previous,const AccountProfile& candidate,const CallerProfileRules& rules) {
    return valid_profile(candidate) && candidate.birth_date==previous.birth_date &&
        (rules.aliases_allowed || candidate.real_name==previous.real_name) &&
        (!rules.location_required || candidate.location) && (!rules.email_required || candidate.email) &&
        (!candidate.email || candidate.email==previous.email || valid_email(*candidate.email)) &&
        (!rules.sex_required || candidate.sex);
}
std::optional<std::pair<std::int64_t,std::int64_t>> caller_local_day_range(
    const CallerLocalFormatter& format,std::int64_t now,bool yesterday) {
    if(!format || !valid_utc_seconds(now) || now<kMinimumUtcSeconds+4*86400LL || now>kMaximumUtcSeconds-4*86400LL) return std::nullopt;
    const auto today=format(now);if(!today || today->size()<10) return std::nullopt;
    auto date=today->substr(0,10);
    if(!valid_iso_civil_date(date)) return std::nullopt;
    const auto boundary=[&](const std::string& target,bool inclusive) -> std::optional<std::int64_t> {
        auto low=now-4*86400LL,high=now+2*86400LL;
        while(low<high) {
            const auto mid=low+(high-low)/2;const auto local=format(mid);
            if(!local || local->size()<10) return std::nullopt;
            if(local->substr(0,10)<target || (!inclusive && local->substr(0,10)==target)) low=mid+1;
            else high=mid;
        }
        return low;
    };
    auto first=boundary(date,true);if(!first) return std::nullopt;
    if(yesterday) {const auto previous=format(*first-1);if(!previous || previous->size()<10) return std::nullopt;date=previous->substr(0,10);}
    const auto start=boundary(date,true),end=boundary(date,false);
    return start && end && *start<*end?std::optional<std::pair<std::int64_t,std::int64_t>>{{*start,*end}}:std::nullopt;
}
std::optional<std::string> run_caller_utility(MenuAction action,const CallerUtilityPorts& p) {
    if(p.account_id<=0 || !p.account || !p.save_profile || !p.history || !p.directory || !p.local_time || !p.utc_now) return std::nullopt;
    if(action==MenuAction::caller_profile) return profile(p);
    CallerDirectoryQuery query;query.limit=std::clamp<unsigned>(p.page_rows,5,20);
    if(action==MenuAction::locate_caller) {
        const auto input=p.terminal.read_visible({"caller.locate","\r\nEnter full or partial name: ",kAccountNameMaximum,EmptyInputPolicy::use_default});
        if(input.outcome==TerminalOutcome::cancelled || input.outcome==TerminalOutcome::empty) return std::string{};
        if(!input.has_value()) return std::nullopt;
        query.name_fragment=input.value;
    } else if(action==MenuAction::caller_directory) {
        const auto choice=p.terminal.read_key({"caller.directory.filter","\r\n[L]ogons today [Y]esterday [A]ll: ",{'L','l','Y','y','A','a'},'A'});
        if(choice.outcome==TerminalOutcome::cancelled) return std::string{};
        if(!choice.has_key()) return std::nullopt;
        const auto key=static_cast<char>(std::toupper(static_cast<unsigned char>(*choice.key)));
        if(key!='A') {query.logon_utc_range=caller_local_day_range(p.local_time,p.utc_now(),key=='Y');if(!query.logon_utc_range) return "Caller dates are unavailable.";}
    } else return std::nullopt;
    for(;;) {
        const auto rows=p.directory(query);if(!rows || !rows.value) return "Caller directory is unavailable. Please try again later.";
        if(rows.value->empty()) return show(p,query.offset?"\r\nEnd of caller list.\r\n":"\r\nNo callers match this selection.\r\n") && finish(p)?std::optional<std::string>{""}:std::nullopt;
        std::string text="\r\n"+cell("Name",30)+" | "+cell("Location",24)+" | Last online\r\n";
        for(const auto& row:*rows.value) text+=cell(row.name,30)+" | "+cell(row.location.value_or(""),24)+" | "+timestamp(p,row.last_logon_utc)+"\r\n";
        text+="\r\n";
        if(!show(p,text)) return std::nullopt;
        if(rows.value->size()<query.limit) return finish(p)?std::optional<std::string>{""}:std::nullopt;
        const auto next=p.terminal.read_key({"caller.directory.page","[N]ext page [Q]uit: ",{'N','n','Q','q'},'Q'});
        if(!next.has_key()) return std::nullopt;
        if(*next.key=='Q' || *next.key=='q') return std::string{};
        query.offset+=rows.value->size();
    }
}
} // namespace sfx::application
