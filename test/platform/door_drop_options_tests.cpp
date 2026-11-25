// Actual native combo selection, protected-model saves and stable record IDs.
#define main retained_drop_options_main
#include "door_command_line_early.cpp"
#undef main

namespace {
const std::pair<const wchar_t*,app::DoorDropFile> drop_expected[]{
    {L"None",app::DoorDropFile::none},{L"XTRN.DAT",app::DoorDropFile::xtrn_dat},
    {L"CHAIN.TXT",app::DoorDropFile::chain_txt},{L"DOOR.SYS (52 lines)",app::DoorDropFile::door_sys52},
    {L"DORINFO#.DEF",app::DoorDropFile::dorinfo_node},{L"SFDOORS.DAT",app::DoorDropFile::sfdoors_dat},
    {L"DOOR.SYS (31 lines)",app::DoorDropFile::door_sys31},{L"DOORFILE.SR",app::DoorDropFile::doorfile_sr},
    {L"DORINFO1.DEF",app::DoorDropFile::dorinfo1_def},{L"DOOR32.SYS",app::DoorDropFile::door32_sys},
    {L"BBSDEV.DRP",app::DoorDropFile::bbsdev_drp}};
void drop_check_list(HWND combo){
    check(SendMessageW(combo,CB_GETCOUNT,0,0)==11,"exact shorter drop list");
    for(unsigned i=0;i<11;++i){
        wchar_t caption[64]{};SendMessageW(combo,CB_GETLBTEXT,i,reinterpret_cast<LPARAM>(caption));
        check(std::wstring(caption)==drop_expected[i].first,"exact expected format label");
        check(SendMessageW(combo,CB_GETITEMDATA,i,0)==static_cast<LRESULT>(drop_expected[i].second),
              "remaining rows carry unchanged stable format IDs");
    }
    for(const wchar_t* removed:{L"CALLINFO.BBS",L"PCBOARD.SYS",L"TRIBBS.SYS",L"BBSDEV.DRP 1.0"})
        check(SendMessageW(combo,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(removed))==CB_ERR,
              "removed choices and obsolete BBSDEV label are absent");
}
void drop_begin_new(State& s){
    seed();auto& d=s.doors;d.menu=2;d.entry=0;d.navigation_placement.reset();d.view=DoorPage::entries;
    refresh_door_configuration_page(s);remember_page(s);
    SendMessageW(d.entries,LB_SETCURSEL,static_cast<WPARAM>(-1),0);press(s,IDC_DOOR_NEW);
    door_set(d.name,"Drop format fixture");door_set(d.code,"DROP_FIXTURE");
    door_set(d.startup,"C:\\Games\\Synthetic");door_set(d.command_line,"fixture.exe");
}
}
int main(){try{
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};
    check(InitCommonControlsEx(&common)!=FALSE,"native control classes");
    for(const int dpi:{96,144,192}){
        State s;s.dpi=dpi;s.instance=GetModuleHandleW(nullptr);s.root=L"C:\\sfxtst\\drop-options-synthetic-only";
        s.window=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Owned drop-file options probe",
            WS_POPUP|WS_CLIPCHILDREN,-30000,-30000,1080,860,nullptr,nullptr,s.instance,nullptr);
        check(s.window&&create_ui(s),"hidden actual SFXConfig controls");
        door_catalog_fixture=snapshot;door_change_fixture=change;show_page(s,26);auto& d=s.doors;
        for(unsigned row=0;row<11;++row){
            drop_begin_new(s);const auto id=d.door;
            check(SendMessageW(d.drop_file,CB_GETCURSEL,0,0)==0,"new Door still defaults to None");
            if(row==0)drop_check_list(d.drop_file);
            SendMessageW(d.drop_file,CB_SETCURSEL,row,0);
            check(save_door_configuration_page(s),"selected format saves successfully");
            const auto* saved=app::find_door(catalog,id);
            check(saved&&saved->program.drop_file==drop_expected[row].second,"save uses stable ID instead of shortened row number");
            const auto bytes=app::encode_door_catalog(catalog);const auto decoded=app::decode_door_catalog(bytes);
            check(decoded&&app::find_door(*decoded,id)->program.drop_file==drop_expected[row].second,
                  "record/IPC round trip preserves each selected format");
            load_door_fields(s);
            check(SendMessageW(d.drop_file,CB_GETCURSEL,0,0)==static_cast<LRESULT>(row)&&text(d.drop_file)==drop_expected[row].first,
                  "reopening selects correct label after the removed rows");
            check(SendMessageW(d.drop_file,CB_GETCOUNT,0,0)==11,"removed options do not return when reopening");
        }
        drop_begin_new(s);const auto unchanged=app::encode_door_catalog(catalog);const auto writes=changes;
        SendMessageW(d.drop_file,CB_SETCURSEL,static_cast<WPARAM>(-1),0);
        check(!save_door_configuration_page(s)&&changes==writes&&app::encode_door_catalog(catalog)==unchanged,
              "missing drop selection reports a field error without saving the wrong ID");
        drop_begin_new(s);SendMessageW(d.program_type,CB_SETCURSEL,1,0);
        door_configuration_command(s,IDC_DOOR_PROGRAM_TYPE,CBN_SELCHANGE);
        SendMessageW(d.io,CB_SETCURSEL,1,0);SendMessageW(d.drop_file,CB_SETCURSEL,9,0);
        const auto native_id=d.door;check(save_door_configuration_page(s),"native Socket Door saves with DOOR32 row9");
        const auto* native=app::find_door(catalog,native_id);
        check(native&&native->program.type==app::DoorProgramType::native&&native->program.io==app::DoorIo::socket&&
              native->program.drop_file==app::DoorDropFile::door32_sys,"Usurper/Blocks configuration retains stable DOOR32 ID12");
        DestroyWindow(s.window);DeleteObject(s.heading_font);DeleteObject(s.ui_font);
        door_catalog_fixture=nullptr;door_change_fixture=nullptr;
    }
    std::cout<<"PASS "<<checks<<" drop options checks; exact11 choices, all stable IDs save/reopen/codec, no retired rows, native Socket DOOR32; DPI96/144/192; no renders or product rebuild\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
