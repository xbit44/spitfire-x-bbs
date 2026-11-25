// Test coordinator for the actual receipt-verified installed shipping Runtime.
// Uses its normal inherited-owner channel and authenticated IPC, no GUI hooks,
// automation build, receipt bypass, or production entry-point changes.
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/bbs_time_zone_provider.hpp"
#include "sfx/application/display.hpp"
#include "../support/section7_host_commands.hpp"
#include "../support/section8a_host_commands.hpp"
void login_menu_runtime(bool welcome=false, bool polish=false, bool findings=false, bool october=false, bool newsletter=false, bool section6=false, bool section6_polish=false, bool asleep_color=false, bool section7=false, bool section7_corrections=false, bool section7_polish=false, bool section7_text=false, bool section7_followup=false, bool section8a=false, bool fresh8a=false, bool section8a_polish=false, bool section8a_corrections=false, bool section8a_login=false, bool section8a_admin=false, bool section8a_config=false, bool presentation=false, bool help=false, bool menu_followup=false) {
    namespace win=sfx::platform::win32;
    namespace app=sfx::application;
const install::Path legacy_root(section7 ? L"C:\\xtest-sfx-section7-20261004" : asleep_color ? L"C:\\xtest-sfx-asleep-color-20261004" : section6_polish ? L"C:\\xtest-sfx-section6-polish-20261004" : section6 ? L"C:\\xtest-sfx-section6-final-20261003" : newsletter ? L"C:\\xtest-sfx-newsletter-20261002" : october ? L"C:\\xtest-sfx-october1-final-20261001" : findings ? L"C:\\xtest-sfx-findings-final-20260930" : polish ? L"C:\\xtest-sfx-section5-polish-20260930" : welcome ? L"C:\\xtest-sfx-welcome-placement-20260928" : L"C:\\xtest-sfx-login-menu-final-20260928");
    const install::Path root=section8b_test::active?section8b_test::root():
    footer_format_test::active?install::Path(L"C:\\xtest-sfx-section8a-footer-qualified-r4-20261008"):
    menu_followup?install::Path(L"C:\\xtest-sfx-section8a-menu-followup-qualified-r5-20261008"):
    help?install::Path(L"C:\\xtest-sfx-section8a-help-qualified-r2-20261008"):
        presentation?install::Path(L"C:\\xtest-sfx-section8a-presentation-qualified-r4-20261007"):
        section8a_config?install::Path(L"C:\\xtest-sfx-section8a-config-qualified-r1-20261007"):
        section8a_admin?install::Path(L"C:\\xtest-sfx-section8a-admin-qualified-r4-20261007"):
        section8a_login?install::Path(L"C:\\xtest-sfx-section8a-login-qualified-20261007"):
        section8a_corrections?install::Path(L"C:\\xtest-sfx-section8a-corrections-qualified-r2-20261006"):
        section8a_polish?install::Path(L"C:\\xtest-sfx-section8a-polish-qualified-r2-20261006"):
        section8a?install::Path(L"C:\\xtest-sfx-section8a-qualified-r2-20261006"):
        section7_followup?install::Path(L"C:\\xtest-sfx-section7-followup-qualified-20261005"):
        section7_text?install::Path(L"C:\\xtest-sfx-section7-text-shipping-20261005"):
        section7_polish?install::Path(L"C:\\xtest-sfx-section7-polish-20261004"):
        section7_corrections?install::Path(L"C:\\xtest-sfx-section7-corrections-20261004"):legacy_root;
    install::require(!install::elevated(),"ordinary installed test coordinator required");
    install::verify_runtime_installation(root,install::operator_sid());
    const auto generation=install::read_receipt(root,install::operator_sid()).generation;
    install::require(section8b_test::backup_redesign ? generation >= 1 : generation==(fresh8a?1:3),"expected isolated fixture generation required");
    std::array<std::uint8_t,app::kLocalControlNonceBytes> nonce{};
    install::require(win::fill_secure_random(nonce.data(),nonce.size()),"owner nonce");
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE read{},write{};
    install::require(CreatePipe(&read,&write,&sa,0),"owner channel");
    install::Handle reader(read),writer(write);
    install::require(SetHandleInformation(write,HANDLE_FLAG_INHERIT,0),"writer not inherited");
    SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
    std::vector<std::uint8_t> memory(size);
    auto* attributes=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
    install::require(InitializeProcThreadAttributeList(attributes,1,0,&size),"process attributes");
    struct Attributes {PPROC_THREAD_ATTRIBUTE_LIST p;~Attributes(){DeleteProcThreadAttributeList(p);}} release{attributes};
    install::require(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&read,sizeof(read),nullptr,nullptr),"owner handle allowlist");
    const auto exe=root/L"Bin/SFXRuntime.exe";
    auto command=L"\""+exe.native()+L"\" --owner-handle="+std::to_wstring(reinterpret_cast<ULONG_PTR>(read));
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);
    startup.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;startup.lpAttributeList=attributes;
    PROCESS_INFORMATION process{};
    install::require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,
        EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup.StartupInfo,&process),"installed Runtime launch");
    install::Handle child(process.hProcess),thread(process.hThread);reader.reset();
    const DWORD pid=GetCurrentProcessId();DWORD written{};
    install::require(WriteFile(write,&pid,sizeof(pid),&written,nullptr)&&written==sizeof(pid),"owner pid");
    install::require(WriteFile(write,nonce.data(),static_cast<DWORD>(nonce.size()),&written,nullptr)&&written==nonce.size(),"owner nonce transfer");writer.reset();
    auto connection=win::connect_local_control_server(win::installation_object_names(root).pipe_name,process.dwProcessId,15000);
    install::require(connection.valid(),"installed authenticated channel");
    std::uint32_t sequence=0;
    const auto exchange=[&](app::LocalControlMessageType type,app::LocalControlMessageType expected,std::vector<std::uint8_t> payload={}) {
        app::LocalControlFrame request,response;request.type=type;request.request_id=++sequence;request.payload=std::move(payload);
        install::require(connection.write_frame(request)&&connection.read_frame(response)&&response.type==expected&&response.request_id==sequence,"installed IPC exchange");
        return response.payload;
    };
    auto hello=app::decode_control_response(exchange(app::LocalControlMessageType::hello,
        app::LocalControlMessageType::hello_response,app::encode_hello_payload(nonce)));
    SecureZeroMemory(nonce.data(),nonce.size());
    if(section8b_test::backup_redesign && hello && hello->code!=app::LocalControlResponseCode::ok)
        std::cerr<<"BACKUP FIXTURE START diagnostic="<<hello->diagnostic_code<<std::endl;
    install::require(hello && hello->code==app::LocalControlResponseCode::ok,"installed ready");
    std::cout<<"READY"<<std::endl;
    for(std::string line;std::getline(std::cin,line) && line!="stop";) {
        if(section8b_test::backup_destination && line=="backup-probe-data") {
            const auto path=root/L"Files"/L"destination-qualification.bin";
            install::require(!std::filesystem::exists(path),"new fixture probe must be absent");
            std::vector<std::uint8_t> data(32U*1024U*1024U);
            install::require(win::fill_secure_random(data.data(),data.size()),"inert qualification probe data");
            auto descriptor=install::mutable_descriptor(install::operator_sid());
            install::create_file(path,data,descriptor.get());
            std::cout<<"{\"ok\":1,\"bytes\":"<<data.size()<<'}'<<std::endl;continue;
        }
        if(section8b_test::backup_destination &&
            (line.rfind("backup-destination ",0)==0 || line.rfind("backup-stale ",0)==0 || line.rfind("backup-save-failure ",0)==0 || line=="backup-schedule-next" || line=="backup-schedule-off")) {
            const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& query)->std::optional<std::vector<std::uint8_t>> {
                return exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(query));
            });
            install::require(status && status->scheduler.configuration_loaded,"shipping live scheduler configuration");
            app::AlphaSchedulerSettingsRequest update;
            update.expected_revision=status->scheduler.configuration_revision;
            const bool stale=line.rfind("backup-stale ",0)==0;
            const bool fail_save=line.rfind("backup-save-failure ",0)==0;
            if(stale)--update.expected_revision;
            update.enabled=status->scheduler.enabled;update.local_hour=status->scheduler.local_hour;update.local_minute=status->scheduler.local_minute;
            if(line=="backup-schedule-next") {
                auto rules=win::WindowsRegistryBbsTimeZoneRuleProvider::create("Pacific Standard Time");
                install::require(rules!=nullptr,"fixture BBS timezone rules");
                class Clock final:public app::IClock{public:std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}std::uint64_t monotonic_millis()const noexcept override{return GetTickCount64();}} clock;
                app::BbsLocalTimeAdapters local(rules->configuration(),*rules,clock);
                const auto wall=local.format_utc_seconds(clock.unix_seconds()/60*60+120);
                install::require(wall&&wall->size()>=19,"bounded future BBS civil time");
                update.enabled=true;update.local_hour=static_cast<std::uint8_t>(std::stoi(wall->substr(11,2)));update.local_minute=static_cast<std::uint8_t>(std::stoi(wall->substr(14,2)));
            } else if(line=="backup-schedule-off") update.enabled=false;
            else update.backup_destination=line.substr(stale?13:fail_save?20:19);
            install::Handle state_pin;
            if(fail_save) state_pin=install::open_object(root/L"Data"/L"SFXScheduler.state",false,GENERIC_READ,FILE_SHARE_READ);
            const auto payload=app::encode_scheduler_settings_request(update);
            install::require(!payload.empty(),"bounded live destination request encodes");
            const auto result=app::decode_control_response(exchange(app::LocalControlMessageType::scheduler_settings_request,
                app::LocalControlMessageType::scheduler_settings_response,payload));
            install::require(result.has_value(),"shipping destination IPC response");
            std::cout<<"{\"ok\":"<<(result->code==app::LocalControlResponseCode::ok)<<",\"code\":"<<static_cast<unsigned>(result->code)
                <<",\"active_at_request\":"<<status->scheduler.active<<",\"queued_at_request\":"<<status->scheduler.queued<<'}'<<std::endl;continue;
        }
        if(section8b_test::backup_redesign && line=="backup") {
            const auto result=app::decode_control_response(exchange(app::LocalControlMessageType::scheduler_run_request,
                app::LocalControlMessageType::scheduler_run_response,app::encode_scheduler_run_request({})));
            install::require(result.has_value(),"shipping backup IPC result");
            std::cout<<"{\"ok\":"<<(result->code==app::LocalControlResponseCode::ok)<<'}'<<std::endl;continue;
        }
        if(section8b_test::backup_redesign && line=="backup-status") {
            const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& query)->std::optional<std::vector<std::uint8_t>> {
                return exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(query));
            });
            install::require(status.has_value(),"shipping scheduler status");
            std::cout<<"{\"loaded\":"<<status->scheduler.configuration_loaded<<",\"enabled\":"<<status->scheduler.enabled
                <<",\"active\":"<<status->scheduler.active<<",\"scheduled\":"<<status->scheduler.active_scheduled
                <<",\"last_run\":"<<status->scheduler.last_run_utc<<",\"last_result\":"<<static_cast<unsigned>(status->scheduler.last_result)
                <<",\"next_run\":"<<status->scheduler.next_run_utc<<'}'<<std::endl;continue;
        }
        if(section8a && section8a_command(line,[&](const app::LocalMessageConfigurationQuery& query){
            return app::decode_message_configuration_snapshot(exchange(app::LocalControlMessageType::message_configuration_query_request,
                app::LocalControlMessageType::message_configuration_query_response,app::encode_message_configuration_query(query))).value_or(app::LocalMessageConfigurationSnapshot{});
        },[&](const app::MessageConfigurationChange& change){
            return app::decode_message_configuration_result(exchange(app::LocalControlMessageType::message_configuration_change_request,
                app::LocalControlMessageType::message_configuration_change_response,app::encode_message_configuration_change(change))).value_or(app::LocalMessageConfigurationResult{});
        }))continue;
        if(section7 && section7_command(line,[&](std::int64_t after) {
            return app::decode_classic_information_snapshot(exchange(app::LocalControlMessageType::classic_information_query_request,
                app::LocalControlMessageType::classic_information_query_response,app::encode_classic_information_query(after))).value_or(app::LocalClassicInformationSnapshot{});
        },[&](const app::ClassicInformationChange& change) {
            return app::decode_account_result(exchange(app::LocalControlMessageType::classic_information_change_request,
                app::LocalControlMessageType::classic_information_change_response,app::encode_classic_information_change(change))).value_or(app::LocalAccountResult{});
        }))continue;
        if(section6 && line=="crash") {
            install::require(TerminateProcess(child.get(),87) && WaitForSingleObject(child.get(),15000)==WAIT_OBJECT_0,"terminate only owned disposable Runtime child");
            connection.disconnect();std::cout<<"CRASHED fixture"<<std::endl;return;
        }
        if(section6 && (line.rfind("limits-",0)==0 || line=="policy-threshold-ten" || line=="policy-threshold-999")) {
            const auto catalog=app::decode_security_snapshot(exchange(app::LocalControlMessageType::security_query_request,
                app::LocalControlMessageType::security_query_response,app::encode_account_query({})));
            install::require(catalog && catalog->code==app::LocalAccountResultCode::ok && !catalog->has_more,"bounded installed catalog");
            app::LocalSecurityChange change;change.expected_revision=catalog->policy.revision;change.new_user_level=catalog->policy.new_user_level;
            change.sysop_threshold=catalog->policy.sysop_threshold;
            if(line.rfind("limits-",0)==0) {
                const auto level=std::find_if(catalog->levels.begin(),catalog->levels.end(),[](const auto& item){return item.level==10;});
                install::require(level!=catalog->levels.end(),"installed ordinary level exists");
                change.action=app::LocalSecurityAction::upsert_level;change.level=10;change.label=level->label;change.description=level->description;
                if(line=="limits-call-one") change.minutes_per_call=1;
                else if(line=="limits-day-one") change.minutes_per_day=1;
                else if(line=="limits-sixty-180") {change.minutes_per_call=60;change.minutes_per_day=180;}
                else install::require(line=="limits-unlimited","bounded installed limits command");
            } else {change.action=app::LocalSecurityAction::set_defaults;change.sysop_threshold=line=="policy-threshold-ten"?10:999;}
            const auto result=app::decode_account_result(exchange(app::LocalControlMessageType::security_change_request,
                app::LocalControlMessageType::security_change_response,app::encode_security_change(change)));
            install::require(result.has_value(),"installed policy result");
            std::cout<<"{\"ok\":"<<(result->code==app::LocalAccountResultCode::ok)<<",\"committed\":"<<result->committed<<'}'<<std::endl;continue;
        }
        if(section6 && line.rfind("editor ",0)==0) {
            std::istringstream input(line.substr(7));std::string action;app::AccountId id{};unsigned level{};std::int64_t expected{};
            input>>action>>id>>level>>expected;
            const auto snapshot=app::decode_account_snapshot(exchange(app::LocalControlMessageType::account_query_request,
                app::LocalControlMessageType::account_query_response,app::encode_account_query({id,"",0})));
            install::require(snapshot && snapshot->code==app::LocalAccountResultCode::ok && snapshot->accounts.size()==1,"installed selected account identity");
            const auto& account=snapshot->accounts.front();app::LocalAccountRequest change;
            change.account_id=id;change.expected_revision=expected?expected:account.revision;change.expected_policy_revision=snapshot->policy.revision;
            change.name=account.display_name.value_or("");change.profile=account.profile;change.security_level=level?static_cast<std::uint16_t>(level):account.security_level.value_or(0);change.purge_allowed=account.purge_allowed.value_or(false);
            if(action=="save") {change.action=app::LocalAccountAction::save_user;change.profile.location="Live editor saved";}
            else if(action=="deactivate") change.action=app::LocalAccountAction::lock;
            else if(action=="reactivate") change.action=app::LocalAccountAction::unlock;
            else if(action=="delete") change.action=app::LocalAccountAction::tombstone;
            else install::require(false,"bounded installed editor command");
            const auto result=app::decode_account_result(exchange(app::LocalControlMessageType::account_command_request,
                app::LocalControlMessageType::account_command_response,app::encode_account_request(change)));
            install::require(result.has_value(),"installed editor result");
            std::cout<<"{\"ok\":"<<(result->code==app::LocalAccountResultCode::ok)<<",\"last_sysop_protected\":"<<(result->code==app::LocalAccountResultCode::last_sysop_protected)<<",\"committed\":"<<result->committed<<",\"revision\":"<<result->revision<<'}'<<std::endl;continue;
        }
        install::require(line=="status","bounded coordinator command");
        const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& query)->std::optional<std::vector<std::uint8_t>> {
            return exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(query));
        });
        install::require(status.has_value(),"installed status snapshot");
        std::cout<<"{\"active\":"<<status->lifecycle.active_work.caller_sessions<<"}"<<std::endl;
    }
    const auto stopped=app::decode_control_response(exchange(app::LocalControlMessageType::shutdown_request,app::LocalControlMessageType::shutdown_response));
    install::require(stopped && stopped->code==app::LocalControlResponseCode::ok,"orderly installed shutdown");
    connection.disconnect();install::require(WaitForSingleObject(child.get(),15000)==WAIT_OBJECT_0,"installed Runtime exited");
    DWORD code{};install::require(GetExitCodeProcess(child.get(),&code)&&code==0,"installed Runtime exit success");
    std::cout<<"STOPPED clean"<<std::endl;
}
