// Reuse the isolated storage fixtures; their historical runners are not called.
#include "sfx/application/door_execution.hpp"
#define main retained_section15_file_fixture
#include "file_storage_tests.cpp"
#undef main
#include "door_first_run_defaults.inc"

int main(int argc,char** argv) { try {
    check(argc==2,"new isolated fixture path required");
    const auto root=std::filesystem::absolute(argv[1]);
    check(!std::filesystem::exists(root),"retain all previous fixture evidence");
    const auto windows=app::fresh_install_starter_doors(app::StarterDoorPlatform::windows);
    const auto native_linux=app::fresh_install_starter_doors(app::StarterDoorPlatform::linux_native);
    check(windows.size()==3&&native_linux.size()==3,"both platforms retain three approved starter definitions");
    check(windows[0].program.startup_directory=="Doors/SFXGuess32"&&
        windows[2].program.startup_directory=="Doors/NewsWire-X-RSS","Windows starter directories retain exact spelling");
    check(native_linux[0].program.startup_directory=="doors/sfxguess32"&&
        native_linux[2].program.startup_directory=="doors/newswire-x-rss","Linux starter directories use fresh lowercase layout");
    check(windows[0].program.command_line=="SFXGuess32.exe --drop \"%D\\DOORDROP.INI\""&&
        windows[2].program.command_line=="NewsWire-X-RSS.exe --drop \"%D\\DOORDROP.INI\"","Windows seed bytes preserved on either build platform");
    check(native_linux[0].program.command_line=="sfxguess32 --drop \"%D/DOORDROP.INI\""&&
        native_linux[2].program.command_line=="newswire-x-rss --drop \"%D/DOORDROP.INI\"","Linux seeds explicitly select actual packaged executables");
    check(app::encode_door_definition(windows[1])==app::encode_door_definition(native_linux[1]),"remote gateway definition unchanged across platforms");
    check(app::fresh_install_starter_doors(static_cast<app::StarterDoorPlatform>(99)).empty(),"unknown target cannot generate guessed starter commands");
    sfx::test::FakeStoragePathPolicy paths; const app::MessageConfigurationActor owner{true,{}};
    for(const std::string variant:{"fresh","older-incomplete","customized","rollback"}) {
        const auto options=settings(root/variant);storage::SqliteStorage db(options,paths);
        check(db.start().succeeded(),"start current-schema isolated installation");
        std::vector<std::uint8_t> customized;
        if(variant=="customized") {
            app::DoorConfigurationChange c;c.operation=id(8300);c.expected_revision=1;
            c.action=app::DoorConfigurationAction::save_menu;c.menu={0,"My Door Menu"};
            check(db.change_door_configuration(owner,c).succeeded(),"pre-completion customization saved");
            customized=app::encode_door_catalog(*db.door_configuration(owner).value);
        }
        auto complete=stage_door_defaults(db);
        complete.starter_doors_eligible=variant!="older-incomplete";
        const auto file=options.data_directory/"SFX.DB";
        if(variant=="rollback") {
            internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
            c.exec("CREATE TRIGGER starter_abort BEFORE INSERT ON sfx_door_definitions WHEN NEW.door_id=2 "
                "BEGIN SELECT RAISE(ABORT,'owned starter fault'); END;","interrupt after first starter");
            check(!db.complete_first_run(complete),"mid-starter failure is reported");
            const auto failed=db.door_configuration(owner);
            check(failed&&failed.value->doors.empty()&&failed.value->entries.empty()&&failed.value->menus.size()==1,
                "all categories, placements and definitions roll back together");
            check(!db.first_run_state().value->complete,"failed transaction leaves first run incomplete");
            c.exec("DROP TRIGGER starter_abort","correct owned failure before same-operation retry");
        }
        check(db.complete_first_run(complete).succeeded(),"first run commits including corrected retry");
        const auto catalog=db.door_configuration(owner);check(catalog&&catalog.value,"read committed catalog");
        if(variant=="older-incomplete")check_starter_door_categories(*catalog.value);
        else if(variant=="customized")
            check(app::encode_door_catalog(*catalog.value)==customized,"eligible customization remains byte exact");
        else {
            check(app::validate_door_catalog(*catalog.value).valid(),"starter catalog validates");
            check(catalog.value->revision==2&&catalog.value->doors.size()==3&&catalog.value->entries.size()==6,
                "three targets and three placements use one first-run revision");
            const auto& game=*app::find_door(*catalog.value,1);const auto& gate=*app::find_door(*catalog.value,2);
            const auto& news=*app::find_door(*catalog.value,3);
            check(game.name=="SFX Guess a Number"&&news.name=="NewsWire-X RSS","owner display names retained");
#ifdef __linux__
            check(game.program.startup_directory=="doors/sfxguess32"&&news.program.startup_directory=="doors/newswire-x-rss",
                "Linux first-run publishes actual lowercase packaged directories");
            check(game.program.command_line=="sfxguess32 --drop \"%D/DOORDROP.INI\""&&
                news.program.command_line=="newswire-x-rss --drop \"%D/DOORDROP.INI\"","Linux starter commands match approved native payload names");
#else
            check(game.program.startup_directory=="Doors/SFXGuess32"&&news.program.startup_directory=="Doors/NewsWire-X-RSS",
                "Windows first-run retains actual existing directory spelling");
            check(game.program.command_line=="SFXGuess32.exe --drop \"%D\\DOORDROP.INI\""&&
                news.program.command_line=="NewsWire-X-RSS.exe --drop \"%D\\DOORDROP.INI\"","Windows starter commands remain exact");
#endif
            for(const auto* d:{&game,&news}) {
                check(d->enabled&&d->kind==app::DoorExecutionKind::local&&d->security_level==10&&d->execution.level==10&&
                    !d->sysop_only&&d->allow_multiple&&!d->event_only&&d->automatic_event==app::DoorAutomaticEvent::none,
                    "ordinary eligible callers use concurrent explicit local entries");
                check(d->program.type==app::DoorProgramType::native&&d->program.io==app::DoorIo::socket&&
                    d->program.drop_file==app::DoorDropFile::doordrop_ini&&d->program.drop_location==app::DoorDropLocation::node&&
                    !d->program.command_shell&&!d->program.doordrop_utf8&&d->program.cleanup_command.empty(),
                    "native socket node drop defaults without shell or cleanup");
#ifdef __linux__
                const auto expanded=app::expand_door_command(d->program.command_line,
                    {"/home/test/SFX Test/Node/2","/home/test/SFX Test/Node/2","/home/test/SFX Test/"+d->program.startup_directory,d->code,2});
                check(expanded&&expanded->find("\"/home/test/SFX Test/Node/2/DOORDROP.INI\"")!=std::string::npos,
                    "Linux drop directory with spaces expands to quoted actual filename");
#else
                const auto expanded=app::expand_door_command(d->program.command_line,
                    {"C:\\SFX Test\\Node\\2","C:\\SFX Test\\Node\\2","C:\\SFX Test\\"+d->program.startup_directory,d->code,2});
                check(expanded&&expanded->find("\"C:\\SFX Test\\Node\\2\\DOORDROP.INI\"")!=std::string::npos,
                    "quoted drop DIRECTORY expansion appends exact filename for spaces");
#endif
            }
            check(gate.name=="X-Gate Game Server"&&gate.rlogin.host=="x-bit.org"&&gate.rlogin.port==5130&&
                gate.rlogin.terminal_type=="xtrn_sec=xtrn_sec"&&gate.rlogin.quiet&&!gate.rlogin.pause&&
                gate.rlogin.prompt_credential&&gate.rlogin.protected_credential.empty()&&gate.rlogin.protected_pepper.empty(),
                "public native gateway settings contain no stored credential");
        }
        const auto exact=app::encode_door_catalog(*catalog.value);
        check(!db.complete_first_run(complete),"completed first-run replay refuses duplication");
        check(db.shutdown().succeeded()&&db.start().succeeded(),"normal restart preserves published state");
        check(app::encode_door_catalog(*db.door_configuration(owner).value)==exact,"restart preserves byte-exact catalog");
        check(db.shutdown().succeeded(),"owned fixture closes normally");
    }
    std::cout<<"PASS "<<checks<<" Section15 starter checks\n";return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<"\n";return 1;} }
