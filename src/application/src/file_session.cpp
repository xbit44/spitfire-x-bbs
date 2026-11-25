#include "sfx/application/file_session.hpp"
#include "sfx/application/qwk.hpp"
#include <algorithm>
#include <charconv>
#include <set>

namespace sfx::application {
namespace {
unsigned positive(std::string_view text){unsigned n{};const auto r=std::from_chars(text.data(),text.data()+text.size(),n);return r.ec==std::errc{}&&r.ptr==text.data()+text.size()?n:0;}
std::string column(std::string_view value,std::size_t width){auto text=file_terminal_text(value,width);std::replace(text.begin(),text.end(),'\n',' ');text.resize(width,' ');return qwk_cp437_to_utf8(text);}
bool same_actor(const FileActor& a,const FileActor& b){return a.session==b.session&&a.account==b.account&&a.account_revision==b.account_revision&&a.policy_revision==b.policy_revision;}
}
FileCallerSession::FileCallerSession(FileSessionPorts p):p_(std::move(p)){}
const FileArea* FileCallerSession::area(FileAreaId id)const{const auto at=std::find_if(catalog_.areas.begin(),catalog_.areas.end(),[id](const auto& a){return a.id==id;});return at==catalog_.areas.end()?nullptr:&*at;}
const FileGroup* FileCallerSession::group(FileGroupId id)const{const auto at=std::find_if(catalog_.groups.begin(),catalog_.groups.end(),[id](const auto& g){return g.id==id;});return at==catalog_.groups.end()?nullptr:&*at;}
bool FileCallerSession::refresh(){
    if(!p_.actor||!p_.authority||!p_.list.live||!p_.list.live())return false;
    const auto actor=p_.actor();if(!actor||!p_.authority().valid())return false;
    const auto catalog=p_.storage.file_catalog(*actor);const auto state=p_.storage.file_caller_state(*actor);
    if(!catalog||!catalog.value||!state||!state.value)return false;
    catalog_=*catalog.value;state_=*state.value;
    if(!area(current_)){
        current_=area(state_.current_area)?state_.current_area:catalog_.areas.empty()?FileAreaId{}:catalog_.areas.front().id;
        if(current_.value&&!(current_==state_.current_area)){
            auto desired=state_;desired.current_area=current_;const auto saved=p_.storage.save_file_caller_state(*actor,desired);
            if(!saved||!saved.value)return false;
            state_=*saved.value;
        }
    }
    return true;
}
std::optional<FileSessionContext> FileCallerSession::context(){
    if(!refresh())return {};
    const auto a=area(current_);const auto g=a?group(a->group):nullptr;
    return FileSessionContext{g?g->name:"None",a?a->name:"None",a?a->number:std::uint16_t{},a?a->id:FileAreaId{}};
}
bool FileCallerSession::say(std::string_view text){
    const auto size=p_.list.dimensions?p_.list.dimensions():MessageEditorDimensions{};
    if(size.ansi&&p_.list.write&&!p_.list.write("\x1b[0;37;40m"))return false;
    return present_file_text(p_.list.terminal,"files.text",text,size.columns,size.rows);
}
bool FileCallerSession::options(const std::vector<std::pair<char,std::string>>& rows){
    const auto size=p_.list.dimensions?p_.list.dimensions():MessageEditorDimensions{};
    if(size.ansi&&p_.list.write){std::string text;for(const auto& row:rows)text+=file_option(row.first,row.second,true)+"\r\n";return p_.list.write(text);}
    std::string text;for(const auto& row:rows)text+=file_option(row.first,row.second,false)+"\n";return say(text);
}
std::optional<char> FileCallerSession::key(std::string identity,std::string prompt,std::vector<char> allowed,char fallback,bool advance_line){
    if(!p_.list.live())return {};
    TerminalKeyRequest request{std::move(identity),std::move(prompt),std::move(allowed),fallback?std::optional<char>{fallback}:std::nullopt,false,true};
    request.advance_line=advance_line;
    const auto result=p_.list.terminal.read_key(request);
    if(result.outcome==TerminalOutcome::cancelled)return 'Q';
    if(!result.has_key()||!p_.list.live())return {};
    return result.key;
}
std::optional<bool> FileCallerSession::yes(std::string identity,std::string prompt,bool default_yes){const auto choice=key(std::move(identity),std::move(prompt),{'Y','N'},default_yes?'Y':'N');if(!choice)return {};return *choice=='Y';}
std::optional<std::string> FileCallerSession::text(std::string identity,std::string prompt,std::size_t maximum){
    if(!p_.list.live())return {};
    const auto result=p_.list.terminal.read_visible({std::move(identity),std::move(prompt),maximum,EmptyInputPolicy::use_default,{},true});
    if(result.outcome==TerminalOutcome::empty||result.outcome==TerminalOutcome::cancelled)return std::string{};
    if(!result.has_value()||!p_.list.live())return {};
    return qwk_cp437_to_utf8(result.value);
}
std::optional<bool> FileCallerSession::change_area(){
    if(!refresh())return {};
    if(catalog_.areas.empty()){if(!say("No accessible File Areas."))return {};return false;}
    std::optional<FileArea> selected;
    {
        std::vector<FileGroup> groups;for(const auto& g:catalog_.groups)if(std::any_of(catalog_.areas.begin(),catalog_.areas.end(),[&](const auto& a){return a.group==g.id;}))groups.push_back(g);
        std::optional<FileGroup> chosen_group;if(groups.size()==1)chosen_group=groups.front();
        while(!selected){
            if(!refresh())return {};
            auto list=p_.list;list.lightbars=list.lightbars&&state_.lightbar;list.emitted={};
            bool custom{};if(p_.selection_artwork){const auto shown=p_.selection_artwork(!chosen_group);if(shown){if(!*shown)return {};custom=true;}}if(custom)list.lightbars=false;
            FileListPage page;page.title=chosen_group?"File Areas":"File Groups";page.context=chosen_group?chosen_group->name:"Choose a File Group";
            if(!chosen_group){for(std::size_t i=0;i<groups.size();++i)page.rows.push_back({static_cast<unsigned>(i+1),groups[i].id.value,groups[i].revision,groups[i].name});}
            else for(const auto& a:catalog_.areas)if(a.group==chosen_group->id)page.rows.push_back({a.number,a.id.value,a.revision,a.name});
            // Groups have no selected maximum; bound the visible selector page.
            std::size_t offset{},row{};FileListChoice choice;
            for(;;){auto slice=page;slice.rows.assign(page.rows.begin()+static_cast<std::ptrdiff_t>(offset),page.rows.begin()+static_cast<std::ptrdiff_t>(std::min(offset+kFilePageMaximum,page.rows.size())));slice.previous=offset>0;slice.next=offset+kFilePageMaximum<page.rows.size();choice=choose_file_row(list,slice,row);if(choice.action==FileListAction::next){offset+=kFilePageMaximum;row=0;}else if(choice.action==FileListAction::previous){offset-=kFilePageMaximum;row=0;}else break;}
            if(choice.action==FileListAction::failed)return {};
            if(choice.action==FileListAction::cancel){if(chosen_group&&groups.size()>1){chosen_group.reset();continue;}return false;}
            if(!refresh())return {};
            if(!chosen_group){const auto g=group({choice.identity});if(!g||g->revision!=choice.revision){if(!say("File Groups changed. Choose again."))return {};return false;}chosen_group=*g;}
            else {const auto a=area({choice.identity});if(!a||!(a->group==chosen_group->id)||a->revision!=choice.revision){if(!say("File Areas changed. Choose again."))return {};return false;}selected=*a;}
        }
    }
    if(!refresh())return {};
    const auto current=area(selected->id);if(!current||current->revision!=selected->revision){if(!say("File Area changed; choose again."))return {};return false;}
    const auto actor=p_.actor();if(!actor)return {};auto desired=state_;desired.current_area=selected->id;
    const auto saved=p_.storage.save_file_caller_state(*actor,desired);if(!saved||!saved.value){if(!say("File Area not changed. Please try again."))return {};return false;}
    current_=selected->id;state_=*saved.value;const auto list=yes("files.area.list","List files in this File Area? [Y/n] ",true);if(!list)return {};
    if(*list){FileQuery q;q.area=current_;if(!browse(q))return {};}return true;
}
bool FileCallerSession::tag(FileId id,std::int64_t revision){
    if(!refresh())return false;
    const auto actor=p_.actor();if(!actor)return false;const auto file=p_.storage.file_information(*actor,id);
    if(!file||!file.value||file.value->revision!=revision)return say("File changed or unavailable. Nothing tagged.");
    const auto a=area(file.value->area);const auto g=a?group(a->group):nullptr;
    if(!a||!g||!file_area_downloadable(*g,*a,p_.authority()))return say("This file is not available for download.");
    return say(downloads_.add(id)?"File added to download queue.":"Already queued, or the download queue is full.");
}
bool FileCallerSession::information(FileId id){
    std::string feedback;
    for(;;){
        if(!refresh())return false;
        const auto actor=p_.actor();if(!actor)return false;const auto found=p_.storage.file_information(*actor,id);
        if(!found||!found.value)return say("File unavailable.");
        const auto file=*found.value;const auto a=area(file.area);const auto g=a?group(a->group):nullptr;if(!a||!g)return say("File unavailable.");
        const auto date=[&](std::int64_t when){return p_.date?p_.date(when):std::to_string(when);};
        const bool editable=file_description_editable(*g,*a,file,p_.authority(),actor->account);
        auto added=date(file.published_utc);if(added.size()>=19&&added[10]==' ')added.resize(10);
        const auto dimensions=p_.list.dimensions?p_.list.dimensions():MessageEditorDimensions{};
        if(!present_file_information(p_.list.terminal,{file.name,g->name+" / "+std::to_string(a->number)+" "+a->name,
            added,file.public_uploader,file.descriptions.long_text.empty()?file.descriptions.short_text:file.descriptions.long_text,
            file.bytes,file.downloads,file_area_downloadable(*g,*a,p_.authority()),editable,feedback},dimensions))return false;
        auto allowed=std::vector<char>{'T','R','V','Q'};if(editable)allowed.push_back('E');const auto choice=key("files.info.choice"," ",allowed,'Q');if(!choice)return false;
        if(*choice=='Q')return true;
        if(*choice=='T'){if(!tag(id,file.revision))return false;
            feedback=std::find(downloads_.items().begin(),downloads_.items().end(),id)!=downloads_.items().end()
                ?"File is in the download queue.":"File was not queued. It may have changed or become unavailable.";
        }
        else if(*choice=='R'||*choice=='V'){if(!p_.view){if(!say("File viewer unavailable."))return false;}else if(!p_.view(id,*choice=='V'))return false;}
        else if(*choice=='E'){
            const auto edited=descriptions(file.descriptions,false,nullptr);if(!edited)continue;
            const auto current_actor=p_.actor();if(!current_actor)return false;const auto saved=p_.storage.edit_own_file_description(*current_actor,p_.operation(),id,file.revision,*edited);
            feedback=saved?"Description saved.":"Description not saved. Permission or file may have changed.";
        }
    }
}
bool FileCallerSession::browse(FileQuery query,std::vector<FileAreaId> scan_areas){
    std::vector<FileQueryCursor> history;FileQueryCursor cursor;std::size_t selected{};std::string feedback;std::optional<FileId> return_selection;
    // A fixed publication fence plus keyset pages bounds a live-growing library.
    for(;;){
        if(!refresh())return false;
        const auto actor=p_.actor();if(!actor)return false;query.sort=sort_;
        const auto dimensions=p_.list.dimensions?p_.list.dimensions():MessageEditorDimensions{};
        const bool lightbar=p_.list.lightbars&&state_.lightbar&&(!p_.list.lightbars_enabled||p_.list.lightbars_enabled());
        const auto limit=lightbar&&dimensions.ansi&&dimensions.columns>=80&&dimensions.rows>=24?std::clamp(dimensions.rows,24U,50U)-5:20U;
        const auto result=p_.storage.file_page(*actor,query,cursor,limit);if(!result||!result.value)return say("Files unavailable or selection changed. Please try again.");
        const auto page=*result.value;query.publication_fence=page.publication_fence;
        if(return_selection){
            const auto at=std::find_if(page.entries.begin(),page.entries.end(),[&](const auto& file){return file.id==*return_selection;});
            if(at!=page.entries.end())selected=static_cast<std::size_t>(at-page.entries.begin());
            else feedback="File changed or unavailable; list refreshed.";
            return_selection.reset();
        }
        FileListPage display;display.numbered=false;display.previous=!history.empty();display.next=page.next.has_value();display.tag=display.sort=display.jump=true;display.notice=feedback;
        if(query.scope==FileSearchScope::area){const auto a=area(query.area);const auto g=a?group(a->group):nullptr;if(!a||!g)return say("File Area unavailable.");if(!a->permissions.uploads_enabled)display.notice="Read/Download Only. "+feedback;}
        // Common 8.3 names use the owner's compact columns. Longer names keep
        // the former 24-cell allowance; unusually large sizes never collide.
        std::size_t name_width=16,size_width=6;
        for(const auto& file:page.entries){name_width=std::max(name_width,file_terminal_text(file.name,24).size());size_width=std::max(size_width,file_size_label(file.bytes).size());}
        display.columns="  "+column("Filename",name_width)+column("Size",size_width)+"   Description";
        for(std::size_t i=0;i<page.entries.size();++i){const auto& file=page.entries[i];const auto a=area(file.area);const auto g=a?group(a->group):nullptr;if(!a||!g)return false;
            const bool queued=std::find(downloads_.items().begin(),downloads_.items().end(),file.id)!=downloads_.items().end();
            const auto size=file_size_label(file.bytes);auto label=std::string(queued?"* ":"  ")+column(file.name,name_width)+std::string(size_width-size.size(),' ')+size+"   "+file.descriptions.short_text;
            display.rows.push_back({static_cast<unsigned>(i+1),file.id.value,file.revision,std::move(label),file.name});
        }
        auto list=p_.list;list.lightbars=list.lightbars&&state_.lightbar;
        const auto shown_actor=*actor;const auto prior_live=list.live;list.live=[&,prior_live,shown_actor]{const auto current=p_.actor();return prior_live()&&current&&same_actor(*current,shown_actor);};
        bool acknowledgement_failed{};
        if(query.new_only)list.emitted=[&,shown_actor](const std::vector<FileListRow>& rows){std::vector<FileScanSeen> seen;for(const auto& row:rows){const auto it=std::find_if(page.entries.begin(),page.entries.end(),[&](const auto& f){return f.id.value==row.identity&&f.revision==row.revision;});if(it==page.entries.end()){acknowledgement_failed=true;return false;}seen.push_back({it->id,it->publication_sequence});}const auto status=p_.storage.acknowledge_file_scan(shown_actor,seen);acknowledgement_failed=!status.succeeded();return status.succeeded();};
        else list.emitted={};
        const auto choice=choose_file_row(list,display,selected);if(choice.action==FileListAction::failed)return acknowledgement_failed&&p_.list.live()?say("Scan progress could not be saved. Unseen files remain for your next scan."):false;
        if(choice.action==FileListAction::cancel){
            // Completion is storage-proven: it refuses a scope with ANY unseen
            // publication. Cancelling early therefore never skips those files.
            if(query.new_only&&!scan_areas.empty()){const auto live=p_.actor();if(live)(void)p_.storage.complete_file_scan(*live,scan_areas,query.publication_fence,query.since_utc);}return true;
        }
        if(choice.action==FileListAction::next){if(page.next){if(history.size()==512)history.erase(history.begin());history.push_back(cursor);cursor=*page.next;selected=0;}continue;}
        if(choice.action==FileListAction::previous){if(!history.empty()){cursor=history.back();history.pop_back();selected=0;}continue;}
        if(choice.action==FileListAction::sort){if(!options({{'F',"Filename"},{'N',"Newest"},{'O',"Oldest"},{'S',"Size"},{'D',"Downloads"},{'Q',"Cancel"}}))return false;const auto sort=key("files.sort","Sort [F N O S D Q]? ",{'F','N','O','S','D','Q'},'Q');if(!sort)return false;if(*sort!='Q'){sort_=*sort=='N'?FileSort::newest:*sort=='O'?FileSort::oldest:*sort=='S'?FileSort::size:*sort=='D'?FileSort::downloads:FileSort::filename;cursor={};history.clear();selected=0;}continue;}
        if(choice.action==FileListAction::jump){const auto term=text("files.list.find","Filename or wildcard (Enter cancels): ",128);if(!term)return false;if(!term->empty()){FileQuery find=query;find.search=FileSearchKind::filename;find.query=*term;find.new_only=find.unseen_only=find.selected_only=false;find.after_publication=0;if(!browse(find))return false;}continue;}
        if(choice.action==FileListAction::tag){if(!tag({choice.identity},choice.revision))return false;feedback="Download queue: "+std::to_string(downloads_.items().size())+" file(s).";}
        else if(choice.action==FileListAction::open){const auto now=p_.actor();if(!now)return false;const auto file=p_.storage.file_information(*now,{choice.identity});if(!file||!file.value||file.value->revision!=choice.revision){feedback="File changed or unavailable; list refreshed.";continue;}if(!information(file.value->id))return false;return_selection=file.value->id;}
    }
}
bool FileCallerSession::search(FileSearchKind kind){
    const auto term=text("files.search",kind==FileSearchKind::filename?"Filename / partial / wildcard (Enter cancels): ":"Up to six words or phrases, separated by semicolons: ",kind==FileSearchKind::filename?128:512);if(!term)return false;if(term->empty())return true;
    if(kind==FileSearchKind::descriptions&&!file_description_terms(*term))return say("Enter one to six nonempty words or phrases.");
    if(!options({{'C',"Current File Area"},{'G',"Current File Group"},{'A',"All Accessible File Areas"},{'Q',"Cancel"}}))return false;
    const auto scope=key("files.search.scope","Search [C G A Q]? ",{'C','G','A','Q'},'C');if(!scope)return false;if(*scope=='Q')return true;
    if(!refresh())return false;
    const auto current=area(current_);if(!current)return say("No accessible File Area.");FileQuery q;q.search=kind;q.query=*term;q.area=current_;q.group=current->group;q.scope=*scope=='A'?FileSearchScope::all:*scope=='G'?FileSearchScope::group:FileSearchScope::area;return browse(q);
}
std::optional<FileId> FileCallerSession::choose_exact(std::string_view prompt,bool downloadable){
    const auto name=text("files.choose.name",std::string(prompt),128);if(!name||name->empty())return {};
    if(!refresh())return {};
    const auto actor=p_.actor();if(!actor)return {};FileQuery q;q.scope=downloadable?FileSearchScope::all:FileSearchScope::area;q.area=current_;q.search=FileSearchKind::filename;q.query=*name;q.downloadable_only=downloadable;
    FileQueryCursor cursor;std::vector<FileQueryCursor> history;std::size_t row{};
    for(;;){const auto current_actor=p_.actor();if(!current_actor)return {};const auto result=p_.storage.file_page(*current_actor,q,cursor,20);if(!result||!result.value){say("File unavailable.");return {};}
        q.publication_fence=result.value->publication_fence;FileListPage page;page.title="Select File";page.previous=!history.empty();page.next=result.value->next.has_value();
        page.numbered=false;page.columns="Filename                      File Group       File Area";
        for(std::size_t i=0;i<result.value->entries.size();++i){const auto& f=result.value->entries[i];const auto a=area(f.area);const auto g=a?group(a->group):nullptr;if(a&&g)page.rows.push_back({static_cast<unsigned>(i+1),f.id.value,f.revision,column(f.name,30)+column(g->name,16)+"#"+std::to_string(a->number)+" "+a->name,f.name});}
        if(page.rows.empty()){say("No matching file available.");return {};}
        // Even one match is explicit; duplicate filenames in different areas
        // never silently select a different physical destination.
        auto list=p_.list;list.lightbars=list.lightbars&&state_.lightbar;list.emitted={};const auto choice=choose_file_row(list,page,row);
        if(choice.action==FileListAction::next){if(result.value->next){history.push_back(cursor);cursor=*result.value->next;row=0;}continue;}
        if(choice.action==FileListAction::previous){if(!history.empty()){cursor=history.back();history.pop_back();row=0;}continue;}
        if(choice.action!=FileListAction::open)return {};
        const auto live=p_.actor();if(!live)return {};const auto selected=p_.storage.file_information(*live,{choice.identity});if(!selected||!selected.value||selected.value->revision!=choice.revision){say("File changed or unavailable.");return {};}return selected.value->id;
    }
}
bool FileCallerSession::new_scan(bool login){
    if(!refresh())return false;
    if(login&&!state_.new_file_scan)return true;
    const auto a=area(current_);if(!a)return login||say("No accessible File Areas.");
    FileQuery query;query.new_only=true;query.unseen_only=true;query.since_utc=state_.since_utc;query.scope=FileSearchScope::all;query.selected_only=login;
    if(!login){
        if(!say("New File Scan\nLast completed: "+(state_.scan_completed_utc?(p_.date?p_.date(*state_.scan_completed_utc):std::to_string(*state_.scan_completed_utc)):"Never")+"\nSaved scan date: "+(p_.date?p_.date(state_.since_utc):std::to_string(state_.since_utc))))return false;
        const auto earlier=text("files.scan.date","Earlier date YYYY-MM-DD, or Enter for saved date: ",10);if(!earlier)return false;
        if(!earlier->empty()){
            const auto parsed=p_.scan_date?p_.scan_date(*earlier):std::nullopt;
            if(!parsed||*parsed>state_.since_utc)return say("Enter a valid date no later than the saved scan date.");
            query.since_utc=*parsed;query.unseen_only=false;
        }
        if(!options({{'C',"Current File Area"},{'G',"Current File Group"},{'S',"My Selected Scan Areas"},{'A',"All Accessible File Areas"},{'Q',"Cancel"}}))return false;
        const auto scope=key("files.scan.scope","Scan [C G S A Q]? ",{'C','G','S','A','Q'},'S');if(!scope)return false;if(*scope=='Q')return true;
        query.scope=*scope=='C'?FileSearchScope::area:*scope=='G'?FileSearchScope::group:FileSearchScope::all;query.area=current_;query.group=a->group;query.selected_only=*scope=='S';
    }
    const auto actor=p_.actor();if(!actor)return false;const auto selected=p_.storage.file_scan_areas(*actor);if(!selected||!selected.value)return say("Scan preferences unavailable.");
    std::vector<FileAreaId> areas;
    for(const auto& candidate:catalog_.areas){
        if(query.scope==FileSearchScope::area&&!(candidate.id==query.area))continue;
        if(query.scope==FileSearchScope::group&&!(candidate.group==query.group))continue;
        const auto preference=std::find_if(selected.value->begin(),selected.value->end(),[&](const auto& s){return s.area==candidate.id;});
        if(query.selected_only&&(preference==selected.value->end()||!preference->selected))continue;
        areas.push_back(candidate.id);
    }
    if(areas.empty())return login||say("No File Areas selected for scanning.");
    if(login){const auto available=p_.storage.file_page(*actor,query,{},1);if(!available||!available.value)return false;if(available.value->entries.empty())return true;query.publication_fence=available.value->publication_fence;
        const auto dimensions=p_.list.dimensions?p_.list.dimensions():MessageEditorDimensions{};
        if(!present_file_login_scan_question(p_.list.terminal,dimensions.ansi))return false;
        const auto scan=yes("files.login.scan"," ",true);if(!scan)return false;if(!*scan)return true;
    }
    return browse(query,std::move(areas));
}
bool FileCallerSession::login_scan(){return new_scan(true);}
bool FileCallerSession::settings(){
    bool repaint=true,positioned=false;
    unsigned columns{},rows{};
    std::string shown_scan,shown_lightbar,feedback,shown_feedback;
    const std::string scan_label="New File Scan at Logon: ",lightbar_label="File Light Bars: ";
    for(;;){
        if(!refresh())return false;
        const bool lightbars=p_.list.lightbars&&(!p_.list.lightbars_enabled||p_.list.lightbars_enabled());
        const auto size=p_.list.dimensions?p_.list.dimensions():MessageEditorDimensions{};
        const bool inline_values=size.ansi&&p_.list.write&&size.columns>=40&&size.rows>=12;
        if(positioned!=inline_values||columns!=size.columns||rows!=size.rows)repaint=true;
        positioned=inline_values;columns=size.columns;rows=size.rows;
        const std::string scan=state_.new_file_scan?"On":"Off";
        const std::string lightbar=lightbars?(state_.lightbar?"On":"Off"):"Disabled by Sysop";
        if(repaint){
            if(positioned){
                std::string frame="\x1b[0;37;40m\x1b[2J\x1b[HFile Scan Preferences\r\n\r\n";
                for(const auto& option:std::vector<std::pair<char,std::string>>{{'N',scan_label+scan},{'L',lightbar_label+lightbar},{'A',"Selected Scan Areas"},{'D',"New File Scan Date"},{'Q',"Return"}})
                    frame+=file_option(option.first,option.second,true)+"\r\n";
                if(!p_.list.write(frame))return false;
            }else if(!say("File Scan Preferences")||!options({{'N',scan_label+scan},{'L',lightbar_label+lightbar},{'A',"Selected Scan Areas"},{'D',"New File Scan Date"},{'Q',"Return"}}))return false;
            shown_scan=scan;shown_lightbar=lightbar;shown_feedback.clear();repaint=false;
        }else if(positioned){
            const auto value=[&](unsigned row,std::size_t column,std::string text,std::size_t width){text.resize(width,' ');return p_.list.write("\x1b["+std::to_string(row)+";"+std::to_string(column)+"H\x1b[0;37;40m"+text);};
            if(scan!=shown_scan&&!value(3,5+scan_label.size(),scan,3))return false;
            if(lightbar!=shown_lightbar&&!value(4,5+lightbar_label.size(),lightbar,17))return false;
            shown_scan=scan;shown_lightbar=lightbar;
        }
        if(positioned){
            if(feedback!=shown_feedback){if(!p_.list.write("\x1b[9;1H\x1b[2K\x1b[0;37;40m"+feedback))return false;shown_feedback=feedback;}
            if(!p_.list.write("\x1b[10;1H\x1b[2K"))return false;
        }else if(feedback!=shown_feedback){if(!feedback.empty()&&!say(feedback))return false;shown_feedback=feedback;}
        const auto choice=key("files.settings","File preferences [N L A D Q]? ",{'N','L','A','D','Q'},'Q',!positioned);if(!choice)return false;
        if(*choice=='Q')return !positioned||p_.list.write("\r\n");
        feedback.clear();
        const auto actor=p_.actor();if(!actor)return false;
        if(*choice=='A'){
            repaint=true;
            for(;;){
                if(!refresh())return false;
                const auto current=p_.actor();if(!current)return false;const auto states=p_.storage.file_scan_areas(*current);if(!states||!states.value)return say("Scan preferences unavailable.");
                FileListPage page;page.title="Selected File Scan Areas";page.context="Enter toggles selected area";
                for(const auto& entry:*states.value){const auto ar=area(entry.area);const auto g=ar?group(ar->group):nullptr;if(ar&&g)page.rows.push_back({ar->number,ar->id.value,entry.revision,std::string(entry.selected?"[X] ":"[ ] ")+g->name+" / "+ar->name});}
                std::size_t row{},offset{};auto list=p_.list;list.lightbars=list.lightbars&&state_.lightbar;list.emitted={};FileListChoice selected;
                for(;;){auto slice=page;slice.rows.assign(page.rows.begin()+static_cast<std::ptrdiff_t>(offset),page.rows.begin()+static_cast<std::ptrdiff_t>(std::min(offset+100,page.rows.size())));slice.previous=offset>0;slice.next=offset+100<page.rows.size();selected=choose_file_row(list,slice,row);if(selected.action==FileListAction::next){offset+=100;row=0;}else if(selected.action==FileListAction::previous){offset-=100;row=0;}else break;}
                if(selected.action==FileListAction::failed)return false;
                if(selected.action==FileListAction::cancel)break;
                const auto old=std::find_if(states.value->begin(),states.value->end(),[&](const auto& s){return s.area.value==selected.identity&&s.revision==selected.revision;});if(old==states.value->end())continue;
                const auto live=p_.actor();if(!live)return false;auto desired=*old;desired.selected=!desired.selected;const auto saved=p_.storage.save_file_scan_area(*live,desired);if(!say(saved?"Scan selection saved.":"Scan selection changed; please try again."))return false;
            }continue;
        }
        auto desired=state_;
        if(*choice=='N')desired.new_file_scan=!desired.new_file_scan;
        else if(*choice=='L'){if(!lightbars){feedback="File light bars are disabled by Sysop.";continue;}desired.lightbar=!desired.lightbar;}
        else if(*choice=='D'){
            repaint=true;
            if(positioned&&!p_.list.write("\r\n"))return false;
            const auto date=text("files.settings.date","New File Scan Date YYYY-MM-DD (Enter cancels): ",10);if(!date)return false;if(date->empty())continue;
            const auto parsed=p_.scan_date?p_.scan_date(*date):std::nullopt;if(!parsed||*parsed>state_.since_utc){if(!say("Enter a valid earlier date."))return false;continue;}desired.since_utc=*parsed;
        }
        const auto saved=p_.storage.save_file_caller_state(*actor,desired);
        if(!saved||!saved.value)feedback="Preference not saved; please try again.";
        else{
            state_=*saved.value;
            if(*choice=='D')feedback="File preference saved.";
            else if(!positioned&&!say(*choice=='N'?scan_label+(state_.new_file_scan?"On":"Off"):lightbar_label+(state_.lightbar?"On":"Off")))return false;
        }
    }
}
std::optional<FileDescriptions> FileCallerSession::descriptions(FileDescriptions initial,bool force_short,bool* sysop_only){
    const auto short_text=text("files.description.short",force_short?"Short description: ":"Short description (Enter keeps current): ",80);if(!short_text)return {};
    if(!short_text->empty()){
        auto value=*short_text;
        // Only live caller-written input owns the legacy leading-backslash
        // command. DIZ/imported metadata never enters this branch.
        if(sysop_only&&!value.empty()&&value.front()=='\\'){*sysop_only=true;value.erase(value.begin());}
        initial.short_text=std::move(value);
    }else if(force_short)return {};
    if(!valid_file_descriptions(initial)){say("Description is empty, too long or contains unsupported text.");return {};}
    const auto extended=yes("files.description.extended","Enter an extended description? [y/N] ",false);if(!extended)return {};
    if(*extended){
        std::string body;
        if(!say("Extended description: Enter a blank line to finish; Escape cancels."))return {};
        for(unsigned line=0;line<200;++line){const auto value=text("files.description.line","> ",80);if(!value)return {};if(value->empty())break;
            if(body.size()+value->size()+1>kFileLongDescriptionBytes){say("Extended description is too long.");return {};}if(!body.empty())body+='\n';body+=*value;
        }initial.long_text=std::move(body);
    }
    return valid_file_descriptions(initial)?std::optional<FileDescriptions>{std::move(initial)}:std::nullopt;
}
std::optional<CallerUploadReview> FileCallerSession::review_upload(const FileUploadItem& item,const std::optional<FileDescriptions>& diz,std::uint64_t bytes){
    (void)bytes;
    auto result=CallerUploadReview{item.prefer_diz&&diz?*diz:item.descriptions,item.sysop_only};
    // A valid selected DIZ or caller-supplied description needs no second
    // confirmation. Missing/invalid required metadata still cannot be accepted.
    if((item.prefer_diz&&!diz)||!valid_file_descriptions(result.descriptions)){
        if(!say("A description is needed before this file can be added."))return {};
        const auto entered=descriptions({},true,&result.sysop_only);if(!entered)return {};result.descriptions=*entered;
    }
    return result;
}
bool FileCallerSession::add_uploads(bool private_submission,std::vector<FileUploadItem>* private_items){
    if(!refresh())return false;
    if(!private_submission){const auto a=area(current_);const auto g=a?group(a->group):nullptr;if(!a||!g||!file_area_uploadable(*g,*a,p_.authority(),false)){
            if(!say("The current File Area does not allow your uploads. Use Change File Area.\n<enter>"))return false;
            return p_.list.terminal.pause({"files.upload.denied"})==TerminalOutcome::completed;
        }
        if(p_.upload_space){const auto space=p_.upload_space(current_);if(!space)return say("Upload space unavailable. Nothing queued.");if(!say("Effective upload space: "+std::to_string(*space)+" bytes."))return false;}
    }else if(!say("Private Upload to Sysop\nThis submission is separate from your normal upload queue."))return false;
    const auto diz=yes("files.upload.diz","Use FILE_ID.DIZ where available for this batch? [Y/n] ",true);if(!diz)return false;prefer_diz_=*diz;
    for(;;){
        if(!refresh())return false;
        const auto name=text("files.upload.name","Upload filename only (Enter finishes adding): ",128);if(!name)return false;if(name->empty())return true;
        if(!valid_file_name(*name)){if(!say("Enter a safe filename only, without a drive or directory."))return false;continue;}
        FileUploadItem item{*name,private_submission?FileAreaId{1}:current_,{sanitize_file_text(*name,80,false),{}},false,prefer_diz_};
        if(!prefer_diz_){const auto entered=descriptions(item.descriptions,true,&item.sysop_only);if(!entered){if(!p_.list.live())return false;continue;}item.descriptions=*entered;}
        if(private_submission){if(!private_items||private_items->size()>=kFileQueueMaximum)return say("Private submission queue full.");
            const auto folded=file_case_key(item.name);if(std::any_of(private_items->begin(),private_items->end(),[&](const auto& prior){return file_case_key(prior.name)==folded;})){if(!say("Filename already queued in this submission."))return false;continue;}private_items->push_back(std::move(item));
        }else if(!uploads_.add(std::move(item))){if(!say("Filename already queued, or upload queue is full."))return false;continue;}
        if(!say("File queued."))return false;
        const auto another=yes("files.upload.another","Add another file? [y/N] ",false);
        if(!another)return false;
        if(!*another)return true;
    }
}
std::optional<FileSessionEffect> FileCallerSession::add_downloads(){
    for(;;){
        if(!queue_view(false,true))return {};
        const auto file=choose_exact("Download filename / partial / wildcard (Enter finishes): ",true);
        if(!file){if(!p_.list.live())return {};break;}
        const auto actor=p_.actor();if(!actor)return {};const auto current=p_.storage.file_information(*actor,*file);if(current&&current.value){if(!tag(*file,current.value->revision))return {};}
    }
    if(downloads_.items().empty())return FileSessionEffect{"Download queue is empty."};
    const auto now=yes("files.download.now","Start download now? [Y/n] ",true);if(!now)return {};if(*now)return transfer(false);return FileSessionEffect{{},MenuId::files};
}
bool FileCallerSession::queue_view(bool upload,bool download){
    if(!refresh())return false;
    std::vector<std::string> lines{"Batch Queues"};
    if(upload){
        lines.emplace_back("Upload Queue:");
        unsigned number{};
        for(const auto& item:uploads_.items()){
            const auto a=area(item.destination);const auto g=a?group(a->group):nullptr;
            lines.push_back(std::to_string(++number)+" "+item.name);
            // Recheck access before showing the destination.
            lines.push_back(a&&g?"  ["+g->name+" / "+std::to_string(a->number)+" "+a->name+"]":
                                "  Destination unavailable");
        }
        lines.push_back("Upload files: "+std::to_string(uploads_.items().size()));
    }
    if(download){
        lines.emplace_back("Download Queue:");
        unsigned number{};
        std::uint64_t total{};
        for(const auto id:downloads_.items()){
            const auto actor=p_.actor();if(!actor)return false;
            const auto found=p_.storage.file_information(*actor,id);++number;
            if(!found||!found.value){lines.push_back(std::to_string(number)+" File unavailable");continue;}
            const auto& file=*found.value;const auto a=area(file.area);const auto g=a?group(a->group):nullptr;
            if(!a||!g)continue;
            lines.push_back(std::to_string(number)+" "+file.name);
            lines.push_back("  "+std::to_string(file.bytes)+" bytes ["+g->name+" / "+std::to_string(a->number)+" "+a->name+"]");
            total=file.bytes>UINT64_MAX-total?UINT64_MAX:total+file.bytes;
        }
        lines.push_back("Download files: "+std::to_string(downloads_.items().size())+"; total bytes: "+std::to_string(total));
    }
    const auto size=p_.list.dimensions?p_.list.dimensions():MessageEditorDimensions{};
    return present_file_transfer_text(p_.list.terminal,"files.transfer.queue",lines,size);
}
bool FileCallerSession::queue_remove(bool upload,bool download){
    if(!queue_view(upload,download))return false;
    if(upload&&download){const auto direction=key("files.queue.direction","Remove from [U] Upload, [D] Download, or [A] All queues? [Q]: ",{'U','D','A','Q'},'Q');if(!direction)return false;if(*direction=='Q')return true;
        if(*direction=='A'){const auto cleared=clear(true,true);if(!cleared)return false;return say(*cleared?"Queue(s) cleared. No physical file deleted.":"Queue(s) unchanged.");}
        upload=*direction=='U';download=!upload;}
    const auto row=text("files.queue.remove","Remove displayed number (Enter cancels): ",3);if(!row)return false;if(row->empty())return true;const auto n=positive(*row);
    const bool removed=n&&(upload?uploads_.remove(n-1):n<=downloads_.items().size()&&downloads_.remove(downloads_.items()[n-1]));
    return say(removed?"Queue entry removed. No physical file deleted.":"Choose a displayed queue number.");
}
std::optional<bool> FileCallerSession::clear(bool upload,bool download,bool returning){
    if((!upload||uploads_.items().empty())&&(!download||downloads_.items().empty()))return true;
    const auto who=p_.caller_name?file_terminal_text(p_.caller_name(),64):std::string("Caller");
    const auto cleared=yes("files.queue.clear",(returning?who+", return to File Menu and clear the untransferred queue? [y/N] ":"Clear selected batch queue(s)? [y/N] "),false);if(!cleared||!*cleared)return cleared;
    if(upload)uploads_.clear();
    if(download)downloads_.clear();
    return true;
}
std::optional<bool> FileCallerSession::leave(MenuId from,MenuId to){
    (void)from;(void)to;
    // Navigation never owns queue deletion. Only explicit Remove/Clear or a
    // proven per-file completion/terminal received rejection consumes work.
    return true;
}
std::optional<FileSessionEffect> FileCallerSession::transfer(bool upload,bool private_submission){
    if(!refresh())return {};
    std::vector<FileUploadItem> incoming;
    if(private_submission){if(!add_uploads(true,&incoming))return {};if(incoming.empty())return FileSessionEffect{"Private submission cancelled."};}
    else if(upload)incoming=uploads_.items();
    if(upload?incoming.empty():downloads_.items().empty())return FileSessionEffect{"Batch queue is empty."};
    const auto protocol=p_.protocol?p_.protocol():std::nullopt;if(!protocol)return FileSessionEffect{"Transfer preference unavailable."};
    using Protocol=TransferProtocolPreference;
    const auto enabled=[&](Protocol value){return value!=Protocol::select_at_transfer&&
        (value==Protocol::xmodem||value==Protocol::ymodem||value==Protocol::ymodem_g||value==Protocol::zmodem)&&
        (!p_.protocol_available||p_.protocol_available(value));};
    auto selected=*protocol;
    if(selected==Protocol::select_at_transfer){
        std::vector<std::pair<char,std::string>> choices;std::vector<char> keys;
        for(const auto& entry:std::vector<std::pair<char,Protocol>>{{'X',Protocol::xmodem},{'Y',Protocol::ymodem},{'G',Protocol::ymodem_g},{'Z',Protocol::zmodem}}){
            if(!enabled(entry.second))continue;
            choices.push_back({entry.first,entry.first=='X'?"XMODEM":entry.first=='Y'?"YMODEM":entry.first=='G'?"YMODEM-G":"ZMODEM"});keys.push_back(entry.first);
        }
        if(keys.empty())return FileSessionEffect{"No transfer protocol is available."};
        choices.push_back({'Q',"Cancel"});keys.push_back('Q');
        const auto blank_line=[&](){
            TerminalDisplayProgram program;program.output_byte_count=2;
            program.operations.push_back({TerminalDisplayOperationKind::logical_newline,{}});
            TerminalDisplayPresentationRequest request{"files.protocol.spacing",&program,{false,std::nullopt}};request.system_ui=true;
            return p_.list.terminal.present_display(request)==TerminalOutcome::completed;
        };
        if(!blank_line()||!options(choices)||!blank_line())return {};
        const auto choice=key("files.protocol","Transfer protocol (Enter cancels): ",keys,'Q');if(!choice)return {};
        if(*choice=='Q')return FileSessionEffect{"Transfer cancelled."};
        selected=*choice=='X'?Protocol::xmodem:*choice=='Y'?Protocol::ymodem:*choice=='G'?Protocol::ymodem_g:Protocol::zmodem;
    }
    if(!enabled(selected))return FileSessionEffect{"Your saved transfer protocol is unavailable. Change it under Your Info / File Transfer."};
    auto outgoing=downloads_.items();
    if(selected==Protocol::xmodem){
        if(upload){
            if(!say("XMODEM sends one file. Enter its queued filename before starting."))return {};
            const auto name=text("files.xmodem.filename","XMODEM upload filename (Enter cancels): ",128);if(!name)return {};
            if(name->empty())return FileSessionEffect{"Transfer cancelled."};
            const auto found=std::find_if(incoming.begin(),incoming.end(),[&](const auto& item){return file_case_key(item.name)==file_case_key(*name);});
            if(found==incoming.end())return FileSessionEffect{"Filename is not in this upload queue. Nothing transferred."};
            const auto item=*found;incoming={item};
        }else outgoing.resize(1);
        if(!say("XMODEM transfers one file at a time. Other files in your normal queue stay queued."))return {};
    }
    const auto logoff=yes("files.transfer.logoff","Log off after transfer? [Y/n] ",true);if(!logoff)return {};
    if(!say(upload?"Begin upload in your terminal.":"Begin download in your terminal."))return {};
    CallerFileBatchResult result;
    if(upload){if(!p_.upload)return FileSessionEffect{"Upload service unavailable."};result=p_.upload(incoming,private_submission,selected);}
    else {if(!p_.download)return FileSessionEffect{"Download service unavailable."};result=p_.download(outgoing,selected);}
    if(!result.connected)return {};
    const bool accepted_upload=upload&&!result.files.empty()&&result.files.size()==incoming.size()&&
        std::all_of(result.files.begin(),result.files.end(),[](const auto& f){return f.completed&&!f.held&&!f.deleted&&!f.skipped&&(f.scan.verdict==FileScanVerdict::clean||f.scan.verdict==FileScanVerdict::not_scanned);});
    const bool scanned_upload=accepted_upload&&std::all_of(result.files.begin(),result.files.end(),[](const auto& f){return f.scan.verdict==FileScanVerdict::clean;});
    std::vector<std::string> report;
    if(!accepted_upload)report.emplace_back(upload?"Upload report:":"Download report:");
    if(result.files.empty())report.emplace_back("Transfer unavailable. No completed file was reported.");
    // Bound the display program even when every file in a large batch has many matches.
    const auto signatures_per_file=1024/std::max<std::size_t>(1,result.files.size());
    std::size_t completed{},received{};std::uint64_t bytes{},received_bytes{};
    for(const auto& file:result.files){
        if(upload){received_bytes+=file.bytes;if(file.received||file.completed)++received;}
        if(!accepted_upload){
            report.push_back(file.name);
            auto detail=file.detail;
            for(const std::string_view suffix:{"; retained privately Private copy retained.",
                                              "; held privately Private copy retained."}){
                if(detail.size()>=suffix.size()&&detail.compare(detail.size()-suffix.size(),suffix.size(),suffix)==0){
                    detail.replace(detail.size()-suffix.size(),suffix.size(),". Private copy retained.");
                    break;
                }
            }
            report.push_back("  "+detail);
            const auto shown=std::min(signatures_per_file,file.scan.detections.size());
            for(std::size_t i=0;i<shown;++i)report.push_back("  "+sanitize_file_text(file.scan.detections[i],128,false));
            if(shown<file.scan.detections.size())
                report.push_back("  Additional signatures: "+std::to_string(file.scan.detections.size()-shown));
        }
        const bool added=file.completed&&(!upload||(!file.held&&!file.deleted&&!file.skipped&&(file.scan.verdict==FileScanVerdict::clean||file.scan.verdict==FileScanVerdict::not_scanned)));
        // Queue ownership is separate from success accounting. A complete receive
        // that is then rejected/deleted is terminal; an interrupted receive or
        // preflight refusal has no received proof and remains available to retry.
        if(upload&&!private_submission&&(added||file.received)){const auto key=file_case_key(file.name);for(std::size_t i=0;i<uploads_.items().size();++i)if(file_case_key(uploads_.items()[i].name)==key){uploads_.remove(i);break;}}
        if(!added)continue;
        ++completed;bytes+=file.bytes;
        if(!upload)downloads_.remove(file.file);
    }
    if(accepted_upload){
        const auto added=completed==1?"file added":"files added";
        const bool no_av=p_.malware_scanning_available&&!p_.malware_scanning_available();
        if(!say(std::string(scanned_upload?"Scan complete - clean; ":"Upload complete - ")+added+(scanned_upload||no_av?"":" (malware scanning Off).")))return {};
    }else if(upload){
        report.push_back("Received: "+std::to_string(received)+" complete file(s), "+std::to_string(received_bytes)+" bytes.");
        report.push_back("Added: "+std::to_string(completed)+" file(s), "+std::to_string(bytes)+" bytes.");
    }else report.push_back("Completed: "+std::to_string(completed)+" file(s), "+std::to_string(bytes)+" bytes.");
    if(!report.empty()){
        const auto size=p_.list.dimensions?p_.list.dimensions():MessageEditorDimensions{};
        if(!present_file_transfer_text(p_.list.terminal,"files.transfer.result",report,size))return {};
    }
    if(*logoff){
        const auto disconnect=p_.transfer_logoff?p_.transfer_logoff():std::nullopt;
        if(!disconnect)return {};
        if(*disconnect)return FileSessionEffect{"Transfer report recorded.",{},true};
        return FileSessionEffect{"Logoff aborted.",MenuId::files};
    }
    if(!say(accepted_upload?"<enter>":"\n<enter>"))return {};
    if(p_.list.terminal.pause({"files.transfer.report"})!=TerminalOutcome::completed)return {};
    return FileSessionEffect{{},MenuId::files};
}
std::optional<FileSessionEffect> FileCallerSession::action(MenuAction action,MenuId current_menu){
    if(!refresh())return {};
    bool result{};
    switch(action){
    case MenuAction::file_change:{const auto changed=change_area();if(!changed)return {};return FileSessionEffect{};}
    case MenuAction::file_list:{FileQuery q;q.area=current_;if(!q.area.value)return FileSessionEffect{"No accessible File Area."};result=browse(q);break;}
    case MenuAction::file_new:result=new_scan(false);break;
    case MenuAction::file_find:result=search(FileSearchKind::filename);break;
    case MenuAction::file_search:result=search(FileSearchKind::descriptions);break;
    case MenuAction::file_read:case MenuAction::file_archive:{const auto file=choose_exact("Filename to view (Enter cancels): ",false);result=file?(p_.view?p_.view(*file,action==MenuAction::file_archive):say("File viewer unavailable.")):p_.list.live();break;}
    case MenuAction::file_download_add:return add_downloads();
    case MenuAction::file_upload_add:{
        const auto a=area(current_);const auto g=a?group(a->group):nullptr;
        const bool permitted=a&&g&&file_area_uploadable(*g,*a,p_.authority(),false);
        if(!add_uploads(false))return {};
        if(!permitted||uploads_.items().empty())return FileSessionEffect{};
        const auto now=yes("files.upload.now","Start upload now? [Y/n] ",true);if(!now)return {};if(*now)return transfer(true);return FileSessionEffect{{},MenuId::files};}
    case MenuAction::file_both_add:{
        if(!options({{'U',"Add upload files"},{'D',"Add download files"},{'Q',"Return"}}))return {};
        const auto direction=key("files.queue.add.direction","Add to queue [U D Q]? [Q]: ",{'U','D','Q'},'Q');if(!direction)return {};
        if(*direction=='Q')return FileSessionEffect{};
        if(*direction=='U'){if(!add_uploads(false))return {};return FileSessionEffect{};}
        for(;;){const auto file=choose_exact("Download filename / partial / wildcard (Enter finishes): ",true);if(!file){if(!p_.list.live())return {};break;}
            const auto actor=p_.actor();if(!actor)return {};const auto entry=p_.storage.file_information(*actor,*file);if(entry&&entry.value&&!tag(*file,entry.value->revision))return {};}
        return FileSessionEffect{};
    }
    case MenuAction::file_upload_view:case MenuAction::file_download_view:case MenuAction::file_both_view:
        result=queue_view(action!=MenuAction::file_download_view,action!=MenuAction::file_upload_view)&&
            say("<enter>")&&p_.list.terminal.pause({"files.queue.view"})==TerminalOutcome::completed;
        break;
    case MenuAction::file_upload_remove:result=queue_remove(true,false);break;
    case MenuAction::file_download_remove:result=queue_remove(false,true);break;
    case MenuAction::file_both_remove:result=queue_remove(true,true);break;
    case MenuAction::file_upload_clear:case MenuAction::file_download_clear:case MenuAction::file_both_clear:{const auto done=clear(action!=MenuAction::file_download_clear,action!=MenuAction::file_upload_clear);if(!done)return {};return FileSessionEffect{*done?"Queue(s) cleared.":"Queue(s) unchanged."};}
    case MenuAction::file_upload_begin:return transfer(true);
    case MenuAction::file_download_begin:return transfer(false);
    case MenuAction::file_private:return transfer(true,true);
    case MenuAction::file_settings:result=settings();break;
    default:return FileSessionEffect{"File action unavailable."};
    }
    (void)current_menu;return result?std::optional<FileSessionEffect>{FileSessionEffect{}}:std::nullopt;
}
} // namespace sfx::application
