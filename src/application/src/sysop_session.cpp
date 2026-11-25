#include "sfx/application/sysop_session.hpp"
#include <algorithm>
#include <charconv>
#include <set>

namespace sfx::application {
namespace {
std::string inert(std::string_view input){std::string out;for(unsigned char c:input)out+=(c>=32&&c!=127)?static_cast<char>(c):' ';return out;}
std::optional<std::uint64_t> number(std::string_view s){std::uint64_t n{};const auto r=std::from_chars(s.data(),s.data()+s.size(),n);if(r.ec!=std::errc{}||r.ptr!=s.data()+s.size())return {};return n;}
char upper(char c){return c>='a'&&c<='z'?static_cast<char>(c-32):c;}
}
std::vector<std::string> wrap_sysop_log_text(std::string_view text,unsigned columns,bool indent){
    const auto width=std::max(1U,columns>1?columns-1:1U);
    std::vector<std::string> lines;
    std::size_t start{};
    do {
        const auto end=text.find('\n',start);
        const auto clean=inert(text.substr(start,end==text.npos?text.size()-start:end-start));
        std::string_view remaining=clean;
        bool continuation=false;
        do {
            const std::string prefix(continuation&&indent?std::min(2U,width-1):0U,' ');
            const auto capacity=width-prefix.size();
            auto take=std::min(capacity,remaining.size());
            if(remaining.size()>capacity){
                const auto space=remaining.rfind(' ',capacity);
                if(space!=remaining.npos&&space>0)take=space;
            }
            lines.push_back(prefix+std::string(remaining.substr(0,take)));
            remaining.remove_prefix(take);
            if(!remaining.empty())remaining.remove_prefix(std::min(remaining.find_first_not_of(' '),remaining.size()));
            continuation=true;
        }while(!remaining.empty());
        if(end==text.npos)break;
        start=end+1;
    }while(start<text.size());
    return lines;
}
unsigned sysop_log_content_rows(std::string_view title,std::string_view details,
    std::uint64_t total_rows,unsigned columns,unsigned rows){
    const auto count=std::to_string(std::max<std::uint64_t>(1,total_rows));
    const auto reserved=wrap_sysop_log_text(title,columns,false).size()+1+
        wrap_sysop_log_text("Page "+count+" of "+count,columns,false).size()+
        (details.empty()?0:wrap_sysop_log_text(details,columns,false).size())+
        wrap_sysop_log_text(kSysopLogControls,columns,false).size()+1;
    return rows>reserved?static_cast<unsigned>(rows-reserved):0;
}
std::string sysop_account_result(const LocalAccountResult& r){
    if(r.committed)return r.code==LocalAccountResultCode::ok?"Saved. Changes take effect under the existing login and access rules.":"Saved, but follow-up is incomplete. Reopen to check the result; do not repeat the change.";
    switch(r.code){
    case LocalAccountResultCode::conflict:return "Not saved: the record changed. Your draft is retained. Reload explicitly before editing the current record.";
    case LocalAccountResultCode::last_sysop_protected:return "Not saved: the last active Sysop account is protected.";
    case LocalAccountResultCode::system_sysop_protected:return "Not saved: the system Sysop account is protected.";
    case LocalAccountResultCode::invalid_request:case LocalAccountResultCode::policy_rejected:return "Not saved: check the name, profile, security level and password rules. Your draft is retained.";
    case LocalAccountResultCode::unauthorized:return "Not saved: Sysop access is no longer available.";
    default:return "Save not confirmed. Your draft is retained; reopen and reconcile before repeating an uncertain change.";
    }
}
bool SysopCallerSession::text(std::string_view s){
    if(!p_.authorized())return false;
    const auto width=std::max(10U,p_.columns()>1?p_.columns()-1:79U);
    std::size_t offset{};unsigned used{};const auto height=std::max(1U,p_.rows()>5?p_.rows()-5:1U);
    while(offset<s.size()){
        const auto end=s.find('\n',offset);auto row=inert(s.substr(offset,end==s.npos?s.size()-offset:end-offset));
        do{const auto part=row.substr(0,width);if(!p_.write(system_menu_text(part,p_.ansi())+"\r\n"))return false;
            row.erase(0,part.size());if(++used>=height&&(!row.empty()||end!=s.npos)){if(!pause("sysop.more"))return false;used=0;}
        }while(!row.empty());
        if(end==s.npos)break;
        offset=end+1;
    }return true;
}
bool SysopCallerSession::screen(std::string_view title,const std::vector<std::pair<char,std::string>>& choices){
    if(!p_.authorized())return false;
    if(p_.terminal.clear_screen()!=TerminalOutcome::completed||!text(title)||!text(""))return false;
    for(const auto& item:choices){
        const auto prefix="["+std::string(1,item.first)+"] ";
        const auto dots=p_.ansi()?"\x1b[0;90;40m........\x1b[0;37;40m ":"........ ";
        if(!p_.write(system_menu_text(prefix,p_.ansi())+dots+inert(item.second)+"\r\n"))return false;
    }return true;
}
std::optional<char> SysopCallerSession::command(std::string_view keys,char fallback){
    for(;;){if(!p_.authorized())return {};
        if(p_.hotkeys()){
            auto r=p_.terminal.read_key({"sysop.command","Command: ",std::vector<char>(keys.begin(),keys.end()),fallback,false,true});
            if(r.outcome==TerminalOutcome::cancelled)return fallback;
            if(r.has_key())return *r.key;
            return {};
        }
        const auto r=p_.terminal.read_visible({"sysop.command","Command: ",1,EmptyInputPolicy::use_default,{},true});
        if(r.outcome==TerminalOutcome::cancelled||r.outcome==TerminalOutcome::empty)return fallback;
        if(r.has_value()&&keys.find(upper(r.value[0]))!=keys.npos)return upper(r.value[0]);
        if(r.outcome!=TerminalOutcome::invalid&&r.outcome!=TerminalOutcome::overlength&&r.outcome!=TerminalOutcome::completed)return {};
        if(!text("Choose one of the listed keys."))return {};
    }
}
std::optional<std::string> SysopCallerSession::line(std::string_view prompt,std::size_t maximum,std::string initial){
    input_cancelled_=false;
    if(!p_.authorized())return {};
    const auto r=p_.terminal.read_visible({"sysop.input",std::string(prompt),maximum,EmptyInputPolicy::use_default,std::move(initial),true});
    input_cancelled_=r.outcome==TerminalOutcome::cancelled;
    if(r.outcome==TerminalOutcome::empty||r.outcome==TerminalOutcome::cancelled)return std::string{};
    return r.has_value()?std::optional<std::string>{r.value}:std::nullopt;
}
bool SysopCallerSession::confirm(std::string_view question){return text(question)&&command("YN",'N')=='Y'&&p_.authorized();}
bool SysopCallerSession::pause(std::string_view identity){return p_.authorized()&&p_.write(system_menu_text("Press <ENTER> to continue.",p_.ansi()))&&p_.terminal.pause({std::string(identity)})==TerminalOutcome::completed;}
bool SysopCallerSession::ack(std::string_view s){return text(s)&&pause("sysop.ack");}
bool SysopCallerSession::run(){
    while(p_.authorized()){
        if(!p_.artwork||!p_.artwork())if(!screen("Spitfire-X Sysop Utilities",{{'C',"Callers"},{'M',"Messages"},{'F',"Files"},{'T',"FTN"},{'L',"Log Viewer"},{'N',"Edit SF Newsletter"},{'Q',"Return to Main Menu"}}))return false;
        const auto key=command("CMFTLNQ");if(!key)return false;if(*key=='Q')return true;
        bool ok=true;switch(*key){case 'C':ok=callers();break;case 'M':ok=p_.messages();break;case 'F':ok=files();break;case 'T':ok=ftn();break;case 'L':ok=logs();break;case 'N':ok=p_.newsletter?p_.newsletter():ack("Newsletter editor unavailable.");break;default:break;}
        if(!ok)return false;
    }return true;
}
bool SysopCallerSession::callers(){
    for(;;){if(!screen("Callers",{{'A',"Manage Accounts"},{'W',"Who's Online"},{'Q',"Back to Sysop Utilities"}}))return false;
        const auto k=command("AWQ");if(!k)return false;if(*k=='Q')return true;
        if(*k=='A'){if(!accounts())return false;}else if(!p_.online())return false;
    }
}
bool SysopCallerSession::accounts(){
    LocalAccountQuery query;
    for(;;){const auto page=p_.accounts(query);if(page.code!=LocalAccountResultCode::ok)return ack("Accounts unavailable or Sysop authority changed.");
        if(!screen("Manage Accounts",{}))return false;
        std::string list;for(std::size_t i=0;i<page.accounts.size();++i){const auto& a=page.accounts[i];list+=std::to_string(i+1)+". "+a.display_name.value_or("Deleted account")+" (#"+std::to_string(a.account_id)+") "+(a.state==AccountState::active?"Active":a.state==AccountState::locked?"Inactive":"Deleted")+"\n";}
        if(!text(list.empty()?"No matching accounts.":list)||!text("[F] Find  [N] New Caller  [P] Previous  [V] Next  [Q] Back"))return false;
        const auto input=line("Select row or command: ",30);if(!input)return false;if(input->empty()||upper((*input)[0])=='Q')return true;
        const auto key=upper((*input)[0]);
        if(key=='F'){const auto name=line("Name prefix or user number (Enter clears): ",30);if(!name)return false;query={};const auto id=number(*name);if(id&&*id>0&&*id<=INT64_MAX){auto found=p_.accounts({static_cast<AccountId>(*id)});if(found.code==LocalAccountResultCode::ok&&!found.accounts.empty()){if(!edit_account(std::move(found),false))return false;}else if(!ack("Account not found or unavailable."))return false;}else query.name_prefix=*name;continue;}
        if(key=='N'){if(!edit_account(page,true))return false;continue;}
        if(key=='P'){query.offset=query.offset>=kLocalAccountPageSize?query.offset-static_cast<std::uint32_t>(kLocalAccountPageSize):0;continue;}
        if(key=='V'){if(page.has_more)query.offset+=static_cast<std::uint32_t>(kLocalAccountPageSize);continue;}
        const auto selected=number(*input);if(!selected||!*selected||*selected>page.accounts.size())continue;
        auto detail=p_.accounts({page.accounts[static_cast<std::size_t>(*selected-1)].account_id});if(detail.code==LocalAccountResultCode::ok&&!detail.accounts.empty()){if(!edit_account(std::move(detail),false))return false;}else if(!ack("Selected account changed or is unavailable."))return false;
    }
}
bool SysopCallerSession::edit_account(LocalAccountSnapshot saved,bool creating){
    LocalAccountRequest draft;draft.action=creating?LocalAccountAction::create_user:LocalAccountAction::save_user;
    const auto load=[&]{if(creating){draft={};draft.action=LocalAccountAction::create_user;draft.expected_revision=1;draft.security_level=saved.policy.new_user_level;return;}const auto& a=saved.accounts.front();draft.account_id=a.account_id;draft.expected_revision=a.revision;draft.expected_policy_revision=saved.policy.revision;draft.name=a.display_name.value_or("");draft.profile=a.profile;draft.security_level=a.security_level.value_or(0);draft.purge_allowed=a.purge_allowed.value_or(false);};load();
    struct Erase{LocalAccountRequest& r;~Erase(){erase_local_account_secrets(r);}} erase{draft};
    bool dirty=creating,security=false,uncertain=false;
    const auto save=[&]()->bool{
        if(uncertain)return ack("Reopen the account and reconcile the previous result before another save.")&&false;
        if(creating){auto pass=p_.terminal.read_secret({"sysop.new.password","Initial password: ",kPasswordMaximum});auto confirm=p_.terminal.read_secret({"sysop.new.confirm","Confirm password: ",kPasswordMaximum});const bool valid=pass.has_value()&&confirm.has_value();draft.password=std::move(pass.value);draft.confirmation=std::move(confirm.value);if(!valid) {erase_local_account_secrets(draft);return false;}}
        auto result=p_.account_change(draft);erase_local_account_secrets(draft);text(sysop_account_result(result));
        if(result.committed){dirty=false;uncertain=result.code!=LocalAccountResultCode::ok;if(creating)return true;const auto current=p_.accounts({draft.account_id});if(current.code==LocalAccountResultCode::ok&&!current.accounts.empty()){saved=current;load();}else uncertain=true;return true;}
        uncertain=result.code==LocalAccountResultCode::storage_failure||result.code==LocalAccountResultCode::revocation_failed;return false;
    };
    for(;;){if(!screen(creating?"New Caller":("Caller: "+draft.name+" (#"+std::to_string(draft.account_id)+")"),{}))return false;
        if(!creating&&!text(std::string(saved.accounts.front().state==AccountState::active?"Active":saved.accounts.front().state==AccountState::locked?"Inactive":"Deleted")+" / "+(saved.online?"Online":"Offline")))return false;
        if(security){const auto fields=creating?"Security\nAssigned level: "+std::to_string(draft.security_level)+" (configured New User level)\nInitial password is requested on Save.\nSave the caller before changing access, purge or account state.\n[T] Personal":"Security\n[L] Assigned level: "+std::to_string(draft.security_level)+"\n[A] Automatic purge: "+(draft.purge_allowed?"Allowed":"Not allowed")+"\n[P] Issue temporary password reset\n[E] "+(saved.accounts.front().state==AccountState::locked?"Reactivate":"Deactivate")+"\n[D] Delete account\n[T] Personal";if(!text(fields))return false;}
        else if(!text("Personal\n[N] Name: "+draft.name+"\n[R] Real name: "+draft.profile.real_name.value_or("")+"\n[L] Location: "+draft.profile.location.value_or("")+"\n[P] Phone: "+draft.profile.phone.value_or("")+"\n[E] Email: "+draft.profile.email.value_or("")+"\n[B] Birth date: "+draft.profile.birth_date.value_or("")+"\n[X] Sex (M/F/X): "+(draft.profile.sex?std::string(1,*draft.profile.sex):"")+"\n[T] Security"))return false;
        if(!text(std::string(dirty?"Unsaved changes.\n":"")+"[S] Save  [O] Reload  [Q] Back"))return false;
        const auto key=command(security?(creating?"TSOQ":"LAPEDTSOQ"):"NRLPEBXTSOQ");if(!key)return false;
        if(*key=='Q'){if(!dirty)return true;if(!text("[S] Save and Continue  [D] Discard Changes  [C] Cancel Navigation"))return false;const auto leave=command("SDC",'C');if(!leave)return false;if(*leave=='D')return true;if(*leave=='S'&&save())return true;continue;}
        if(*key=='T'){security=!security;continue;}
        if(*key=='S'){if(save()&&creating)return ack("New caller created with the configured New User level.");if(!pause("sysop.save.result"))return false;continue;}
        if(*key=='O'){if(creating)continue;if(dirty&&!confirm("Discard this draft and reload the current saved account? [y/N]"))continue;auto current=p_.accounts({draft.account_id});if(current.code==LocalAccountResultCode::ok&&!current.accounts.empty()){saved=std::move(current);load();dirty=false;uncertain=false;}continue;}
        if(security){
            if(*key=='L'){if(creating){if(!ack("New callers use the configured New User level. Save first to assign another level."))return false;continue;}const auto v=line("Assigned level (1-999): ",3);if(!v)return false;if(input_cancelled_)continue;const auto n=number(*v);if(n&&*n>=1&&*n<=999){draft.security_level=static_cast<std::uint16_t>(*n);dirty=true;}}
            else if(*key=='A'){draft.purge_allowed=!draft.purge_allowed;dirty=true;}
            else if(!creating&&(*key=='E'||*key=='D'||*key=='P')){
                if(dirty){if(!ack("Save or discard profile changes before changing account state or password."))return false;continue;}
                auto request=draft;request.action=*key=='D'?LocalAccountAction::tombstone:*key=='P'?LocalAccountAction::issue_temporary_password:saved.accounts.front().state==AccountState::locked?LocalAccountAction::unlock:LocalAccountAction::lock;
                if(*key=='P'&&!text("The temporary password expires after 72 hours. The caller must choose a new password at the next login."))return false;
                if(!confirm((*key=='D'?"Delete ":*key=='P'?"Reset the password for ":request.action==LocalAccountAction::unlock?"Reactivate ":"Deactivate ")+draft.name+" (#"+std::to_string(draft.account_id)+")? Applicable sessions end; history is retained. [y/N]"))continue;
                if(*key=='P'){auto a=p_.terminal.read_secret({"sysop.reset.password","Temporary password: ",kPasswordMaximum});auto b=p_.terminal.read_secret({"sysop.reset.confirm","Confirm password: ",kPasswordMaximum});const bool valid=a.has_value()&&b.has_value();request.password=std::move(a.value);request.confirmation=std::move(b.value);if(!valid){erase_local_account_secrets(request);continue;}}
                const auto result=p_.account_change(request);erase_local_account_secrets(request);if(!ack(sysop_account_result(result)))return false;if(result.committed&&*key=='D')return true;auto current=p_.accounts({draft.account_id});if(current.code==LocalAccountResultCode::ok&&!current.accounts.empty()){saved=current;load();}else return true;
            }
        }else{
            if(*key=='N'){const auto v=line("Name: ",30,draft.name);if(!v)return false;if(input_cancelled_)continue;if(!v->empty()){draft.name=*v;dirty=true;}continue;}
            if(*key=='X'){const auto v=line("Sex M/F/X (blank clears): ",1);if(!v)return false;if(input_cancelled_)continue;if(v->empty())draft.profile.sex.reset();else if(upper((*v)[0])=='M'||upper((*v)[0])=='F'||upper((*v)[0])=='X')draft.profile.sex=upper((*v)[0]);else {if(!ack("Choose M, F or X."))return false;continue;}dirty=true;continue;}
            auto* field=*key=='R'?&draft.profile.real_name:*key=='L'?&draft.profile.location:*key=='P'?&draft.profile.phone:*key=='E'?&draft.profile.email:&draft.profile.birth_date;
            const auto v=line(*key=='B'?"Birth date YYYY-MM-DD (blank clears): ":"Value (blank clears): ",128,field->value_or(""));if(!v)return false;if(input_cancelled_)continue;*field=v->empty()?std::optional<std::string>{}:std::optional<std::string>{*v};dirty=true;
        }
    }
}
std::optional<std::string> SysopCallerSession::choose(std::string_view title,const std::vector<SysopNamedChoice>& choices){
    std::size_t offset{};const auto count=std::max(1U,p_.rows()>8?p_.rows()-8:1U);
    for(;;){if(!screen(title,{}))return {};std::string rows;for(std::size_t i=offset;i<std::min(choices.size(),offset+count);++i)rows+=std::to_string(i+1)+". "+choices[i].name+(choices[i].enabled?"":" (Unavailable)")+"\n";if(!text(rows.empty()?"No entries.":rows)||!text("[P] Previous  [N] Next  [Q] Back"))return {};
        const auto r=line("Select number: ",12);if(!r||r->empty()||upper((*r)[0])=='Q')return {};
        if(upper((*r)[0])=='P'){offset=offset>=count?offset-count:0;continue;}if(upper((*r)[0])=='N'){if(offset+count<choices.size())offset+=count;continue;}
        const auto n=number(*r);if(n&&*n>0&&*n<=choices.size()&&choices[static_cast<std::size_t>(*n-1)].enabled)return choices[static_cast<std::size_t>(*n-1)].key;
    }
}
std::optional<FileAreaId> SysopCallerSession::choose_area(const FileConfiguration& config){
    std::vector<SysopNamedChoice> groups;for(const auto& g:config.groups)groups.push_back({std::to_string(g.id.value),g.name,g.enabled});const auto group=choose("File Groups",groups);if(!group)return {};
    std::vector<SysopNamedChoice> areas;for(const auto& a:config.areas)if(std::to_string(a.area.group.value)==*group)areas.push_back({std::to_string(a.area.id.value),a.area.name,a.area.enabled});const auto area=choose("File Areas",areas);if(!area)return {};return FileAreaId{static_cast<std::int64_t>(*number(*area))};
}
FileManagementJobStatus SysopCallerSession::job(FileManagementTask task){
    if(pending_file_){auto result=pending_status_;result.code=LocalAccountResultCode::conflict;result.detail="Review the previous file job before starting another operation.";return result;}
    task.job=p_.identifier();if(task.kind==FileManagementTaskKind::scan_pending||task.kind==FileManagementTaskKind::scan_offline)task.target=task.job;auto result=p_.file_start(task);
    if(result.code!=LocalAccountResultCode::ok)return result;
    if(result.state==FileManagementJobState::running){text("Working. [Q] Cancel remaining work. Completed changes are retained.");
        std::uint64_t shown_seconds{};
        const bool waited=p_.wait([&]{result=p_.file_status(task.job);if(result.elapsed_millis/1000>shown_seconds){shown_seconds=result.elapsed_millis/1000;p_.write(system_menu_text("Current pass: "+std::to_string(result.bytes_done)+" / "+std::to_string(result.bytes_total)+" bytes. [Q] Cancel",p_.ansi())+"\r\n");}return result.code!=LocalAccountResultCode::ok||result.state!=FileManagementJobState::running;});
        if(!waited){result=p_.file_cancel(task.job);text("Cancellation requested. Completed changes are retained.");}
    }
    const bool mutation=task.kind==FileManagementTaskKind::operation||task.kind==FileManagementTaskKind::scan_pending||task.kind==FileManagementTaskKind::scan_offline;
    if(mutation&&(result.state==FileManagementJobState::running||result.state==FileManagementJobState::held||result.state==FileManagementJobState::failed||
        (result.journal_confirmed&&result.state!=FileManagementJobState::completed&&result.state!=FileManagementJobState::cancelled))){
        pending_file_=task;pending_status_=result;
    }
    return result;
}
bool SysopCallerSession::review_file_job(){
    while(pending_file_&&p_.authorized()){
        if(!screen("Previous File Job",{})||!text(pending_status_.detail)||
            !text(pending_file_->kind==FileManagementTaskKind::operation?"[S] Check Status  [R] Reconcile Same Operation  [Q] Back":"[S] Check Status  [R] Resume Scan  [Q] Back"))return false;
        const auto key=command("SRQ");if(!key)return false;if(*key=='Q')return true;
        auto result=p_.file_status(pending_status_.job);
        if(result.code!=LocalAccountResultCode::ok){if(!ack("Status unavailable. Original operation retained; do not repeat it as a new change."))return false;continue;}
        pending_status_=result;
        if(result.state==FileManagementJobState::completed||result.state==FileManagementJobState::cancelled||(!result.journal_confirmed&&result.state!=FileManagementJobState::running&&result.state!=FileManagementJobState::held&&result.state!=FileManagementJobState::failed)){
            pending_file_.reset();return ack(result.detail);
        }
        if(*key=='R'&&result.state!=FileManagementJobState::running&&pending_file_->kind==FileManagementTaskKind::operation){
            FileManagementTask retry;retry.kind=FileManagementTaskKind::reconcile_operation;retry.target=pending_file_->operation.operation;retry.job=p_.identifier();
            result=p_.file_start(retry);if(result.code==LocalAccountResultCode::ok)pending_status_=result;
            else if(!ack("Reconciliation unavailable. The original operation and recovery history remain retained."))return false;
        }else if(*key=='R'&&result.state!=FileManagementJobState::running){
            auto retry=*pending_file_;retry.job=p_.identifier();result=p_.file_start(retry);
            if(result.code==LocalAccountResultCode::ok){pending_file_=retry;pending_status_=result;}
            else if(!ack("Scan could not resume. Completed changes remain retained."))return false;
        }
    }return true;
}
bool SysopCallerSession::files(){
    for(;;){std::vector<std::pair<char,std::string>> choices{{'B',"Browse"},{'F',"Search Filenames"},{'P',"Search Pending Files"},{'O',"Search Offline Files"}};if(pending_file_)choices.push_back({'J',"Review Previous File Job"});choices.push_back({'Q',"Back to Sysop Utilities"});if(!screen("Files",choices))return false;
        const auto k=command(pending_file_?"BFPOJQ":"BFPOQ");if(!k)return false;if(*k=='Q')return true;if(*k=='J'){if(!review_file_job())return false;continue;}
        if(*k=='B'||*k=='F'){FileManagementQuery query;query.state=FileCatalogState::available;
            if(*k=='B'){const auto config=p_.file_configuration();if(!config){if(!ack("File configuration unavailable."))return false;continue;}const auto area=choose_area(*config);if(!area)continue;query.area=*area;}
            else{const auto pattern=line("Filename or wildcard (* / ?): ",128);if(!pattern)return false;if(pattern->empty())continue;query.field=FileManagementSearch::filename;query.search=*pattern;}
            if(!file_list(query,*k=='F'))return false;
        }else{FileManagementTask task;task.kind=*k=='P'?FileManagementTaskKind::scan_pending:FileManagementTaskKind::scan_offline;const auto r=job(task);if(!ack(r.detail.empty()?"Scan unavailable; no result is claimed.":r.detail))return false;
            if(r.report)for(std::uint16_t page=0;page<r.report->pages;++page){FileManagementTask report;report.kind=FileManagementTaskKind::scan_report;report.target=r.job;report.view_page=page;const auto part=job(report);if(!ack(part.detail))break;}
        }
    }
}
bool SysopCallerSession::file_list(FileManagementQuery query,bool search){
    FileId after;std::vector<FileId> history;std::map<std::int64_t,ManagedFile> selected;
    for(;;){FileManagementReadRequest request;request.filter=query;request.after=after;const auto page=p_.files(request);if(page.code!=LocalAccountResultCode::ok)return ack("File list unavailable or changed.");
        if(!screen(search?"Filename Results":"Browse Files",{}))return false;
        std::string rows;for(std::size_t i=0;i<page.records.size();++i){const auto& f=page.records[i].file;rows+=std::to_string(i+1)+". "+(selected.count(f.id.value)?"* ":"  ")+f.name+" ("+std::to_string(f.bytes)+" bytes)\n";}
        if(!text(rows.empty()?"No files.":rows)||!text("Selected: "+std::to_string(selected.size())+"\n[S] Select row  [A] Select All  [C] Clear selection\n[D] Details  [Z] ZIP Contents  "+std::string(search?"[M] Move":"[R] Rename")+"\n[X] Remove selected  [P] Previous  [N] Next  [Q] Back"))return false;
        const auto k=command(search?"SACDZMXPNQ":"SACDZRXPNQ");if(!k)return false;if(*k=='Q')return true;
        if(*k=='P'){if(!history.empty()){after=history.back();history.pop_back();}continue;}if(*k=='N'){if(page.next.value){history.push_back(after);after=page.next;}continue;}if(*k=='C'){selected.clear();continue;}
        if(*k=='A'){
            if(!text("Selecting all matching files, including off-screen records. [Q] Cancel."))return false;
            const auto all=p_.select_all_files(query);if(all){selected.clear();for(const auto& f:*all)selected.emplace(f.file.id.value,f);}
            else if(!ack("Select All did not complete. Previous selection retained."))return false;
            continue;
        }
        if(*k=='X'){if(selected.empty())continue;std::string names;for(const auto& f:selected)names+=f.second.file.name+" ("+std::to_string(f.second.file.bytes)+" bytes)\n";
            if(!text(names)||!confirm("Permanently delete these "+std::to_string(selected.size())+" selected files from disk and listings? History is retained. [y/N]"))continue;
            std::size_t deleted{},attempted{};for(const auto& f:selected){if(!p_.authorized())break;FileManagementTask task;task.operation={};task.operation.operation=p_.identifier();task.operation.file=f.second.file.id;task.operation.expected_revision=f.second.file.revision;task.operation.action=FileOperationAction::purge;const auto result=job(task);++attempted;if(result.code==LocalAccountResultCode::ok&&result.state==FileManagementJobState::completed)++deleted;else{if(!text(f.second.file.name+": "+result.detail))return false;if(pending_file_||result.cancellation_requested)break;}}
            if(!ack("Deleted: "+std::to_string(deleted)+". Not confirmed: "+std::to_string(attempted-deleted)+". Not attempted: "+std::to_string(selected.size()-attempted)+"."))return false;
            selected.clear();continue;
        }
        const auto row=line("Row number: ",12);if(!row)return false;const auto n=number(*row);if(!n||!*n||*n>page.records.size())continue;const auto f=page.records[static_cast<std::size_t>(*n-1)];
        if(*k=='S'){if(selected.count(f.file.id.value))selected.erase(f.file.id.value);else selected.emplace(f.file.id.value,f);continue;}
        if(*k=='D'){FileManagementReadRequest detail;detail.kind=FileManagementQueryKind::detail;detail.selection=f.file.id;const auto r=p_.files(detail);if(r.code!=LocalAccountResultCode::ok||r.records.empty()){if(!ack("File details unavailable."))return false;}else {const auto& a=r.records.front();if(!ack(a.file.name+"\nBytes: "+std::to_string(a.file.bytes)+"\nUploader: "+a.file.public_uploader+"\n"+a.file.descriptions.short_text+"\n"+a.file.descriptions.long_text))return false;}continue;}
        if(*k=='Z'){if(!file_view(f))return false;continue;}
        FileManagementTask task;task.operation.operation=p_.identifier();task.operation.file=f.file.id;task.operation.expected_revision=f.file.revision;
        if(*k=='R'){const auto name=line("New filename: ",128,f.file.name);if(!name)return false;if(name->empty()||*name==f.file.name)continue;const auto config=p_.file_configuration();if(!config)continue;const auto area=std::find_if(config->areas.begin(),config->areas.end(),[&](const auto& a){return a.area.id==f.file.area;});if(area==config->areas.end())continue;task.operation.action=FileOperationAction::rename;task.operation.destination=f.file.area;task.operation.expected_destination_revision=area->area.revision;task.operation.filename=*name;}
        else{const auto config=p_.file_configuration();if(!config)continue;const auto target=choose_area(*config);if(!target)continue;const auto area=std::find_if(config->areas.begin(),config->areas.end(),[&](const auto& a){return a.area.id==*target;});if(area==config->areas.end())continue;task.operation.action=FileOperationAction::move;task.operation.destination=*target;task.operation.expected_destination_revision=area->area.revision;task.operation.filename=f.file.name;}
        const auto result=job(task);if(!ack(result.detail.empty()?"Operation unavailable; refresh before retrying.":result.detail))return false;selected.erase(f.file.id.value);
    }
}
bool SysopCallerSession::file_view(const ManagedFile& file){
    FileManagementTask task;task.kind=FileManagementTaskKind::open_view;task.view_file=file.file.id;task.view_archive=true;auto r=job(task);
    for(;;){if(!r.view)return ack(r.detail.empty()?"ZIP preview unavailable.":r.detail);const auto v=*r.view;
        if(!screen(v.filename+(v.member.empty()?"":" / "+v.member),{}))return false;
        std::string rows;
        if(v.archive){for(const auto& entry:v.entries)rows+=std::to_string(entry.index+1)+". "+entry.name+" ("+std::to_string(entry.bytes)+" bytes)"+(entry.readable?"":" - preview unavailable")+"\n";}
        else{for(std::size_t i=0;i<v.cells.size();++i){const auto c=v.cells[i].glyph;rows+=(c>=32&&c!=127)?static_cast<char>(c):' ';if((i+1)%kFileViewColumns==0)rows+='\n';}}
        if(!text(rows)||!text("[P] Previous  [N] Next  [M] Member  [I] ZIP Index  [Q] Back"))return false;
        const auto k=command("PNMIQ");if(!k)return false;
        task={};task.target=v.cursor;if(*k=='Q'){task.kind=FileManagementTaskKind::close_view;job(task);return true;}
        if(*k=='M'){const auto input=line("Member number: ",5);if(!input)return false;const auto n=number(*input);if(!n||!*n||*n>10000)continue;task.kind=FileManagementTaskKind::read_view_member;task.view_member=static_cast<std::uint16_t>(*n-1);}
        else if(*k=='I'){task.kind=FileManagementTaskKind::view_archive_index;}
        else{task.kind=v.archive?FileManagementTaskKind::view_archive_index:FileManagementTaskKind::page_view;if(*k=='P'){if(!v.page)continue;task.view_page=v.page-1;}else{const auto size=v.archive?kFileViewArchivePage:kFileViewRows;if((v.page+1)*size>=v.total)continue;task.view_page=v.page+1;}}
        r=job(task);
    }
}
bool SysopCallerSession::show_page(const SysopTextPage& p){if(!screen(p.title,{}))return false;std::string rows;for(const auto& row:p.lines)rows+=row+"\n";return text(rows)&&text(p.notice)&&text("[P] Previous Page  [N] Next Page  [R] Refresh  [Q] Back");}
bool SysopCallerSession::show_log_page(const SysopTextPage& page){
    if(!p_.authorized()||p_.terminal.clear_screen()!=TerminalOutcome::completed)return false;
    const auto write_rows=[&](const std::vector<std::string>& lines){
        for(const auto& line:lines)if(!p_.authorized()||!p_.write(system_menu_text(inert(line),p_.ansi())+"\r\n"))return false;
        return true;
    };
    return write_rows(wrap_sysop_log_text(page.title.empty()?"Log Viewer":page.title,p_.columns(),false))&&
        write_rows({""})&&write_rows(page.lines)&&
        write_rows(wrap_sysop_log_text(page.notice,p_.columns(),false))&&
        write_rows(wrap_sysop_log_text(kSysopLogControls,p_.columns(),false));
}
bool SysopCallerSession::logs(){
    for(;;){if(!screen("Log Viewer",{{'H',"HEYSYSOP.LOG"},{'C',"CALLERS.LOG"},{'B',"bink.log"},{'F',"freshclam.log"},{'A',"BotGate Activity"},{'Q',"Back to Sysop Utilities"}}))return false;const auto k=command("HCBFAQ");if(!k)return false;if(*k=='Q')return true;
        const auto source=*k=='H'?SysopLog::heysysop:*k=='C'?SysopLog::callers:*k=='B'?SysopLog::bink:*k=='F'?SysopLog::freshclam:SysopLog::botgate;std::optional<std::uint64_t> page;
        auto columns=p_.columns(),rows=p_.rows();
        for(;;){
            if(columns!=p_.columns()||rows!=p_.rows()){page.reset();columns=p_.columns();rows=p_.rows();}
            const auto view=p_.log(source,page);
            if(columns!=p_.columns()||rows!=p_.rows())continue;
            if(!show_log_page(view))return false;
            const auto key=command("PNRQ");if(!key)return false;if(*key=='Q')break;
            if(*key=='R')page.reset();else if(*key=='P'&&view.previous)page=view.page-1;else if(*key=='N'&&view.next)page=view.page+1;else page=view.page;
        }
    }
}
bool SysopCallerSession::ftn(){
    for(;;){const auto networks=p_.networks();const auto network=choose("FTN Networks",networks);if(!network)return true;
        const auto selected=std::find_if(networks.begin(),networks.end(),[&](const auto& item){return item.key==*network;});
        const auto title="FTN - "+(selected==networks.end()?std::string("Selected Network"):selected->name);
        for(;;){if(!screen(title,{{'P',"Poll Now"},{'Q',"View Queues"},{'H',"View Held Mail"},{'A',"BinkP Activity"},{'B',"Back to Networks"}}))return false;const auto k=command("PQHAB",'B');if(!k)return false;if(*k=='B')break;
            if(*k=='P'){const auto link=choose("Poll Now - select link",p_.links(*network));if(link&&!ack(p_.poll(*network,*link)))return false;continue;}
            const auto kind=*k=='Q'?SysopFtnView::queues:*k=='H'?SysopFtnView::held:SysopFtnView::activity;std::uint64_t page{};
            for(;;){const auto view=p_.ftn_view(*network,kind,page);if(!show_page(view))return false;const auto key=command("PNRQ");if(!key)return false;if(*key=='Q')break;if(*key=='R')page=0;else if(*key=='P'&&view.previous)page=view.page-1;else if(*key=='N'&&view.next)page=view.page+1;}
        }
    }
}
} // namespace sfx::application
