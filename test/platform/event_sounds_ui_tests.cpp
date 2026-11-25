// Actual product dialog/procedure and production configuration publisher.
// Runtime IPC and event producers are deliberately outside this EARLY UI gate.
#define wWinMain retained_control_panel_entry
#include "../../src/apps/control_panel/main.cpp"
#undef wWinMain
#include <mmsystem.h>
#include <fstream>
#include <iostream>
#include <new>
namespace {
unsigned checks{},saves{},audio_success{},audio_refused{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct State {HWND window;win32::DesktopAppearance& appearance;};
void pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
void write_wave(const std::filesystem::path& p){
    std::vector<unsigned char> b;const auto text=[&](const char* s){b.insert(b.end(),s,s+4);};
    const auto u16=[&](unsigned n){b.push_back(static_cast<unsigned char>(n));b.push_back(static_cast<unsigned char>(n>>8));};
    const auto u32=[&](unsigned n){for(int i=0;i<4;++i)b.push_back(static_cast<unsigned char>(n>>(i*8)));};
    text("RIFF");u32(36+800);text("WAVE");text("fmt ");u32(16);u16(1);u16(1);u32(8000);u32(8000);u16(1);u16(8);text("data");u32(800);
    // 100ms quiet PCM fixture, never distributed in a product/archive.
    b.resize(844,128);std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());
}
}
#include "native_png_capture.hpp"
int main(int argc,char** argv){try{
    check(argc==2 || (argc==3 && std::string(argv[2])=="--interactive"),"fresh owned evidence directory required");const auto out=std::filesystem::absolute(argv[1]).lexically_normal();
    const auto repo=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    check(out.lexically_relative(repo/L"build").native().find(L"..")==std::wstring::npos && out!=repo/L"build" && !std::filesystem::exists(out),"new bounded build evidence root");
    std::filesystem::create_directories(out/L"Config");const auto sid=win32::current_process_user_sid_string();win32::WindowsStorageDaclPolicy policy(sid);
    check(policy.apply(out/L"Config",win32::StorageDaclObjectKind::directory).succeeded(),"actual private Config DACL");
    const auto wav=out/L"preview.wav",missing=out/L"missing.wav",bad=out/L"invalid.wav";write_wave(wav);{std::ofstream f(bad);f<<"not a wave";}
    check(static_cast<bool>(win32::read_event_sound(wav.wstring())),"actual safe PCM file reader");
    check(win32::read_event_sound(wav.wstring()).duration_ms==100,"validated PCM duration bounds async playback lifetime");
    {win32::EventSoundPlayer live;std::wstring detail;
        if(live.preview(wav.wstring(),detail)){
            check(live.playing()&&live.owns_buffer(),"async player retains buffer while live");Sleep(250);
            check(!live.playing()&&live.owns_buffer(),"finished playback is detectable without freeing active memory");
        }else check(!live.playing()&&!detail.empty(),"unavailable device is nonfatal and explained");
        live.stop();check(!live.owns_buffer()&&!live.playing(),"Stop releases async buffer and lifetime");
    }
    for(const auto& p:std::vector<std::wstring>{missing.wstring(),bad.wstring(),L"\\\\server\\file.wav",L"C:\\x.wav:stream",L"C:\\..\\x.wav"})check(!win32::read_event_sound(p),"missing invalid network ADS traversal refuse");
    app::OperationalConfiguration baseline;baseline.bbs_time_zone="Pacific Standard Time";baseline.security_policy_revision=1;baseline.security_policy_hash=std::string(64,'a');
    check(win32::publish_operational_configuration_file(out,sid,baseline).succeeded(),"actual authoritative configuration publication");
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&controls)!=FALSE,"native common controls");
    if(argc==3){
        EventSoundsDialog model;
        model.load=[&]()->std::optional<app::AlphaConfigurationSnapshot>{const auto disk=win32::read_operational_configuration_file(out);if(!disk.configuration)return {};return app::AlphaConfigurationSnapshot{*disk.configuration,*disk.configuration};};
        model.save=[&](const app::AlphaConfigurationUpdateRequest& r){const auto disk=win32::read_operational_configuration_file(out);app::AlphaConfigurationUpdateResult result;
            if(!disk.configuration || disk.configuration->configuration_revision!=r.expected_revision){result.detail="Refused - configuration changed; reopen before saving";return result;}
            const auto saved=win32::publish_operational_configuration_file(out,sid,r.configuration);result.code=saved?app::LocalControlResponseCode::ok:app::LocalControlResponseCode::configuration_failure;result.detail=saved?"Applied":"Refused - configuration could not be saved";
            if(saved)++saves;std::cout<<"INTERACTIVE publication result="<<static_cast<unsigned>(result.code)<<" saved="<<saves<<std::endl;return result;
        };
        std::cout<<"INTERACTIVE native product window; real WAV chooser/player/publisher, synthetic Runtime callbacks. Fixture "<<out<<std::endl;
        DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_EVENT_SOUNDS),nullptr,event_sounds_proc,reinterpret_cast<LPARAM>(&model));
        std::cout<<"PASS interactive dialog closed; saved="<<saves<<" audio buffer released="<<!model.player.owns_buffer()<<std::endl;return 0;
    }
    for(unsigned theme=0;theme<2;++theme)for(const unsigned dpi:{96U,144U,192U}){
        const auto tag=std::to_string(theme)+"-"+std::to_string(dpi);const auto key=L"Software\\Spitfire-X BBS\\Tests\\Section13-"+out.filename().wstring()+L"-"+std::to_wstring(theme)+L"-"+std::to_wstring(dpi);
        HKEY handle{};DWORD disposition{};check(RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&handle,&disposition)==ERROR_SUCCESS && disposition==REG_CREATED_NEW_KEY,"isolated theme key only");
        const DWORD value=theme;check(RegSetValueExW(handle,L"Theme",0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value))==ERROR_SUCCESS,"test theme");RegCloseKey(handle);
        g_appearance.~DesktopAppearance();new(&g_appearance)win32::DesktopAppearance(key);g_automation_dpi=dpi;
        EventSoundsDialog model;model.confirm_exit=[](HWND){return IDNO;};bool reject{};unsigned browse_calls{};
        model.load=[&]()->std::optional<app::AlphaConfigurationSnapshot>{const auto disk=win32::read_operational_configuration_file(out);if(!disk.configuration)return {};return app::AlphaConfigurationSnapshot{*disk.configuration,*disk.configuration};};
        model.save=[&](const app::AlphaConfigurationUpdateRequest& request){++saves;const auto disk=win32::read_operational_configuration_file(out);
            // Round-trip actual protected-channel payload codecs; authority itself is qualified after this early gate.
            const auto bytes=app::encode_configuration_update_request(request);const auto decoded=app::decode_configuration_update_request(bytes);
            check(decoded && app::equivalent_operational_configuration(decoded->configuration,request.configuration),"real request codec exact settings");
            if(reject || !disk.configuration || disk.configuration->configuration_revision!=request.expected_revision)return app::AlphaConfigurationUpdateResult{app::LocalControlResponseCode::invalid_request,"configuration_revision_conflict",app::AlphaConfigurationActivation::refused,0,0,"Refused - configuration changed; reload before saving"};
            const auto result=win32::publish_operational_configuration_file(out,sid,request.configuration);
            return app::AlphaConfigurationUpdateResult{result?app::LocalControlResponseCode::ok:app::LocalControlResponseCode::configuration_failure,"",app::AlphaConfigurationActivation::applied,request.configuration.configuration_revision,request.configuration.configuration_revision,result?"Applied":"Refused - configuration could not be saved"};
        };
        bool cancel_browse{};
        model.browse=[&](HWND)->std::optional<std::wstring>{++browse_calls;if(cancel_browse)return {};return wav.wstring();};
        const auto dialog=CreateDialogParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_EVENT_SOUNDS),nullptr,event_sounds_proc,reinterpret_cast<LPARAM>(&model));check(dialog!=nullptr,"actual Event Sounds resource and procedure");
        // Only DPI input is overridden; shipping layout and font paths run.
        SetWindowPos(dialog,nullptr,-30000,-30000,MulDiv(940,dpi,96),MulDiv(580,dpi,96),SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(dialog,SW_SHOWNOACTIVATE);sound_layout(dialog,model);pump();
        State capture{dialog,g_appearance};render_png(capture,out/("event-sounds-"+tag+".png"));
        for(int i=0;i<10;++i){check(sound_field(dialog,IDC_SOUND_PATH_FIRST+i)&&sound_field(dialog,IDC_SOUND_BROWSE_FIRST+i),"ten native path/Browse assignments");}
        check(!model.player.owns_buffer(),"open does not play");
        const auto browse_before=window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST));cancel_browse=true;
        SendMessageW(dialog,WM_COMMAND,IDC_SOUND_BROWSE_FIRST,0);cancel_browse=false;
        check(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST))==browse_before && !model.player.owns_buffer(),"canceled chooser preserves draft without playback");
        for(int i=0;i<10;++i)SendMessageW(dialog,WM_COMMAND,IDC_SOUND_BROWSE_FIRST+i,0);
        check(browse_calls==11 && !model.player.owns_buffer(),"Browse every row without playback");
        const auto before_saves=saves;SendMessageW(dialog,WM_COMMAND,IDC_SOUND_TEST,0);
        if(model.player.owns_buffer())++audio_success;else ++audio_refused;
        check(saves==before_saves,"preview never saves or emits a BBS event");SendMessageW(dialog,WM_COMMAND,IDC_SOUND_STOP,0);check(!model.player.owns_buffer(),"Stop releases async owned audio");
        SendMessageW(dialog,WM_COMMAND,IDOK,0);check(saves==before_saves+1,"Save invokes authoritative callback once");
        const auto saved=win32::read_operational_configuration_file(out);check(saved.configuration.has_value(),"disk settings readable after Save");
        for(const auto& path:saved.configuration->event_sounds.paths)check(path==wide_to_utf8(wav.wstring()),"all ten saved mappings including BinkP");
        sound_select(dialog,model,0);SetWindowTextW(sound_field(dialog,IDC_SOUND_PATH_FIRST),missing.c_str());const auto saved_count=saves;SendMessageW(dialog,WM_COMMAND,IDOK,0);
        check(saves==saved_count && window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST))==missing.wstring(),"missing file refuses before publication and retains draft");
        render_png(capture,out/("event-sounds-error-"+tag+".png"));
        SetWindowTextW(sound_field(dialog,IDC_SOUND_PATH_FIRST),L"");reject=true;SendMessageW(dialog,WM_COMMAND,IDOK,0);check(saves==saved_count+1 && window_text(GetDlgItem(dialog,IDC_SOUND_STATUS)).find(L"Refused")!=std::wstring::npos,"save conflict is visible and leaves draft intact");reject=false;
        SendMessageW(dialog,WM_COMMAND,IDC_SOUND_CLEAR,0);check(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST)).empty() && std::filesystem::exists(wav),"Clear only clears selected draft and keeps WAV");
        SendMessageW(dialog,WM_COMMAND,IDOK,0);check(win32::read_operational_configuration_file(out).configuration->event_sounds.paths[0].empty(),"Clear then Save persists silent assignment");
        SetWindowTextW(sound_field(dialog,IDC_SOUND_PATH_FIRST),wav.c_str());SendMessageW(dialog,WM_COMMAND,IDC_SOUND_TEST,0);
        const auto cancel_saves=saves;SendMessageW(dialog,WM_COMMAND,IDCANCEL,0);check(!model.player.owns_buffer() && saves==cancel_saves,"Cancel stops preview without saving draft");
        if(IsWindow(dialog))DestroyWindow(dialog);
        // Reopen uses saved configuration, not the canceled/missing draft.
        const auto reopened=CreateDialogParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_EVENT_SOUNDS),nullptr,event_sounds_proc,reinterpret_cast<LPARAM>(&model));
        check(window_text(sound_field(reopened,IDC_SOUND_PATH_FIRST)).empty(),"reopen reads cleared saved mapping rather than canceled draft");
        SetWindowPos(reopened,nullptr,-30000,-30000,MulDiv(740,dpi,96),MulDiv(320,dpi,96),SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(reopened,SW_SHOWNOACTIVATE);sound_select(reopened,model,9);pump();
        RECT row_rect{},body_rect{};GetWindowRect(sound_field(reopened,IDC_SOUND_BROWSE_FIRST+9),&row_rect);GetClientRect(GetDlgItem(reopened,IDC_SOUND_ROWS),&body_rect);
        MapWindowPoints(HWND_DESKTOP,GetDlgItem(reopened,IDC_SOUND_ROWS),reinterpret_cast<POINT*>(&row_rect),2);
        check(model.scroll>0 && row_rect.top>=0 && row_rect.bottom<=body_rect.bottom,"short window scrolls selected last event fully into view");
        State small_capture{reopened,g_appearance};render_png(small_capture,out/("event-sounds-small-"+tag+".png"));DestroyWindow(reopened);
        RegDeleteKeyW(HKEY_CURRENT_USER,key.c_str());
    }
    // A new configuration object/process-style reader has no unsaved UI state.
    const auto reread=win32::read_operational_configuration_file(out);check(reread.configuration && reread.configuration->event_sounds.paths[9]==wide_to_utf8(wav.wstring()),"BinkP mapping survives independent disk reader");
    std::cout<<"PASS "<<checks<<" native UI/publisher checks; audio API accepted="<<audio_success<<" refused="<<audio_refused<<" devices="<<waveOutGetNumDevs()<<". No audible owner acceptance; no Runtime producers or installed IPC qualification.\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
