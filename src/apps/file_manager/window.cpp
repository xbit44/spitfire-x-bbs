#include "window.hpp"
#include "dialogs.hpp"
#include "preview_window.hpp"
#include "visuals.hpp"
#include "sfx/platform/win32/editor_placement.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <bcrypt.h>
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <map>
#include <string_view>

namespace sfx::file_manager {
namespace {
namespace ins=win::installation;
constexpr wchar_t manager_class[]=L"SFXFileManagerWindow";
enum Control : UINT {browse=2001,search_files,pending_files,offline_files,entries,back,open_item,
    rename_file,move_file,remove_file,details,contents,information,status_check,retry_same,cancel_job,status_line,
    legacy_files=40010,recovery_files,reconcile_job,cancel_intent,hatch_file};
enum class Screen {home,groups,areas,files,search,legacy,recovery};
enum class Purpose {ordinary,remove,view,view_close,scan,scan_report,reconcile,hatch_verify,hatch};
std::wstring wide(std::string_view value){if(value.empty())return {};const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);if(n<=0)return {};std::wstring out(static_cast<std::size_t>(n),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),n);return out;}
std::string utf8(std::wstring_view value){if(value.empty())return {};const auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);if(n<=0)return {};std::string out(static_cast<std::size_t>(n),'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),n,nullptr,nullptr);return out;}
app::Identifier128 fresh_id(){app::Identifier128 id;do{ins::require(BCryptGenRandom(nullptr,id.bytes.data(),static_cast<ULONG>(id.bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"file_manager_random_unavailable");}while(!id.valid());return id;}
std::wstring date(std::int64_t utc){if(utc<0)return L"Unknown";ULARGE_INTEGER tick;tick.QuadPart=(static_cast<ULONGLONG>(utc)+11644473600ULL)*10000000ULL;FILETIME file{tick.LowPart,tick.HighPart};SYSTEMTIME time{};if(!FileTimeToSystemTime(&file,&time))return L"Unknown";wchar_t value[40]{};swprintf_s(value,L"%04u-%02u-%02u %02u:%02u UTC",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute);return value;}
struct State {
    HINSTANCE instance;std::filesystem::path root;std::wstring owner;ClientWorker::Factory factory;
    HWND window{};std::map<UINT,HWND> controls;HFONT font{};int dpi{96};
    win::DesktopAppearance appearance;win::EditorPlacementStore placement{L"Software\\Spitfire-X BBS\\File Manager Window"};
    std::unique_ptr<ClientWorker> worker;std::unique_ptr<PreviewWindow> preview;JobTracker tracking;
    ClientCommand last;Purpose purpose{Purpose::ordinary};Screen screen{Screen::home};
    std::optional<app::FileConfiguration> configuration;app::FileGroupId group;app::FileAreaId area;
    std::vector<app::FileGroupId> groups;std::vector<app::FileAreaId> areas;
    std::vector<app::ManagedFile> rows,removals;std::vector<app::FileManagementPending> unresolved;
    app::FileManagementQueryKind recovery_kind{app::FileManagementQueryKind::operations};
    app::Identifier128 scan_job;std::uint16_t scan_page{},scan_pages{};bool scan_complete{};
    std::wstring report;std::string pattern;std::size_t removal_index{},read_count{};std::uint64_t removed_count{},serial{};
    ULONGLONG polled{};bool ready{},online{},closing{},disconnecting{},refreshing{},loading{},stop_batch{},tagged{},persist_placement{true};
    std::optional<app::FileManagementViewPage> viewport;
    std::optional<app::ManagedFile> hatch_selection;
    State(HINSTANCE i,std::filesystem::path r,std::wstring o,ClientWorker::Factory f,std::wstring theme_key=L"Software\\Spitfire-X BBS\\Appearance"):instance(i),root(std::move(r)),owner(std::move(o)),factory(std::move(f)),appearance(std::move(theme_key)){}
    ~State(){preview.reset();if(worker)worker->stop();if(font)DeleteObject(font);}
    HWND control(UINT id)const{const auto i=controls.find(id);return i==controls.end()?nullptr:i->second;}
    bool available()const{return ready&&worker&&!worker->busy()&&!tracking.blocks_edits()&&!closing&&!loading;}
    bool file_screen()const{return screen==Screen::files||screen==Screen::search||screen==Screen::legacy;}
    void message(const std::wstring& value){SetWindowTextW(control(status_line),value.c_str());}
    const app::FileAreaConfiguration* configured_area(app::FileAreaId id)const{if(configuration)for(const auto& a:configuration->areas)if(a.area.id==id)return &a;return nullptr;}
    std::wstring area_label(app::FileAreaId id)const{const auto* a=configured_area(id);if(!a)return L"Unknown File Area";std::wstring value;for(const auto& g:configuration->groups)if(g.id==a->area.group)value=wide(g.name)+L" / ";return value+wide(a->area.name);}
    int focused()const{return ListView_GetNextItem(control(entries),-1,LVNI_FOCUSED);}
    std::vector<std::size_t> selected()const{std::vector<std::size_t> result;for(int n=ListView_GetNextItem(control(entries),-1,LVNI_SELECTED);n>=0;n=ListView_GetNextItem(control(entries),n,LVNI_SELECTED))result.push_back(static_cast<std::size_t>(n));return result;}
    void enable();void layout();bool create();void show();void command(UINT);void navigate();void go_back();void load();
    void request_read(app::FileId after={},app::Identifier128 pending_after={});bool send(ClientCommand);bool start(app::FileManagementTask,Purpose);void tick();void receive(ClientReply);void completed(const app::FileManagementJobStatus&);
    void remove_selected();void next_removal();void finish_removal();void rename_or_move(bool);void show_details();void close();void key(WPARAM);
    void view_navigation(ViewNavigation);void read_scan_page();void finish_scan_report();
    void begin_hatch();void new_hatch();void read_hatches(app::Identifier128 after={});void review_pending_hatch(const ClientReply&);void review_hatch(const app::FtnHatchCatalog&);
    static LRESULT CALLBACK proc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK list_keys(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
};
void State::layout(){
    RECT client{};GetClientRect(window,&client);const auto scale=[&](int value){return MulDiv(value,dpi,96);};
    const int width=(std::max)(scale(660),static_cast<int>(client.right)),height=(std::max)(scale(430),static_cast<int>(client.bottom));
    const auto put=[&](UINT id,int x,int y,int w,int h){MoveWindow(control(id),scale(x),scale(y),w<0?width-scale(x+12):scale(w),scale(h),TRUE);};
    put(information,12,12,-1,34);
    if(screen==Screen::home){for(unsigned n=0;n<4;++n)put(browse+n,24,68+static_cast<int>(n)*48,280,36);}
    else{for(unsigned n=0;n<4;++n)put(browse+n,12+static_cast<int>(n)*166,50,158,30);}
    MoveWindow(control(entries),scale(12),scale(94),width-scale(24),height-scale(218),TRUE);
    const int bottom=height-scale(114);unsigned index{};std::vector<UINT> actions;
    for(const auto id:{back,open_item,rename_file,move_file,remove_file,details,contents,hatch_file,reconcile_job,cancel_intent})if(GetWindowLongPtrW(control(id),GWL_STYLE)&WS_VISIBLE)actions.push_back(id);
    const auto step=actions.empty()?scale(108):(std::min)(scale(108),(width-scale(24))/static_cast<int>(actions.size()));for(auto id:actions){MoveWindow(control(id),scale(12)+static_cast<int>(index)*step,bottom,(std::max)(scale(30),step-scale(6)),scale(28),TRUE);++index;}
    MoveWindow(control(status_line),scale(12),height-scale(72),width-scale(24),scale(24),TRUE);
    index=0;for(const auto id:{status_check,retry_same,cancel_job}){MoveWindow(control(id),scale(12+static_cast<int>(index)*180),height-scale(40),scale(172),scale(28),TRUE);++index;}
}
void State::enable(){
    const bool idle=available();for(const auto id:{browse,search_files,pending_files,offline_files,back,open_item})EnableWindow(control(id),idle);
    // Keep a read-only snapshot painted normally during work. The list subclass
    // consumes input while busy; every action still checks available()/native authority.
    EnableWindow(control(entries),!closing);const auto picks=selected();const bool one=idle&&file_screen()&&picks.size()==1&&picks[0]<rows.size()&&!rows[picks[0]].operation_pending;
    EnableWindow(control(rename_file),one&&screen==Screen::files);EnableWindow(control(move_file),one&&screen==Screen::search);
    EnableWindow(control(details),one&&screen!=Screen::legacy);EnableWindow(control(contents),one&&screen!=Screen::legacy);
    EnableWindow(control(hatch_file),one&&screen!=Screen::legacy&&rows[picks[0]].file.state==app::FileCatalogState::available);
    EnableWindow(control(remove_file),idle&&file_screen()&&!picks.empty());
    for(const auto id:{reconcile_job,cancel_intent})EnableWindow(control(id),idle&&screen==Screen::recovery&&picks.size()==1);
    const bool tracked=tracking.blocks_edits()&&worker&&!worker->busy()&&!closing;
    EnableWindow(control(status_check),tracked);EnableWindow(control(retry_same),tracked&&tracking.state()==JobTracking::unknown);EnableWindow(control(cancel_job),(tracked||!removals.empty())&&!closing);
    for(const auto id:{status_check,retry_same,cancel_job})ShowWindow(control(id),tracking.blocks_edits()||!removals.empty()?SW_SHOW:SW_HIDE);
    if(preview)preview->busy(!idle);
}
void State::show(){
    refreshing=true;tagged=false;ListView_DeleteAllItems(control(entries));
    for(const auto id:{entries,back,open_item,rename_file,move_file,remove_file,details,contents,hatch_file,reconcile_job,cancel_intent})ShowWindow(control(id),SW_HIDE);
    if(screen!=Screen::home){ShowWindow(control(entries),SW_SHOW);ShowWindow(control(back),SW_SHOW);}
    for(const auto id:{open_item})ShowWindow(control(id),screen==Screen::groups||screen==Screen::areas?SW_SHOW:SW_HIDE);
    for(const auto id:{remove_file,details,contents})ShowWindow(control(id),file_screen()&&(screen!=Screen::legacy||id==remove_file)?SW_SHOW:SW_HIDE);
    ShowWindow(control(hatch_file),file_screen()&&screen!=Screen::legacy?SW_SHOW:SW_HIDE);
    ShowWindow(control(rename_file),screen==Screen::files?SW_SHOW:SW_HIDE);ShowWindow(control(move_file),screen==Screen::search?SW_SHOW:SW_HIDE);
    for(const auto id:{reconcile_job,cancel_intent})ShowWindow(control(id),screen==Screen::recovery?SW_SHOW:SW_HIDE);
    const auto add=[&](const std::array<std::wstring,4>& values){const auto n=ListView_GetItemCount(control(entries));LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=n;item.pszText=const_cast<wchar_t*>(values[0].c_str());ListView_InsertItem(control(entries),&item);for(int k=1;k<4;++k)ListView_SetItemText(control(entries),n,k,const_cast<wchar_t*>(values[static_cast<std::size_t>(k)].c_str()));};
    std::wstring title=L"File Manager - choose an operation";
    if(screen==Screen::groups){title=L"Browse - choose a File Group";groups.clear();if(configuration)for(const auto& g:configuration->groups){groups.push_back(g.id);add({wide(g.name),L"",L"",L""});}}
    if(screen==Screen::areas){title=L"Browse - choose a File Area";areas.clear();if(configuration)for(const auto& a:configuration->areas)if(a.area.group==group){areas.push_back(a.area.id);add({wide(a.area.name),L"",L"",L""});}}
    if(file_screen()){
        title=screen==Screen::legacy?L"Previously removed files - review and Remove to finish deletion":screen==Screen::search?L"Filename search: "+wide(pattern):L"Browse: "+area_label(area);
        title+=L" ("+std::to_wstring(rows.size())+L" files)";
        for(const auto& row:rows)add({wide(row.file.name),wide(row.file.descriptions.short_text),std::to_wstring(row.file.bytes),area_label(row.file.area)});
    }
    if(screen==Screen::recovery){title=L"Unfinished work - select an item to review";for(const auto& row:unresolved)add({wide(row.filename),wide(row.detail),L"",area_label(row.area)});}
    SetWindowTextW(control(information),title.c_str());
    if(ListView_GetItemCount(control(entries)))ListView_SetItemState(control(entries),0,LVIS_FOCUSED|LVIS_SELECTED,LVIS_FOCUSED|LVIS_SELECTED);
    refreshing=false;enable();layout();
}
bool State::send(ClientCommand request){request.serial=++serial;if(!worker||!worker->submit(request))return false;last=std::move(request);enable();return true;}
bool State::start(app::FileManagementTask task,Purpose why){if(!available()||!tracking.begin(task))return false;purpose=why;ClientCommand request;request.kind=ClientCommandKind::start;request.task=std::move(task);if(!send(std::move(request))){tracking.link_failed();message(L"Job status is unknown. Check status or retry the same request.");enable();return false;}return true;}
void State::request_read(app::FileId after,app::Identifier128 cursor){ClientCommand request;request.kind=ClientCommandKind::read;
    if(screen==Screen::recovery){request.query.kind=recovery_kind;request.query.pending_after=cursor;}
    else{request.query.after=after;request.query.filter.state=screen==Screen::legacy?app::FileCatalogState::removed:app::FileCatalogState::available;request.query.filter.area=screen==Screen::files?area:app::FileAreaId{};if(screen==Screen::search){request.query.filter.field=app::FileManagementSearch::filename;request.query.filter.search=pattern;}}
    if(!send(request)){loading=false;message(L"File list could not be read. Choose the operation again.");}}
void State::load(){rows.clear();unresolved.clear();read_count=0;loading=true;message(L"Reading files...");show();request_read();}
void State::tick(){
    if(worker)if(auto reply=worker->poll())receive(std::move(*reply));if(!window)return;
    if(closing&&worker&&!worker->busy()&&!disconnecting){disconnecting=true;ClientCommand request;request.kind=ClientCommandKind::disconnect;send(request);return;}
    if(!closing&&tracking.state()==JobTracking::running&&!worker->busy()&&GetTickCount64()-polled>=150){polled=GetTickCount64();ClientCommand request;request.kind=ClientCommandKind::status;request.job=tracking.exact_task()->job;send(request);}
}
void State::receive(ClientReply reply){
    if(reply.serial!=last.serial)return;
    if(reply.kind==ClientCommandKind::disconnect){ready=false;if(!reply.diagnostic.empty())show_readonly(window,appearance,L"File Manager needs attention",wide(reply.diagnostic));DestroyWindow(window);return;}
    if(reply.kind==ClientCommandKind::hatch_catalog){enable();if(reply.hatch_catalog)review_hatch(*reply.hatch_catalog);else message(wide(reply.diagnostic));hatch_selection.reset();enable();return;}
    if(reply.kind==ClientCommandKind::connect||reply.kind==ClientCommandKind::configuration){ready=reply.connected&&reply.configuration.has_value();online=reply.online;configuration=std::move(reply.configuration);show();message(ready?L"Ready. Choose Browse or a search operation.":wide(reply.diagnostic));return;}
    if(reply.kind==ClientCommandKind::read){
        if(last.query.kind==app::FileManagementQueryKind::hatches){enable();review_pending_hatch(reply);enable();return;}
        if(reply.read_code!=app::LocalAccountResultCode::ok){loading=false;rows.clear();unresolved.clear();show();message(L"File list unavailable. No changes were made; reopen this operation.");return;}
        if(last.query.kind==app::FileManagementQueryKind::detail){
            enable(); // restore list colors before the modal disables its owner
            if(reply.records.size()==1){const auto& f=reply.records[0].file;show_readonly(window,appearance,L"File Details",L"Filename: "+wide(f.name)+L"\nFile Group / File Area: "+area_label(f.area)+L"\nSize: "+std::to_wstring(f.bytes)+L" bytes\nUploader: "+wide(f.public_uploader)+L"\nContent origin: "+(f.origin==app::FileOrigin::ftn?std::wstring(L"FTN file (local copy history retained)"):f.origin==app::FileOrigin::caller_upload?std::wstring(L"Caller upload"):f.origin==app::FileOrigin::sysop_copy?std::wstring(L"Sysop copy"):std::wstring(L"Sysop import"))+L"\nDate added: "+date(f.uploaded_utc)+L"\n\nDescription: "+wide(f.descriptions.short_text)+L"\n\n"+wide(f.descriptions.long_text));}enable();return;
        }
        if(screen==Screen::recovery)unresolved.insert(unresolved.end(),reply.pending.begin(),reply.pending.end());
        else for(auto& row:reply.records)if(screen!=Screen::legacy||!row.purged)rows.push_back(std::move(row));
        read_count+=reply.records.size()+reply.pending.size();
        if(read_count>100000){loading=false;rows.clear();unresolved.clear();show();message(L"This list exceeds the display limit. Narrow the filename search. Nothing selected or changed.");return;}
        if(reply.next.value||reply.pending_next.valid()){request_read(reply.next,reply.pending_next);return;}
        loading=false;
        std::stable_sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return app::file_case_key(a.file.name)<app::file_case_key(b.file.name);});
        show();message(screen==Screen::files?L"Space selects a file. Ctrl+A selects every file in this File Area.":screen==Screen::legacy?L"Only retained files are shown. Already deleted files remain in history.":L"Select a file to review its options.");return;
    }
    if(!tracking.observe(reply.status,reply.kind==ClientCommandKind::start)){message(L"Job status is unknown. Check status or retry the same request before other changes.");enable();return;}
    if(reply.status.code==app::LocalAccountResultCode::ok&&reply.status.state==app::FileManagementJobState::running){message(L"Working... "+wide(reply.status.detail));enable();return;}
    completed(reply.status);enable();
}
void State::completed(const app::FileManagementJobStatus& result){
    enable();
    const bool success=result.code==app::LocalAccountResultCode::ok&&result.state==app::FileManagementJobState::completed;
    if(purpose==Purpose::hatch){hatch_selection.reset();show_readonly(window,appearance,success?L"Hatch Queued":L"Hatch Result",wide(result.detail.empty()?"Hatch status could not be confirmed. Check the current job status before retrying.":result.detail));if(success)load();return;}
    if(purpose==Purpose::hatch_verify){bool verified{};if(success&&result.inspection&&hatch_selection){const auto& inspection=*result.inspection;for(std::size_t i=0;i<inspection.catalog.records.size()&&i<inspection.presence.size();++i){const auto& row=inspection.catalog.records[i];if(row.file.id==hatch_selection->file.id&&row.file.revision==hatch_selection->file.revision&&inspection.presence[i]==app::FilePayloadPresence::present){verified=true;hatch_selection=row;break;}}}if(!verified){hatch_selection.reset();message(L"The selected file's current identity/hash could not be verified. No hatch was prepared.");return;}ClientCommand request;request.kind=ClientCommandKind::hatch_catalog;send(request);return;}
    if(purpose==Purpose::remove){const auto& row=removals[removal_index];if(success)++removed_count;else report+=wide(row.file.name)+L" ["+area_label(row.file.area)+L", record "+std::to_wstring(row.file.id.value)+L"]: "+wide(result.detail.empty()?"Deletion could not be confirmed. Check the current job status before retrying.":result.detail)+L"\n";++removal_index;next_removal();return;}
    if(purpose==Purpose::view){if(success&&result.view){viewport=result.view;preview->show(*result.view);}else message(wide(result.detail));return;}
    if(purpose==Purpose::view_close){viewport.reset();return;}
    if(purpose==Purpose::scan){report=wide(result.detail);scan_complete=success;
        if(result.report){scan_job=result.job;scan_page=0;scan_pages=result.report->pages;read_scan_page();}else finish_scan_report();return;}
    if(purpose==Purpose::scan_report){
        if(!success||!result.report||result.report->page!=scan_page||result.report->pages!=scan_pages||report.size()+result.detail.size()>8*1024*1024){scan_complete=false;report+=L"\nResults could not be read completely. Do not treat this as a complete filename list.";finish_scan_report();return;}
        report+=wide(result.detail);if(++scan_page<scan_pages)read_scan_page();else finish_scan_report();return;
    }
    if(purpose==Purpose::reconcile&&success){load();return;}
    if(success){load();return;}show_readonly(window,appearance,L"File Operation",wide(result.detail.empty()?"Operation could not be confirmed. Check the current job status before retrying.":result.detail));
}
void State::read_scan_page(){app::FileManagementTask task;task.job=fresh_id();task.kind=app::FileManagementTaskKind::scan_report;task.target=scan_job;task.view_page=scan_page;if(!start(task,Purpose::scan_report)){scan_complete=false;report+=L"\nResults could not be read completely.";finish_scan_report();}}
void State::finish_scan_report(){message(scan_complete?L"Scan finished. Open Browse to review files.":L"Scan stopped or failed. Completed changes remain saved; review the results.");enable();show_readonly(window,appearance,L"File Scan Results",report);report.clear();}
void State::navigate(){if(!available())return;const auto n=focused();if(n<0)return;const auto index=static_cast<std::size_t>(n);
    if(screen==Screen::groups&&index<groups.size()){group=groups[index];screen=Screen::areas;show();}
    else if(screen==Screen::areas&&index<areas.size()){area=areas[index];screen=Screen::files;load();}
    else if(file_screen())show_details();
}
void State::go_back(){if(!available())return;preview->close();if(screen==Screen::files)screen=Screen::areas;else if(screen==Screen::areas)screen=Screen::groups;else screen=Screen::home;rows.clear();show();}
void State::show_details(){const auto picks=selected();if(!available()||picks.size()!=1||picks[0]>=rows.size()||screen==Screen::legacy)return;ClientCommand request;request.kind=ClientCommandKind::read;request.query.kind=app::FileManagementQueryKind::detail;request.query.selection=rows[picks[0]].file.id;send(request);}
void State::rename_or_move(bool move){
    const auto picks=selected();if(!available()||picks.size()!=1||picks[0]>=rows.size())return;const auto row=rows[picks[0]];if(row.operation_pending)return;
    FormField field;field.label=move?L"Destination File Group / File Area":L"New filename";
    std::vector<FormField> fields;
    if(move){field.kind=FormFieldKind::choice;field.selected=row.file.area.value;for(const auto& a:configuration->areas)field.choices.push_back({a.area.id.value,area_label(a.area.id)});fields.push_back(field);}
    else{field.value=wide(row.file.name);fields.push_back(field);}
    const auto chosen=show_form(window,appearance,move?L"Move File":L"Rename File",L"The file on disk and its database entry change together. An existing filename is never overwritten.",std::move(fields),[move](const auto& f)->std::optional<std::wstring>{if(!move&&!app::valid_file_name(utf8(f[0].value)))return L"Enter one valid filename, without a path.";return {};});if(!chosen)return;
    viewport.reset();preview->close(); // the mutation closes its native view lease
    app::FileManagementTask task;task.job=fresh_id();auto& op=task.operation;op.operation=fresh_id();op.file=row.file.id;op.expected_revision=row.file.revision;op.action=move?app::FileOperationAction::move:app::FileOperationAction::rename;op.destination=move?app::FileAreaId{(*chosen)[0].selected}:row.file.area;const auto* destination=configured_area(op.destination);if(!destination)return;op.expected_destination_revision=destination->area.revision;op.filename=move?row.file.name:utf8((*chosen)[0].value);start(task,Purpose::ordinary);
}
void State::remove_selected(){
    if(!available()||!file_screen())return;const auto picks=selected();if(picks.empty())return;std::vector<app::ManagedFile> selected_rows;std::wstring listed=L"Are you sure you want to remove the listed file(s)?\n\n";
    for(const auto n:picks){if(n>=rows.size())return;selected_rows.push_back(rows[n]);listed+=wide(rows[n].file.name)+L"  ["+area_label(rows[n].file.area)+L", "+std::to_wstring(rows[n].file.bytes)+L" bytes, added "+date(rows[n].file.uploaded_utc)+L"]\n";}
    listed+=L"\nFiles selected: "+std::to_wstring(selected_rows.size())+L". Yes permanently deletes these files. No leaves them unchanged.";
    if(!show_readonly(window,appearance,L"Confirm File Removal",listed,true))return;
    // Suppress a second asynchronous close-view job. The trusted mutation
    // dispatcher releases its own view lease before opening the exact file.
    viewport.reset();preview->close();removals=std::move(selected_rows);removal_index=0;removed_count=0;report.clear();stop_batch=false;next_removal();
}
void State::next_removal(){
    if(stop_batch||removal_index==removals.size()){finish_removal();return;}const auto& row=removals[removal_index];
    app::FileManagementTask task;task.job=fresh_id();task.operation.operation=fresh_id();task.operation.file=row.file.id;task.operation.expected_revision=row.file.revision;task.operation.action=app::FileOperationAction::purge;
    if(!start(task,Purpose::remove)){stop_batch=true;finish_removal();}
}
void State::finish_removal(){
    const auto summary=L"Deleted: "+std::to_wstring(removed_count)+L". Not confirmed deleted: "+std::to_wstring(removal_index-removed_count)+L". Not attempted: "+std::to_wstring(removals.size()-removal_index)+L".\n\n"+report;
    removals.clear();show_readonly(window,appearance,L"File Removal Results",summary);load();
}
void State::view_navigation(ViewNavigation navigation){
    if(!available())return;app::FileManagementTask task;task.job=fresh_id();
    if(navigation.kind==ViewNavigationKind::close){if(!viewport)return;task.kind=app::FileManagementTaskKind::close_view;task.target=viewport->cursor;start(task,Purpose::view_close);return;}
    task.target=navigation.cursor;task.view_page=navigation.selection;task.view_member=navigation.selection;
    task.kind=navigation.kind==ViewNavigationKind::member?app::FileManagementTaskKind::read_view_member:navigation.kind==ViewNavigationKind::archive_index?app::FileManagementTaskKind::view_archive_index:app::FileManagementTaskKind::page_view;start(task,Purpose::view);
}
#include "hatch_review.inc"
void State::command(UINT id){
    if(id==IDCANCEL){close();return;}
    if(id==40001){show_readonly(window,appearance,L"File Manager Help",L"Browse: choose File Group, File Area, then files. Space toggles selection; Ctrl/Shift click select several files. Ctrl+A selects all files in the current File Area, including off-screen files. F2 renames one file. Delete removes selected files after one confirmation.\n\nSearch Filenames searches every configured File Area; * and ? patterns are supported. Search results offer Move, Remove, Details and read-only ZIP Contents.\n\nSearch Pending Files registers unlisted files in every area, using FILE_ID.DIZ or No Description. Identical content under different filenames can be registered. Search Offline Files removes confirmed-missing listings and skips inaccessible locations. Both scans run in bulk.\n\nFile Manager follows the theme selected in SFX Control Panel. Old deletion history does not appear in file lists.");return;}
    if(id==status_check||id==retry_same||id==cancel_job){
        if(id==cancel_job)stop_batch=true;if(!tracking.blocks_edits()||!worker||worker->busy())return;ClientCommand request;request.job=tracking.exact_task()->job;request.kind=id==status_check?ClientCommandKind::status:id==retry_same?ClientCommandKind::start:ClientCommandKind::cancel;if(id==retry_same)request.task=*tracking.exact_task();send(request);return;
    }
    if(!available())return;
    if(id==hatch_file){begin_hatch();return;}
    if(id==browse){screen=Screen::groups;area={};show();return;}
    if(id==search_files){FormField field;field.label=L"Filename or pattern";field.value=wide(pattern);const auto value=show_form(window,appearance,L"Search Filenames",L"Search all File Groups and File Areas. Examples: 20meg*.zip or *.zip.",{field},[](const auto& f)->std::optional<std::wstring>{if(f[0].value.empty()||utf8(f[0].value).size()>128)return L"Enter a filename or pattern up to 128 bytes.";return {};});if(value){pattern=utf8((*value)[0].value);screen=Screen::search;area={};load();}return;}
    if(id==pending_files||id==offline_files){app::FileManagementTask task;task.job=fresh_id();task.target=task.job;task.kind=id==pending_files?app::FileManagementTaskKind::scan_pending:app::FileManagementTaskKind::scan_offline;start(task,Purpose::scan);return;}
    // Historical recovery commands are not exposed as FM menu operations.
    if(id==legacy_files||id==recovery_files)return;
    if(id==recovery_files){FormField field;field.label=L"Unfinished work";field.kind=FormFieldKind::choice;field.selected=0;field.choices={{0,L"File operations"},{1,L"File imports"},{2,L"Upload cleanup"}};const auto choice=show_form(window,appearance,L"Review Unfinished Work",L"Select existing work to review. Starting a new operation is not a recovery action.",{field});if(choice){recovery_kind=(*choice)[0].selected==0?app::FileManagementQueryKind::operations:(*choice)[0].selected==1?app::FileManagementQueryKind::imports:app::FileManagementQueryKind::intake;screen=Screen::recovery;load();}return;}
    if(id==back){go_back();return;}if(id==open_item){navigate();return;}if(id==details){show_details();return;}if(id==rename_file){if(screen==Screen::files)rename_or_move(false);return;}if(id==move_file){if(screen==Screen::search)rename_or_move(true);return;}if(id==remove_file){remove_selected();return;}
    if(id==contents){const auto picks=selected();if(picks.size()!=1||picks[0]>=rows.size())return;app::FileManagementTask task;task.job=fresh_id();task.kind=app::FileManagementTaskKind::open_view;task.view_file=rows[picks[0]].file.id;task.view_archive=true;start(task,Purpose::view);return;}
    if((id==reconcile_job||id==cancel_intent)&&screen==Screen::recovery){const auto picks=selected();if(picks.size()!=1||picks[0]>=unresolved.size())return;const auto& row=unresolved[picks[0]];app::FileManagementTask task;task.job=fresh_id();task.target=row.operation;
        if(recovery_kind==app::FileManagementQueryKind::intake){if(id!=reconcile_job||row.state!=static_cast<unsigned>(app::FileIntakeState::accepted)){message(L"This upload requires explicit retained-upload review. No cleanup was attempted.");return;}task.kind=app::FileManagementTaskKind::reconcile_intake;}
        else task.kind=recovery_kind==app::FileManagementQueryKind::imports?(id==reconcile_job?app::FileManagementTaskKind::reconcile_import:app::FileManagementTaskKind::cancel_import):(id==reconcile_job?app::FileManagementTaskKind::reconcile_operation:app::FileManagementTaskKind::cancel_operation);
        if(show_readonly(window,appearance,L"Confirm Unfinished Work",wide(row.filename)+L"\n"+wide(row.detail)+L"\n\nContinue this exact recorded operation?",true))start(task,Purpose::reconcile);return;
    }
}
void State::key(WPARAM key){
    if(!available())return;
    if(key=='A'&&(GetKeyState(VK_CONTROL)&0x8000)){if(screen==Screen::files&&area.value){ListView_SetItemState(control(entries),-1,LVIS_SELECTED,LVIS_SELECTED);tagged=true;enable();}else message(L"Ctrl+A selects all files while browsing one File Area.");return;}
    if(key==VK_DELETE){command(remove_file);return;}if(key==VK_F2){command(rename_file);return;}if(key==VK_RETURN){navigate();return;}if(key==VK_ESCAPE){go_back();return;}
    const auto current=focused();if(current<0)return;
    if(key==VK_SPACE&&file_screen()){tagged=true;const auto selected=ListView_GetItemState(control(entries),current,LVIS_SELECTED);ListView_SetItemState(control(entries),current,selected?0:LVIS_SELECTED,LVIS_SELECTED);enable();return;}
    if(key==VK_UP||key==VK_DOWN){const int row=(std::max)(0,(std::min)(ListView_GetItemCount(control(entries))-1,current+(key==VK_UP?-1:1)));if(!tagged){ListView_SetItemState(control(entries),-1,0,LVIS_SELECTED);ListView_SetItemState(control(entries),row,LVIS_SELECTED,LVIS_SELECTED);}ListView_SetItemState(control(entries),current,0,LVIS_FOCUSED);ListView_SetItemState(control(entries),row,LVIS_FOCUSED,LVIS_FOCUSED);ListView_EnsureVisible(control(entries),row,FALSE);enable();}
}
LRESULT CALLBACK State::list_keys(HWND handle,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){auto* state=reinterpret_cast<State*>(data);if(!state->available()&&(msg==WM_KEYDOWN||msg==WM_CHAR||msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP||msg==WM_LBUTTONDBLCLK||msg==WM_RBUTTONDOWN||msg==WM_RBUTTONUP||msg==WM_MOUSEWHEEL))return 0;const bool ours=w==VK_SPACE||w==VK_DELETE||w==VK_F2||w==VK_RETURN||w==VK_ESCAPE||w==VK_UP||w==VK_DOWN||(w=='A'&&(GetKeyState(VK_CONTROL)&0x8000));if(msg==WM_GETDLGCODE&&ours)return DefSubclassProc(handle,msg,w,l)|DLGC_WANTMESSAGE;if(msg==WM_KEYDOWN&&ours){state->key(w);return 0;}if(msg==WM_LBUTTONDOWN&&(GetKeyState(VK_CONTROL)&0x8000||GetKeyState(VK_SHIFT)&0x8000))state->tagged=true;if(msg==WM_NCDESTROY)RemoveWindowSubclass(handle,list_keys,id);return DefSubclassProc(handle,msg,w,l);}
void State::close(){
    if(closing)return;if(tracking.blocks_edits()&&!show_readonly(window,appearance,L"Close File Manager",L"A job is running or its status is unknown. Closing does not undo completed changes. Recovery records are kept. Close File Manager?",true))return;
    stop_batch=true;closing=true;preview->close();enable();
}
bool State::create(){
    const auto dc=GetDC(window);if(dc){dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(window,dc);}if(dpi<=0)dpi=96;
    NONCLIENTMETRICSW metrics{sizeof(metrics)};if(!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0))return false;font=CreateFontIndirectW(&metrics.lfMessageFont);
    const auto add=[&](const wchar_t* cls,const wchar_t* label,DWORD style,UINT id){auto child=CreateWindowExW(0,cls,label,WS_CHILD|WS_VISIBLE|(wcscmp(cls,L"STATIC")?WS_TABSTOP:0)|style,0,0,10,10,window,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),instance,nullptr);ins::require(child!=nullptr,"file_manager_control_unavailable");controls[id]=child;SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);};
    add(L"STATIC",L"File Manager",SS_LEFT|SS_NOPREFIX,information);
    const std::array<std::pair<UINT,const wchar_t*>,4> operations{{{browse,L"Browse"},{search_files,L"Search Filenames"},{pending_files,L"Search Pending Files"},{offline_files,L"Search Offline Files"}}};for(const auto& item:operations)add(L"BUTTON",item.second,BS_PUSHBUTTON,item.first);
    add(WC_LISTVIEWW,L"",WS_BORDER|LVS_REPORT|LVS_SHOWSELALWAYS,entries);ListView_SetExtendedListViewStyle(control(entries),LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    for(unsigned n=0;n<4;++n){LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;column.cx=MulDiv(n==0?210:n==1?290:n==2?105:220,dpi,96);column.pszText=const_cast<wchar_t*>(n==0?L"Filename / selection":n==1?L"Description":n==2?L"Bytes":L"File Group / File Area");ListView_InsertColumn(control(entries),static_cast<int>(n),&column);}SetWindowSubclass(control(entries),list_keys,1,reinterpret_cast<DWORD_PTR>(this));
    for(const auto& item:std::array<std::pair<UINT,const wchar_t*>,12>{{{back,L"Back"},{open_item,L"Open"},{rename_file,L"Rename"},{move_file,L"Move"},{remove_file,L"Remove"},{details,L"View Details"},{contents,L"ZIP Contents"},{reconcile_job,L"Finish"},{cancel_intent,L"Cancel intent"},{status_check,L"Check job status"},{retry_same,L"Retry same request"},{cancel_job,L"Cancel remaining work"}}})add(L"BUTTON",item.second,BS_PUSHBUTTON,item.first);
    add(L"BUTTON",L"Hatch File...",BS_PUSHBUTTON,hatch_file);
    add(L"STATIC",L"Connecting...",SS_LEFT|SS_NOPREFIX,status_line);
    auto menu=CreateMenu(),file=CreatePopupMenu(),help=CreatePopupMenu();AppendMenuW(file,MF_STRING,IDCANCEL,L"Close");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(file),L"File");AppendMenuW(help,MF_STRING,40001,L"File Manager help");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(help),L"Help");SetMenu(window,menu);
    worker=std::make_unique<ClientWorker>(root,owner,factory);preview=std::make_unique<PreviewWindow>(instance,window,appearance,[this](ViewNavigation n){view_navigation(n);});apply_file_manager_visuals(window,appearance);show();SetTimer(window,1,100,nullptr);ClientCommand request;request.kind=ClientCommandKind::connect;send(request);return true;
}
LRESULT CALLBACK State::proc(HWND handle,UINT msg,WPARAM w,LPARAM l){auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(handle,GWLP_USERDATA));if(msg==WM_NCCREATE){state=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);state->window=handle;SetWindowLongPtrW(handle,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));}if(!state)return DefWindowProcW(handle,msg,w,l);LRESULT themed{};if(state->appearance.control_message(handle,msg,w,l,themed))return themed;
    try{switch(msg){case WM_CREATE:return state->create()?0:-1;case WM_SIZE:state->layout();return 0;case WM_TIMER:state->tick();return 0;case WM_COMMAND:if(HIWORD(w)==BN_CLICKED||HIWORD(w)==0)state->command(LOWORD(w));return 0;
    case WM_NOTIFY:{const auto* notice=reinterpret_cast<NMHDR*>(l);if(notice->hwndFrom==state->control(entries)){if(notice->code==NM_DBLCLK)state->navigate();if(notice->code==LVN_ITEMCHANGED&&!state->refreshing)state->enable();}return 0;}
    case WM_ERASEBKGND:{RECT r{};GetClientRect(handle,&r);FillRect(reinterpret_cast<HDC>(w),&r,state->appearance.background_brush());return 1;}
    case WM_GETMINMAXINFO:{auto* bounds=reinterpret_cast<MINMAXINFO*>(l);bounds->ptMinTrackSize={MulDiv(720,state->dpi,96),MulDiv(500,state->dpi,96)};return 0;}
    case WM_CLOSE:state->close();return 0;case WM_DESTROY:KillTimer(handle,1);if(state->persist_placement)state->placement.save(handle);PostQuitMessage(0);return 0;case WM_NCDESTROY:state->window=nullptr;SetWindowLongPtrW(handle,GWLP_USERDATA,0);break;
    default:if(msg==WM_SETTINGCHANGE||(msg&&msg==win::DesktopAppearance::changed_message())){state->appearance.reload();apply_file_manager_visuals(handle,state->appearance);}break;}
    }catch(...){state->tracking.link_failed();state->loading=false;state->stop_batch=true;state->message(L"Action unavailable. Check the current job status before retrying.");state->enable();if(msg==WM_CREATE)return -1;}
    return DefWindowProcW(handle,msg,w,l);
}
} // namespace
int run_file_manager_ui(HINSTANCE instance,const std::filesystem::path& root,const std::wstring& owner,ClientWorker::Factory factory){
    State state(instance,root,owner,std::move(factory));WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=State::proc;cls.hInstance=instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));cls.lpszClassName=manager_class;ins::require(RegisterClassExW(&cls)!=0,"file_manager_window_registration_failed");
    RECT initial{50,40,1080,750};bool maximized{};if(const auto saved=state.placement.load(720,500)){initial=saved->rectangle;maximized=saved->maximized;}
    auto window=CreateWindowExW(0,manager_class,L"Spitfire-X File Manager",WS_OVERLAPPEDWINDOW,initial.left,initial.top,initial.right-initial.left,initial.bottom-initial.top,nullptr,nullptr,instance,&state);
    if(!window){UnregisterClassW(manager_class,instance);return 1;}ShowWindow(window,maximized?SW_SHOWMAXIMIZED:SW_SHOW);UpdateWindow(window);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(state.preview&&state.preview->translate(message))continue;if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    state.preview.reset();state.worker.reset();UnregisterClassW(manager_class,instance);return 0;
}
} // namespace sfx::file_manager
