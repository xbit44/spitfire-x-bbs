// Actual native picker/dialog and, in --installed mode, the shipping Runtime
// client and sound timer path. Uploaded WAVs are read-only local inputs.
#define SFX_OWNER_SOUND_TEST
#include "event_sounds_picker_tests.cpp"

namespace {
unsigned timer_ticks{};
LRESULT CALLBACK sound_test_timer(HWND window,UINT message,WPARAM w,LPARAM l){
    if(message==WM_TIMER){auto* state=reinterpret_cast<AppState*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(state){update_event_sounds(*state);++timer_ticks;}return 0;}
    return DefWindowProcW(window,message,w,l);
}
std::vector<unsigned char> file_bytes(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void put_bytes(const std::filesystem::path& p,const std::vector<unsigned char>& b){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());}
void size32(std::vector<unsigned char>& b,std::size_t at,unsigned value){for(int i=0;i<4;++i)b[at+i]=static_cast<unsigned char>(value>>(i*8));}
}
int main(int argc,char** argv){
    std::atomic<bool> finished{};std::thread watchdog([&]{for(int i=0;i<900&&!finished;++i)Sleep(100);if(!finished)ExitProcess(97);});int code=1;
    try{
        check(argc==3||argc==4,"evidence output, supplied WAV directory and optional --installed");
        const auto out=std::filesystem::absolute(argv[1]).lexically_normal(),samples=std::filesystem::absolute(argv[2]).lexically_normal();
        const auto repo=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
        check(!out.lexically_relative(repo/L"build").empty()&&out.lexically_relative(repo/L"build").native().find(L"..")==std::wstring::npos&&!std::filesystem::exists(out),"new bounded build evidence directory");
        std::filesystem::create_directories(out/L"Config");
        const bool installed=argc==4&&std::string(argv[3])=="--installed";
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&controls)!=FALSE,"native controls ready");
        const auto sid=win32::current_process_user_sid_string();win32::WindowsStorageDaclPolicy policy(sid);
        check(policy.apply(out/L"Config",win32::StorageDaclObjectKind::directory).succeeded(),"private diagnostic configuration");
        const auto valid=out/L"plain.wav";write_wave(valid);const auto original=file_bytes(valid);
        std::vector<std::pair<std::wstring,bool>> variants;
        auto metadata=original;metadata.insert(metadata.end(),{'C','D','i','f',3,0,0,0,1,2,3,0});size32(metadata,4,static_cast<unsigned>(metadata.size()-8));
        variants.push_back({L"bounded-odd-metadata.wav",true});put_bytes(out/variants.back().first,metadata);
        auto extension=original;extension.insert(extension.begin()+36,2,0);size32(extension,16,18);size32(extension,4,static_cast<unsigned>(extension.size()-8));
        variants.push_back({L"pcm-fmt18.wav",true});put_bytes(out/variants.back().first,extension);
        extension[36]=1;variants.push_back({L"invalid-extension.wav",false});put_bytes(out/variants.back().first,extension);
        metadata.pop_back();size32(metadata,4,static_cast<unsigned>(metadata.size()-8));variants.push_back({L"missing-padding.wav",false});put_bytes(out/variants.back().first,metadata);
        metadata=original;metadata.insert(metadata.end(),{'f','a','c','t',255,255,255,127});size32(metadata,4,static_cast<unsigned>(metadata.size()-8));variants.push_back({L"oversized-metadata.wav",false});put_bytes(out/variants.back().first,metadata);
        metadata=original;metadata.insert(metadata.end(),original.begin()+12,original.begin()+36);size32(metadata,4,static_cast<unsigned>(metadata.size()-8));variants.push_back({L"duplicate-format.wav",false});put_bytes(out/variants.back().first,metadata);
        metadata=original;metadata[20]=3;variants.push_back({L"compressed-format.wav",false});put_bytes(out/variants.back().first,metadata);
        metadata=original;size32(metadata,28,1);variants.push_back({L"invalid-rate.wav",false});put_bytes(out/variants.back().first,metadata);
        for(const auto& v:variants)check(static_cast<bool>(win32::read_event_sound((out/v.first).wstring()))==v.second,"bounded metadata/format negative and positive control");
        std::vector<std::filesystem::path> waves{samples/L"auth_d.wav"};for(int i=1;i<=10;++i)waves.push_back(samples/(L"Alarm"+std::wstring(i<10?L"0":L"")+std::to_wstring(i)+L".wav"));
        std::vector<std::vector<unsigned char>> preimages;
        for(const auto& p:waves){preimages.push_back(file_bytes(p));check(!preimages.back().empty()&&static_cast<bool>(win32::read_event_sound(p.wstring())),"supplied original PCM WAV accepted including CDif metadata");win32::EventSoundPlayer player;std::wstring detail;check(player.preview(p.wstring(),detail),"supplied WAV accepted by actual Windows async preview");player.stop();}
        app::OperationalConfiguration baseline;baseline.bbs_time_zone="Pacific Standard Time";baseline.security_policy_revision=1;baseline.security_policy_hash=std::string(64,'a');
        AppState state;std::string diagnostic;std::optional<app::AlphaConfigurationSnapshot> restore;HWND timer{};
        struct Stop {AppState& state;HWND& timer;~Stop(){if(timer)DestroyWindow(timer);std::string diagnostic;if(state.runtime.child_alive())state.runtime.shutdown(diagnostic);}} stop{state,timer};
        if(installed){
            wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);
            check(std::filesystem::path(module).parent_path()==std::filesystem::path(L"C:/Program Files/SFX-Section13-Test-20261028-r1/Bin"),"installed mode restricted to own disposable Setup fixture");
            check(state.runtime.launch(diagnostic),"actual shipping RuntimeClient launches installed Runtime");restore=state.runtime.configuration_snapshot();check(restore.has_value(),"actual protected configuration snapshot");
            auto blank=restore->saved;blank.event_sounds={};++blank.configuration_revision;check(state.runtime.update_configuration({restore->saved.configuration_revision,blank}).code==app::LocalControlResponseCode::ok,"isolate only fixture sound assignments");
            state.sound_preview_open=true;WNDCLASSW wc{};wc.lpfnWndProc=sound_test_timer;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SfxSoundOwnerTimer";RegisterClassW(&wc);
            timer=CreateWindowW(wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,nullptr);check(timer!=nullptr,"owned Panel sound timer window");SetWindowLongPtrW(timer,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&state));SetTimer(timer,1,50,nullptr);
        }else check(win32::publish_operational_configuration_file(out,sid,baseline).succeeded(),"private local publication baseline");
        const auto configure=[&](EventSoundsDialog& model){
            model.confirm_exit=[](HWND){return IDNO;};
            model.load=[&]()->std::optional<app::AlphaConfigurationSnapshot>{if(installed)return state.runtime.configuration_snapshot();const auto disk=win32::read_operational_configuration_file(out);if(!disk.configuration)return {};return app::AlphaConfigurationSnapshot{*disk.configuration,*disk.configuration};};
            model.save=[&](const app::AlphaConfigurationUpdateRequest& r){++saves;if(installed)return state.runtime.update_configuration(r);const auto disk=win32::read_operational_configuration_file(out);check(disk.configuration&&r.expected_revision==disk.configuration->configuration_revision,"last loaded revision used");const auto saved=win32::publish_operational_configuration_file(out,sid,r.configuration);app::AlphaConfigurationUpdateResult result;result.code=saved?app::LocalControlResponseCode::ok:app::LocalControlResponseCode::configuration_failure;result.detail=saved?"Applied":"Refused";return result;};
        };
        EventSoundsDialog model;configure(model);
        modal_walk(model,[&](HWND dialog){
            ShowWindow(dialog,SW_SHOWNOACTIVATE);pump();
            check(window_text(GetDlgItem(dialog,IDCANCEL))==L"Exit","plain Exit button");
            RECT client{};GetClientRect(dialog,&client);check(client.right<850&&client.bottom<550,"compact normal-DPI default client size");
            for(std::size_t i=0;i<waves.size();++i){const auto row=i%10;real_picker(dialog,model,row,waves[i].wstring(),false);check(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST+row))==waves[i].wstring(),"real Browse displays exact owner selection");SendMessageW(dialog,WM_COMMAND,IDOK,0);const auto current=model.load();check(current&&current->saved.event_sounds.paths[row]==wide_to_utf8(waves[i].wstring())&&current->active.event_sounds.paths[row]==current->saved.event_sounds.paths[row],"Save immediately updates durable and active mapping without restarting");}
            for(int cycle=0;cycle<3;++cycle){sound_select(dialog,model,0);SendMessageW(dialog,WM_COMMAND,IDC_SOUND_CLEAR,0);SendMessageW(dialog,WM_COMMAND,IDOK,0);check(model.load()->saved.event_sounds.paths[0].empty(),"Clear/Save persists silence while process stays running");real_picker(dialog,model,0,waves[cycle].wstring(),false);SendMessageW(dialog,WM_COMMAND,IDOK,0);check(model.load()->saved.event_sounds.paths[0]==wide_to_utf8(waves[cycle].wstring()),"reassign after Clear persists immediately in same process");}
            const auto saved=model.load()->saved.event_sounds;const auto count=saves;const auto invalid=(out/L"compressed-format.wav").wstring();real_picker(dialog,model,0,invalid,false);
            check(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST))==invalid&&window_text(GetDlgItem(dialog,IDC_SOUND_STATUS)).find(L"Not saved")!=std::wstring::npos,"invalid Browse stays visible with rejection");
            SendMessageW(dialog,WM_COMMAND,IDOK,0);check(saves==count&&model.load()->saved.event_sounds==saved&&window_text(GetDlgItem(dialog,IDC_SOUND_STATUS)).find(L"No changes saved")!=std::wstring::npos,"Save refuses invalid replacement instead of republishing old assignment as Applied");
            State capture{dialog,g_appearance};render_png(capture,out/L"invalid-selection.png");
            model.confirm_exit=[](HWND){return IDYES;};SendMessageW(dialog,WM_COMMAND,IDCANCEL,0);check(sound_dirty(dialog,model)&&saves==count&&window_text(GetDlgItem(dialog,IDC_SOUND_STATUS)).find(L"No changes saved")!=std::wstring::npos,"Exit/Save remains on invalid draft");
            real_picker(dialog,model,0,waves[0].wstring(),false);SendMessageW(dialog,WM_COMMAND,IDOK,0);const auto stable=saves;SendMessageW(dialog,WM_COMMAND,IDOK,0);check(saves==stable&&window_text(GetDlgItem(dialog,IDC_SOUND_STATUS)).find(L"No assignment changes")!=std::wstring::npos,"no-op Save is truthful and does not republish");
            render_png(capture,out/L"compact-saved.png");
            if(installed){for(int wait=0;wait<10;++wait){pump();Sleep(20);}}
            if(installed)check(timer_ticks>0&&state.runtime.child_alive()&&state.sound_mappings==model.load()->active.event_sounds,"actual Panel polling ran through picker messages and sees live saved mappings");
        });
        EventSoundsDialog reopened;configure(reopened);modal_walk(reopened,[&](HWND dialog){ShowWindow(dialog,SW_SHOWNOACTIVATE);pump();check(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST))==waves[0].wstring(),"same-process reopen reads latest saved replacement");SetWindowTextW(sound_field(dialog,IDC_SOUND_PATH_FIRST),valid.c_str());reopened.confirm_exit=[](HWND){return IDCANCEL;};SendMessageW(dialog,WM_COMMAND,IDCANCEL,0);check(sound_dirty(dialog,reopened),"Exit/Cancel keeps unsaved changes");reopened.confirm_exit=[](HWND){return IDYES;};SendMessageW(dialog,WM_COMMAND,IDCANCEL,0);check(reopened.load()->saved.event_sounds.paths[0]==wide_to_utf8(valid.wstring()),"Exit/Save persists valid draft");});
        if(installed){auto current=state.runtime.configuration_snapshot();check(current.has_value(),"pre-restart snapshot");check(state.runtime.restart(diagnostic),"actual Runtime restart");const auto restarted=state.runtime.configuration_snapshot();check(restarted&&restarted->active.event_sounds==current->saved.event_sounds&&restarted->saved.event_sounds==current->saved.event_sounds,"latest replacement survives actual Runtime restart");auto original_config=restarted->saved;original_config.event_sounds=restore->saved.event_sounds;++original_config.configuration_revision;check(state.runtime.update_configuration({restarted->saved.configuration_revision,original_config}).code==app::LocalControlResponseCode::ok,"restore fixture sound mappings only");}
        for(std::size_t i=0;i<waves.size();++i)check(file_bytes(waves[i])==preimages[i],"uploaded audio original remains byte-exact");
        std::cout<<"PASS "<<checks<<" owner sound checks; installed="<<installed<<"; Panel timer ticks="<<timer_ticks<<"; no physical audible acceptance claimed\n";code=0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<std::endl;}
    finished=true;watchdog.join();return code;
}
