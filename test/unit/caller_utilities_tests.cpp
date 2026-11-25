#include "sfx/application/caller_utilities.hpp"
#include "sfx/application/calling_time.hpp"
#include <deque>
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;
unsigned checks{};
void check(bool ok,const char* text) {++checks;if(!ok) throw std::runtime_error(text);}
struct Terminal final:app::ICallerTerminal {
    std::string output;
    std::deque<char> keys;
    std::deque<std::string> values;
    app::TerminalOutcome set_text_style(app::TerminalTextStyle) override {return app::TerminalOutcome::completed;}
    app::TerminalOutcome clear_screen() override {return app::TerminalOutcome::completed;}
    app::TerminalInputResult read_visible(const app::TerminalInputRequest& request) override {
        output+=request.prompt;
        if(values.empty()) return {app::TerminalOutcome::disconnected,{}};
        auto text=values.front();values.pop_front();
        return {text.empty()?app::TerminalOutcome::empty:app::TerminalOutcome::completed,text};
    }
    app::TerminalInputResult read_secret(const app::TerminalInputRequest&) override {return {app::TerminalOutcome::invalid,{}};}
    app::TerminalKeyResult read_key(const app::TerminalKeyRequest& request) override {
        output+=request.prompt;
        if(keys.empty()) return {app::TerminalOutcome::disconnected,{}};
        auto key=keys.front();keys.pop_front();return {app::TerminalOutcome::completed,key};
    }
    app::TerminalOutcome present(const app::TerminalPresentationRequest& request) override {output+=request.non_secret_text;return app::TerminalOutcome::completed;}
    app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest&) override {return app::TerminalOutcome::invalid;}
    app::TerminalOutcome pause(const app::TerminalPauseRequest&) override {return app::TerminalOutcome::completed;}
};
struct Fixture {
    Terminal terminal;
    app::AccountRecord account;
    app::CallerUtilityPorts ports{terminal,5};
    unsigned saves{};
    bool conflict{};
    Fixture() {
        account.account_id=5;account.revision=1;account.display_name="Mellie";account.security_level=std::uint16_t{10};
        account.profile.real_name="Private Name";account.profile.birth_date="1980-02-29";
        account.statistics={7,120,86401,86402,app::SessionOutcomeCode::normal};
        ports.account=[&]{return std::optional<app::AccountRecord>{account};};
        ports.save_profile=[&](const app::AccountRecord& shown,const app::AccountProfile& candidate) {
            ++saves;check(shown.account_id==5 && shown.revision==account.revision,"self update carries stable ID and shown revision");
            if(conflict) {++account.revision;return false;}account.profile=candidate;++account.revision;return true;
        };
        ports.directory=[](const app::CallerDirectoryQuery&) {return app::StorageResult<std::vector<app::CallerDirectoryEntry>>{{},std::vector<app::CallerDirectoryEntry>{}};};
        ports.history=[](const app::HistoryQuery&) {return app::StorageResult<std::vector<app::SessionRecord>>{{},std::vector<app::SessionRecord>{}};};
        ports.utc_now=[] {return std::int64_t{100000};};
        ports.local_time=[](std::int64_t stamp)->std::optional<std::string> {
            return std::string(stamp<0?"2026-03-06":stamp<86400?"2026-03-07":stamp<169200?"2026-03-08":stamp<259200?"2026-03-09":"2026-03-10")+" 12:00:00";
        };
    }
};
int main() {try {
    Fixture base;
    const auto day=app::caller_local_day_range(base.ports.local_time,100000,false);
    check(day && day->first==86400 && day->second==169200,"configured local day has 23 real hours at spring transition");
    const auto fall=app::caller_local_day_range(base.ports.local_time,200000,false);
    check(fall && fall->second-fall->first==90000,"configured local day has 25 real hours at fall transition");
    const auto yesterday=app::caller_local_day_range(base.ports.local_time,100000,true);
    check(yesterday && yesterday->first==0 && yesterday->second==86400,"yesterday selects previous civil day, not UTC subtraction");
    check(!app::caller_local_day_range([](std::int64_t)->std::optional<std::string>{return {};},100000,false),"unavailable timezone fails closed");
    app::CallerTimeAccounting time(1000,base.ports.local_time);
    check(time.advance(1000,86395) && time.total_seconds()==0,"authentication starts with no charged time");
    check(time.advance(11000,86405) && time.total_seconds()==10 &&
        time.daily_seconds()==std::vector<std::pair<std::string,std::int64_t>>{{"2026-03-07",5},{"2026-03-08",5}},
        "monotonic elapsed splits at configured midnight");
    check(time.advance(11000,86405) && time.total_seconds()==10,"repeat checkpoint does not charge twice");
    check(time.advance(12000,86400) && time.total_seconds()==11,"wall-clock rollback cannot subtract elapsed time");
    check(time.advance(13000,200000) && time.total_seconds()==12,"wall-clock forward jump does not charge disconnected time");
    check(!time.advance(999,200000) && time.total_seconds()==12,"invalid monotonic clock fails without corrupting accounting");
    app::CallerTimeAccounting multi(0,base.ports.local_time);
    check(multi.advance(259200000,259200) && multi.total_seconds()==259200 &&
        multi.daily_seconds()==std::vector<std::pair<std::string,std::int64_t>>{{"2026-03-07",86400},{"2026-03-08",82800},{"2026-03-09",90000}},
        "long checkpoints preserve 23/25-hour local days rather than guessed 24-hour reset");
    app::SecurityLevelRecord limits;limits.level=10;limits.minutes_per_call=60;limits.minutes_per_day=180;
    check(app::caller_remaining_seconds(limits,120,9000)==1800,"daily allowance can be tighter than independent per-call allowance");
    check(app::caller_remaining_seconds(limits,3500,3500)==100,"per-call allowance can be tighter than daily allowance");
    check(app::caller_remaining_seconds(limits,3600,0)==0 && app::caller_remaining_seconds(limits,0,10800)==0,"either exhausted allowance ends the call");
    limits.level=500;limits.minutes_per_call.reset();limits.minutes_per_day.reset();
    check(!app::caller_remaining_seconds(limits,10000,10000),"unconfigured legacy or other level remains unlimited");
    limits.level=999;limits.minutes_per_call=1;limits.minutes_per_day=1;
    check(!app::caller_remaining_seconds(limits,10000,10000),"999-only time exemption never depends on Sysop threshold");
    auto changed=base.account.profile;changed.real_name="New Private Name";
    check(app::valid_caller_profile_change(base.account.profile,changed,{}),"separate real name editable with aliases");
    app::CallerProfileRules rules;rules.aliases_allowed=false;
    check(!app::valid_caller_profile_change(base.account.profile,changed,rules),"real-name change disallowed in real-name identity mode");
    changed=base.account.profile;changed.birth_date="1981-01-01";
    check(!app::valid_caller_profile_change(base.account.profile,changed,{}),"DOB correction never caller writable");
    rules={true,true,true,true};changed=base.account.profile;
    check(!app::valid_caller_profile_change(base.account.profile,changed,rules),"required profile fields cannot be cleared");
    changed.email="not-email";check(!app::valid_caller_profile_change(base.account.profile,changed,{}),"new email follows registration validation");
    changed=base.account.profile;changed.sex='X';check(app::valid_caller_profile_change(base.account.profile,changed,{}),"profile supports X independently of registration");
    for(const auto key:std::string("RLPES")) {
        Fixture f;f.terminal.keys={key,'Q'};
        f.terminal.values={key=='R'?"New Name":key=='L'?"City, ST":key=='P'?"555-0100":key=='E'?"me@example.invalid":"x"};
        check(app::run_caller_utility(app::MenuAction::caller_profile,f.ports).has_value() && f.saves==1,"all approved profile fields save");
        if(key=='S')check(f.account.profile.sex=='X'&&f.terminal.output.find("Current Sex (M/F/X)")!=std::string::npos,"profile advertises all choices and saves lowercase x as X");
        check(f.account.display_name=="Mellie" && f.account.profile.birth_date=="1980-02-29" && f.account.security_level==10,"profile save preserves identity DOB and security");
        check(f.terminal.output.find(">>>>>> JUST FOR YOUR INFORMATION <<<<<<")!=std::string::npos &&
            f.terminal.output.find(">>>>>>>> THINGS YOU CAN CHANGE <<<<<<<<")!=std::string::npos,"approved separated two-column headings");
        check(f.terminal.output.find("7")!=std::string::npos && f.terminal.output.find("2 minutes")!=std::string::npos,"statistics use real recorded data");
        check(f.terminal.output.find("Recorded authenticated time;")==std::string::npos &&
            f.terminal.output.find("DOB, caller name and security corrections:")==std::string::npos,
            "owner polish removes explanatory profile sentences");
        check(f.terminal.output.find("DOB:")!=std::string::npos && f.terminal.output.find("1980-02-29")!=std::string::npos &&
            f.terminal.output.find("Security:")!=std::string::npos && f.terminal.output.find("[H] History:")!=std::string::npos,
            "owner polish retains read-only DOB/security and personal history");
    }
    Fixture conflict;conflict.conflict=true;conflict.terminal.keys={'L','Q'};conflict.terminal.values={"New City"};
    check(app::run_caller_utility(app::MenuAction::caller_profile,conflict.ports).has_value() && !conflict.account.profile.location &&
        conflict.terminal.output.find("Not saved.")!=std::string::npos,"conflict does not silently overwrite a Sysop edit");
    Fixture cancel;cancel.terminal.keys={'L','Q'};cancel.terminal.values={""};
    check(app::run_caller_utility(app::MenuAction::caller_profile,cancel.ports).has_value() && !cancel.saves,"Enter leaves field unchanged");
    Fixture wrong;wrong.account.account_id=8;
    check(!app::run_caller_utility(app::MenuAction::caller_profile,wrong.ports) && !wrong.saves,"another account cannot be viewed or edited");
    for(const auto key:std::string("LYA")) {
        Fixture f;f.terminal.keys={key};
        f.ports.directory=[&](const app::CallerDirectoryQuery& query) {
            check(key=='A'?!query.logon_utc_range:query.logon_utc_range.has_value(),"approved date filters reach storage");
            return app::StorageResult<std::vector<app::CallerDirectoryEntry>>{{},std::vector<app::CallerDirectoryEntry>{{"Mellie","Public city",86401}}};
        };
        check(app::run_caller_utility(app::MenuAction::caller_directory,f.ports).has_value(),"directory all today yesterday readable");
        check(f.terminal.output.find(" | ")!=f.terminal.output.npos&&f.terminal.output.find("\r\n\r\nPress <ENTER>")!=f.terminal.output.npos,"caller directory separates name/location/date and leaves a blank before continue");
        check(f.terminal.output.find("Public city")!=std::string::npos && f.terminal.output.find("Private Name")==std::string::npos &&
            f.terminal.output.find("1980")==std::string::npos,"public directory contains no private profile");
    }
    Fixture locate;locate.terminal.values={"ell"};locate.ports.page_rows=5;locate.terminal.keys={'N'};
    locate.ports.directory=[&](const app::CallerDirectoryQuery& query) {
        check(query.name_fragment=="ell","full or partial name preserved to storage");
        std::vector<app::CallerDirectoryEntry> rows(query.offset?1:5,{"Mellie","City",{}});
        return app::StorageResult<std::vector<app::CallerDirectoryEntry>>{{},rows};
    };
    check(app::run_caller_utility(app::MenuAction::locate_caller,locate.ports).has_value() && locate.terminal.output.find("Enter full or partial name:")!=std::string::npos,"locate returns every match with paging");
    Fixture empty;empty.terminal.keys={'A'};
    check(app::run_caller_utility(app::MenuAction::caller_directory,empty.ports).has_value() && empty.terminal.output.find("No callers match")!=std::string::npos,"empty caller selection explicit");
    Fixture hist;hist.terminal.keys={'H','Q'};
    hist.ports.history=[&](const app::HistoryQuery& query) {
        check(query.account_id==5,"personal call history bound to authenticated identity");
        app::SessionRecord row;row.account_id=5;row.node_id=2;row.started_utc_seconds=86401;row.persisted_seconds=125;row.outcome=app::SessionOutcomeCode::normal;
        return app::StorageResult<std::vector<app::SessionRecord>>{{},std::vector<app::SessionRecord>{row}};
    };
    check(app::run_caller_utility(app::MenuAction::caller_profile,hist.ports).has_value() && hist.terminal.output.find("YOUR CALL HISTORY")!=std::string::npos &&
        hist.terminal.output.find("normal")!=std::string::npos,"personal history clearly accessible and truthful");
    std::cout<<"PASS caller utilities "<<checks<<" checks\n";return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}}
