#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/shipping_schema.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_sfx_baseline.hpp"
#include "sqlite_statement.hpp"
#include "sfx/application/message_compose.hpp"
#include "sfx/application/message_selection.hpp"
#include "sfx/application/message_reader.hpp"
#include "sfx/application/message_session.hpp"
#include "sfx/application/local_message_administration.hpp"
#include "sfx/test/terminal_stream_fake.hpp"
#include "sfx/test/sqlite_write_fault_vfs.hpp"
#include <iostream>
#include <fstream>
#include "sfx/application/message_presentation.hpp"
#include <future>
#include <atomic>
#include <set>

namespace app=sfx::application;
namespace db=sfx::adapters::storage;
namespace internal=db::internal;
unsigned checks{};
void check(bool yes,const char* why){++checks;if(!yes)throw std::runtime_error(why);}
app::Identifier128 id(unsigned n){app::Identifier128 v;v.bytes[0]=8;v.bytes[14]=static_cast<std::uint8_t>(n>>8);v.bytes[15]=static_cast<std::uint8_t>(n);return v;}
db::SqliteStorageSettings options(const std::filesystem::path& root){std::filesystem::create_directories(root);db::SqliteStorageSettings s;s.data_directory=root;s.utc_now=[]{return std::int64_t{1791278000};};return s;}
void edit(const std::filesystem::path& file,const char* sql){internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec(sql,"isolated test mutation");}
#include "beta6_frozen_beta5_ledger_checks.inc"
std::int64_t scalar(const std::filesystem::path& file,const char* sql){internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);if(std::string_view(sql)=="SELECT count(*) FROM sfx_schema_migrations")check_frozen_beta5_ledger(c);return c.scalar_int64(sql,"test invariant");}
void seed(const std::filesystem::path& file){
    internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
    c.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(1,'Sysop','sysop',999,'active',1,1,1700000000,1700000000),(2,'Alice','alice',10,'active',1,1,1700000000,1700000000),(3,'Bob','bob',10,'active',1,1,1700000000,1700000000)","test actors");
    c.exec("INSERT INTO sfx_account_statistics(account_id) SELECT account_id FROM sfx_accounts; INSERT INTO sfx_account_profiles(account_id) SELECT account_id FROM sfx_accounts; INSERT INTO sfx_account_name_reservations SELECT name_key,account_id,1,display_name,1700000000,NULL FROM sfx_accounts", "complete synthetic account fixture");
    c.exec("UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash", "test published policy");
    c.exec("UPDATE sfx_installation_state SET staged_sysop_account_id=1,setup_state='complete',completed_utc_seconds=1700000000", "trusted first-run fixture");
    for(unsigned n=1;n<=3;++n){internal::SqliteStatement q(c.get(),"INSERT INTO sfx_account_sessions VALUES(?1,?2,?2,1700000000,0,1700000000,NULL,NULL,?1)","test session");q.bind(1,id(n));q.bind(2,static_cast<std::int64_t>(n));q.step();}
}
app::CommitMessageRequest request(db::SqliteStorage& s,app::MessageActor a,app::MessageTarget t,unsigned n){auto c=s.begin_message(a,t);check(static_cast<bool>(c),"authorized begin");return {a,id(n),*c.value,"Subject","Body\n"};}
void migration_faults(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    for(const char* point:{"before-local-message-migration","before-local-message-migration-commit","after-local-message-migration-commit"}){
        auto s=options(root/point);auto plan=db::sfx_storage_migrations();plan.resize(7);
        {db::SqliteStorage v7(s,policy,plan);check(static_cast<bool>(v7.start()),"exact v7 prior");check(static_cast<bool>(v7.shutdown()),"prior stopped");}
        seed(s.data_directory/"SFX.DB");
        const auto legacy_backup=s.data_directory/"legacy-v7.db";
        {db::SqliteStorage v7(s,policy,plan);check(v7.start().succeeded(),"completed v7 retained accounts start");check(v7.create_backup(legacy_backup).status.succeeded(),"whole completed v7 recovery backup before migration");check(v7.shutdown().succeeded(),"completed v7 backup stopped");}
        s.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"injected migration fault"}:app::StorageStatus::success();};
        {db::SqliteStorage failed(s,policy);check(!failed.start(),"migration failure truthful");}
        {internal::SqliteConnection c(s.data_directory/"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(c.get(),true),"recognized exact lineage after failure");check(c.scalar_int64("PRAGMA user_version","atomic migration")== (std::string_view(point)=="after-local-message-migration-commit"?8:7),"migration atomic");}
        s.bootstrap_boundary_hook={};db::SqliteStorage retry(s,policy);const auto retry_start=retry.start();if(!retry_start)std::cerr<<"retry startup: "<<retry_start.operation<<" code "<<static_cast<unsigned>(retry_start.code)<<'\n';check(static_cast<bool>(retry_start) && retry.health().schema_version==31,"retry idempotent to current shipping lineage");
        check(scalar(s.data_directory/"SFX.DB","SELECT account_id FROM sfx_system_sysop")==1 && scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_accounts")==3,"eligible retained bootstrap designated with all legacy accounts");
        check(static_cast<bool>(retry.shutdown()),"retry stopped");
        check(retry.validate_backup(legacy_backup).succeeded() && retry.restore_backup(legacy_backup).succeeded(),"verified completed v7 recovery restore");
        check(retry.start().succeeded() && retry.health().schema_version==31,"restored legacy database migrates once on start");
        check(scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_account_sessions")==3 && scalar(s.data_directory/"SFX.DB","SELECT account_id FROM sfx_system_sysop")==1,"legacy sessions and trusted assignment survive restore/re-migration");
        check(retry.shutdown().succeeded(),"recovered legacy fixture stopped");
    }
}
void administration_migration_faults(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    for(const char* point:{"before-message-admin-migration","before-message-admin-migration-commit","after-message-admin-migration-commit"}){
        auto s=options(root/point);auto plan=db::sfx_storage_migrations();plan.resize(8);
        {db::SqliteStorage prior(s,policy,plan);check(prior.start().succeeded(),"exact v8 before admin upgrade");check(prior.shutdown().succeeded(),"v8 stopped");}
        seed(s.data_directory/"SFX.DB");
        const auto backup=s.data_directory/"prior-v8.db";
        {db::SqliteStorage prior(s,policy,plan);check(prior.start().succeeded(),"populated v8 starts");check(prior.create_backup(backup).status.succeeded(),"whole v8 recovery retained");check(prior.shutdown().succeeded(),"v8 backup stopped");}
        s.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"injected admin migration fault"}:app::StorageStatus::success();};
        {db::SqliteStorage failed(s,policy);check(!failed.start(),"admin upgrade failure truthful");}
        {internal::SqliteConnection c(s.data_directory/"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(c.get(),true),"exact recognized schema after admin failure");check(c.scalar_int64("PRAGMA user_version","atomic admin migration")== (std::string_view(point)=="after-message-admin-migration-commit"?9:8),"admin ledger/schema publication atomic");}
        s.bootstrap_boundary_hook={};db::SqliteStorage retry(s,policy);check(retry.start().succeeded() && retry.health().schema_version==31,"admin upgrade retries exactly once");check(retry.shutdown().succeeded(),"admin upgraded fixture stopped");
        check(retry.validate_backup(backup).succeeded() && retry.restore_backup(backup).succeeded(),"actual v8 recovery through production adapter");
        check(retry.start().succeeded() && retry.health().schema_version==31,"restored v8 upgrades once");
        check(scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_accounts")==3 && scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_schema_migrations")==31,"accounts and exact thirty-one migration ledger preserved");
        check(retry.shutdown().succeeded(),"v8 recovery fixture stopped");
    }
}
void query_checks(db::SqliteStorage& store,const std::filesystem::path& file,app::MessageActor alice,app::MessageActor bob){
    app::MessageCatalogCursor catalog;catalog.page_size=1;
    auto groups=store.message_group_page(bob,catalog);check(groups && groups.value->entries.size()==1 && !groups.value->next,"bounded group catalog");
    auto echoes=store.local_echo_page(bob,{1},catalog);check(echoes && echoes.value->entries.front().id.value==1 && echoes.value->next,"echo catalog continuation");
    auto echo2=store.local_echo_page(bob,{1},*echoes.value->next);check(echo2 && echo2.value->entries.front().id.value==2 && !echo2.value->next,"echo page stable identity");
    app::MessageQueryOptions options;options.echo={2};
    auto context=store.begin_message_query(bob,options);check(static_cast<bool>(context),"capture ordinary query fence");
    auto first=store.query_message_headers(bob,*context.value,{},1);check(first && first.value->entries.size()==1 && first.value->entries.front().identity.area_number==1 && first.value->next,"ordinary ascending first page");
    auto second=store.query_message_headers(bob,*context.value,*first.value->next,1);check(second && second.value->entries.front().identity.area_number==2 && !second.value->next,"ordinary continuation no duplicate");
    auto alien=*context.value;alien.owner=alice.account;check(!store.query_message_headers(bob,alien,{},1),"query context actor binding");
    options.search="%_ OR 1=1";auto literal=store.begin_message_query(bob,options);auto none=store.query_message_headers(bob,*literal.value,{},1);check(none && none.value->entries.empty() && none.value->next,"literal search bounded zero-match continuation");
    options.search="bOdY";literal=store.begin_message_query(bob,options);check(store.query_message_headers(bob,*literal.value,{},1).value->entries.size()==1,"literal case insensitive body search");
    options.search=std::string(129,'x');check(!store.begin_message_query(bob,options),"search length bound");
    options.search="\x1b";check(!store.begin_message_query(bob,options),"search control rejected");
    app::MessageTarget addressed{app::MessageKind::public_echo,{2},3,{}};
    auto older=request(store,alice,addressed,40);older.subject="Older unread";auto unread=store.commit_message(older);check(static_cast<bool>(unread),"addressed scan fixture commit");
    auto newer=request(store,alice,{app::MessageKind::public_echo,{2},{},{}},41);auto high=store.commit_message(newer);store.open_message(bob,high.value->id);
    options={};options.mode=app::MessageQueryMode::regular_new;options.echo={2};
    auto regular=store.begin_message_query(bob,options);auto page=store.query_message_headers(bob,*regular.value,{},50);check(page && page.value->entries.size()==1 && page.value->entries.front().identity.id==unread.value->id,"To-you below high-water retained despite Unread Off");
    auto preferences=store.message_scan_preference(bob,{2});auto both_off=store.set_message_scan_preference(bob,{2},preferences.value->revision,app::MessageScanColumn::to_you,false);check(static_cast<bool>(both_off),"disable second scan column");
    check(store.query_message_headers(bob,*regular.value,{},50).value->entries.empty(),"both Off excludes scan not manual reading");
    store.set_message_scan_preference(bob,{2},both_off.value->revision,app::MessageScanColumn::to_you,true);
    options.mode=app::MessageQueryMode::personal;options.scope=app::MessageQueryScope::all_accessible;options.echo={};
    auto personal=store.begin_message_query(bob,options);auto unread_page=store.query_message_headers(bob,*personal.value,{},1);check(unread_page && unread_page.value->entries.size()==1 && unread_page.value->entries.front().identity.id==unread.value->id && unread_page.value->next && unread_page.value->next->history,"personal unread first then history cursor");
    check(!store.quote_source(bob,unread.value->id).value->header.recipient_opened,"query alone does not mark recipient");
    store.open_message(bob,unread.value->id);
    auto later=request(store,alice,addressed,42);auto arrival=store.commit_message(later);check(static_cast<bool>(arrival),"arrival beyond personal fence");
    auto history=store.query_message_headers(bob,*personal.value,*unread_page.value->next,50);check(history && history.value->entries.size()==1 && history.value->entries.front().identity.area_number==2 && !history.value->next,"history excludes just-opened item and later arrival");
    auto next_personal=store.begin_message_query(bob,options);auto next_unread=store.query_message_headers(bob,*next_personal.value,{},50);check(next_unread && next_unread.value->entries.size()==1 && next_unread.value->entries.front().identity.id==arrival.value->id,"next scan includes later arrival");
    options.mode=app::MessageQueryMode::inbox;auto inbox=store.begin_message_query(bob,options);check(store.query_message_headers(bob,*inbox.value,{},50).value->entries.empty(),"other mailbox invisible in list");
    options.mode=app::MessageQueryMode::sent;auto sent=store.begin_message_query(alice,options);check(store.query_message_headers(alice,*sent.value,{},50).value->entries.size()==1,"sender private copy retained in sent list");
    edit(file,"UPDATE sfx_message_configuration_state SET revision=revision+1 WHERE singleton_id=1");
    check(!store.local_echo_page(bob,{1},*echoes.value->next) && !store.query_message_headers(bob,*personal.value,{},50),"configuration revision prevents stale numeric remap");
}
void configuration_checks(db::SqliteStorage& store,const std::filesystem::path& file,app::MessageActor alice,app::MessageActor bob){
    app::MessageConfigurationActor owner{true,{}},ordinary{false,alice};
    check(!store.message_configuration(ordinary,{},{}),"ordinary caller cannot administer messages");
    auto initial=store.message_configuration(owner,{},{});check(initial && initial.value->system_sysop==1 && initial.value->groups.size()==1,"trusted configuration snapshot");
    app::MessageConfigurationChange c;c.operation=id(60);c.expected_configuration_revision=initial.value->revision;c.group.name="Second";c.group.order=1;c.group.access=999;
    auto created=store.change_message_configuration(owner,c);check(created && created.value->entity_id==2,"create stable group");
    auto replay=store.change_message_configuration(owner,c);check(replay && replay.value->replayed && replay.value->entity_id==2,"group creation replay retains ID");
    c.group.name="Mismatch";check(!store.change_message_configuration(owner,c),"configuration operation mismatch refused");
    c={};c.operation=id(61);c.action=app::MessageConfigurationAction::echo;c.expected_configuration_revision=created.value->configuration_revision;c.echo.group={1};c.echo.name="Third echo";c.echo.order=2;
    auto echo=store.change_message_configuration(owner,c);check(echo && echo.value->entity_id==3,"new echo allocates stable ID and unique number");
    app::MessageTarget target{app::MessageKind::public_echo,{3},{},{}};auto post=store.commit_message(request(store,alice,target,62));check(post && post.value->area_number==1,"new echo independent sequence");
    auto catalog=store.message_configuration(owner,app::MessageGroupId{1},{});auto third=catalog.value->echoes.back();
    c={};c.operation=id(63);c.action=app::MessageConfigurationAction::echo;c.expected_configuration_revision=echo.value->configuration_revision;c.echo=third;c.echo.group={2};c.echo.name="Moved";
    auto moved=store.change_message_configuration(owner,c);check(moved && !store.open_message(bob,post.value->id),"group move applies destination gate immediately");
    check(scalar(file,"SELECT next_message_number FROM sfx_local_echoes WHERE echo_id=3")==2 && scalar(file,"SELECT conference_number FROM sfx_local_echoes WHERE echo_id=3")==3,"move retains number/content sequence");
    c.operation=id(64);c.expected_configuration_revision=moved.value->configuration_revision;c.echo.revision=moved.value->entity_revision;c.echo.enabled=false;
    auto disabled=store.change_message_configuration(owner,c);check(disabled && store.message_configuration(owner,app::MessageGroupId{2},{}).value->echoes.size()==1,"disabled echo retained for administration");
    c.operation=id(65);c.expected_configuration_revision=disabled.value->configuration_revision;c.echo.revision=disabled.value->entity_revision;c.echo.enabled=true;auto enabled=store.change_message_configuration(owner,c);check(static_cast<bool>(enabled),"re-enable same echo identity");
    app::MessageConfigurationChange clone;clone.action=app::MessageConfigurationAction::clone_permissions;clone.operation=id(66);clone.expected_configuration_revision=enabled.value->configuration_revision;clone.clone_source={1};clone.source_revision=1;clone.clone_targets={{{3},enabled.value->entity_revision},{{2},999}};clone.permission_mask=4;
    check(!store.change_message_configuration(owner,clone) && scalar(file,"SELECT posting_minimum FROM sfx_local_echoes WHERE echo_id=3")==1 && scalar(file,"SELECT revision FROM sfx_message_configuration_state")==enabled.value->configuration_revision,"stale clone target rolls back all earlier changes");
    clone.operation=id(67);clone.clone_targets.pop_back();auto copied=store.change_message_configuration(owner,clone);check(copied && scalar(file,"SELECT posting_minimum FROM sfx_local_echoes WHERE echo_id=3")==999 && scalar(file,"SELECT reading_minimum FROM sfx_local_echoes WHERE echo_id=3")==1,"clone selected posting only");
    clone.operation=id(68);clone.expected_configuration_revision=copied.value->configuration_revision;clone.permission_mask=0;auto noop=store.change_message_configuration(owner,clone);check(noop && noop.value->configuration_revision==copied.value->configuration_revision,"unchecked clone no-op");
    auto draft=request(store,alice,{app::MessageKind::public_echo,{2},{},{}},69);
    c={};c.action=app::MessageConfigurationAction::line_limit;c.operation=id(70);c.expected_configuration_revision=copied.value->configuration_revision;c.security_level=10;c.maximum_lines=1;c.expected_limit_revision=1;
    auto limited=store.change_message_configuration(owner,c);check(limited && !store.commit_message(draft),"changed limit refuses stale draft");c.operation=id(71);c.expected_configuration_revision=limited.value->configuration_revision;c.maximum_lines=200;c.expected_limit_revision=limited.value->entity_revision;auto restored=store.change_message_configuration(owner,c);check(static_cast<bool>(restored),"restore finite line allowance");
    auto sysop=store.system_sysop_target(alice);check(sysop && sysop.value->recipient==1 && sysop.value->sysop_assignment_revision,"System Sysop stable target");auto old_mail=request(store,alice,*sysop.value,72);
    c={};c.action=app::MessageConfigurationAction::system_sysop;c.operation=id(73);c.expected_configuration_revision=restored.value->configuration_revision;c.sysop=2;c.expected_account_revision=1;c.expected_assignment_revision=*sysop.value->sysop_assignment_revision;
    check(!store.change_message_configuration(owner,c),"ineligible System Sysop refused");c.operation=id(74);c.sysop=1;auto assigned=store.change_message_configuration(owner,c);check(assigned && !store.commit_message(old_mail),"assignment revision requires explicit draft refresh");
    auto recipient=store.resolve_message_recipient(alice,"bO");check(recipient && recipient.value->entries.size()==1 && recipient.value->entries.front().account==3,"partial current recipient case insensitive");
    check(store.resolve_message_recipient(alice,"3").value->entries.front().account==3 && store.resolve_message_recipient(alice,"%_").value->entries.empty(),"numeric exact recipient and literal name matching");
    check(!store.resolve_message_recipient(alice,"\x1b"),"recipient query control rejected");
}
void compose_checks(db::SqliteStorage& store,const std::filesystem::path& file,app::MessageActor alice,app::MessageActor bob,app::MessageId parent){
    const app::MessageTarget target{app::MessageKind::public_echo,{2},{},{}};
    const auto total=scalar(file,"SELECT total_committed FROM sfx_message_counters");
    unsigned operation=300;
    const auto run=[&](std::string_view script,bool ansi){
        sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text(script);app::StreamCallerTerminal terminal(stream);
        app::MessageComposePorts ports{store,terminal,[&]{return std::optional<app::MessageActor>{alice};},[&]{return id(operation++);},[ansi]{return app::MessageEditorDimensions{ansi,80,24};},[&]{return terminal.read_editor_byte();},[](std::string_view){return true;}};
        return app::compose_local_message(ports,target,"All Callers","Alice");
    };
    check(run("Compose\r\nbody\r\n/e1\r\n/S\r\n/s\r\nY\r\n",false)==app::MessageComposeOutcome::saved,"line compose stores numbered slash replacement literally");
    check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==total+1,"connected save counts once");
    check(run("Subject\r\nbody\r\n/a\r\nN\r\n/s\r\nN\r\n/s\r\nY\r\n",false)==app::MessageComposeOutcome::saved,"discard No/save No retain connected draft");
    check(run("Abort\r\nbody\r\n/a\r\nY\r\n",false)==app::MessageComposeOutcome::cancelled && scalar(file,"SELECT total_committed FROM sfx_message_counters")==total+2,"confirmed discard never posts");
    check(run("Full screen\r\nHi\x1aY\r\n",true)==app::MessageComposeOutcome::saved,"FSE live Ctrl-Z normal confirmation save");
    check(run("Help save\r\ntext\r\n/?\r\nS\r\nY\r\n",false)==app::MessageComposeOutcome::saved,"ESC/help menu save uses shared confirmation path");
    check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==total+4,"compose modes success-only counters");
    check(run("Safe defaults\r\nkept\x0c\r\n\x01\r\n\x1a\r\n",true)==app::MessageComposeOutcome::saved,"ESC Edit and Abort No defaults retain draft; Save Yes default commits");
    {
        sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text("Escape safe\r\n");app::StreamCallerTerminal terminal(stream);
        const std::vector<int> keys{'k',26,27,-1,'!',26,13};std::size_t next{};
        app::MessageComposePorts ports{store,terminal,[&]{return std::optional<app::MessageActor>{alice};},[&]{return id(operation++);},[]{return app::MessageEditorDimensions{true,80,24};},[&]{if(next==keys.size())return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::disconnected,{}};const auto key=keys[next++];return key<0?app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::timed_out,{}}:app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::completed,static_cast<unsigned char>(key)};},[](std::string_view){return true;}};
        check(app::compose_local_message(ports,target,"All Callers","Alice")==app::MessageComposeOutcome::saved && next==keys.size(),"Save Escape returns draft; only subsequent Enter accepts default Yes");
    }
    for(bool ansi:{false,true}){
        sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text(ansi?"\r\n\x11\r\n\x1aY\r\n":"\r\n/q\r\nAll\r\n/s\r\nY\r\n");app::StreamCallerTerminal terminal(stream);
        std::string rendered;
        app::MessageComposePorts ports{store,terminal,[&]{return std::optional<app::MessageActor>{bob};},[&]{return id(operation++);},[ansi]{return app::MessageEditorDimensions{ansi,80,24};},[&]{return terminal.read_editor_byte();},[&](std::string_view text){rendered+=text;return true;},{}};
        auto reply=target;reply.recipient=2;
        check(app::compose_local_message(ports,reply,"Alice","Bob",parent,"Subject")==app::MessageComposeOutcome::saved,"plain/ANSI selected quote reply save");
        if(ansi)check(rendered.find("Space marks; A All; N ranges; ESC cancels; Enter returns")!=rendered.npos,"actual quote-selection frame has exact footer while Enter still applies selected quotes");
    }
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE parent_message_id=1")==2,"reply parent stable and modes share quote model");
}
void editor_popup_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto settings=options(root);
    db::SqliteStorage store(settings,policy);
    check(store.start().succeeded()&&store.shutdown().succeeded(),"fresh editor popup fixture");
    seed(settings.data_directory/"SFX.DB");
    check(store.start().succeeded(),"editor popup fixture actors ready");
    const app::MessageActor alice{id(2),2,1,1},bob{id(3),3,1,1};
    const app::MessageTarget target{app::MessageKind::public_echo,{2},{},{}};
    auto posting=request(store,alice,target,904);
    posting.body="First quote row\nSecond quote row\nThird quote row\n";
    const auto posted=store.commit_message(posting);
    check(posted&&posted.value,"three quote rows prepared through normal storage");

    sfx::test::DeterministicInteractiveByteStreamFake stream;
    stream.script_non_secret_text("Popup draft\r");
    app::StreamCallerTerminal terminal(stream);
    const std::vector<int> keys{
        'k',27,-1,27,'[','C',27,'[','D','\r', // Actions: move right, back to Edit, Enter.
        26,27,'[','C','\r',                   // Save: move to No and return.
        17,27,'[','B',' ','\r',               // Quote: mark the second row and insert it.
        1,27,'[','D','\r'                    // Abort: move to Yes and discard.
    };
    std::size_t next{};
    std::vector<std::string> writes;
    app::MessageComposePorts ports{store,terminal,[&]{return std::optional<app::MessageActor>{bob};},[]{return id(905);},
        []{return app::MessageEditorDimensions{true,80,24};},[&]{
            if(next==keys.size())return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::disconnected,{}};
            const auto key=keys[next++];
            if(key<0)return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::timed_out,{}};
            return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::completed,static_cast<unsigned char>(key)};
        },[&](std::string_view frame){writes.emplace_back(frame);return true;}};
    auto reply=target;
    reply.recipient=alice.account;
    check(app::compose_local_message(ports,reply,"Alice","Bob",posted.value->id)==app::MessageComposeOutcome::cancelled&&
        next==keys.size(),"modal actions, save decline, quote and confirmed discard keep their commands");
    const auto after=[&](std::string_view text)->const std::string&{
        const auto first=std::find_if(writes.begin(),writes.end(),[&](const auto& frame){return frame.find(text)!=frame.npos;});
        check(first!=writes.end()&&std::next(first)!=writes.end(),"popup has an entry and an arrow update");
        const auto& delta=*std::next(first);
        check(delta.find("\x1b[2J")==delta.npos&&delta.find("\x1b[1;1H")==delta.npos,"arrow update keeps editor backdrop intact");
        return delta;
    };
    check(after("Editor actions - Insert").find("\x1b[0;30;47m Subject ")!=std::string::npos,"actions repaint selected control");
    check(after("Yes - Save").find("\x1b[0;30;47m No - Edit ")!=std::string::npos,"save confirmation repaints selected control");
    const auto& quote=after("Quote source: 1/3");
    check(quote.find("Quote source: 2/3")!=quote.npos&&quote.find("\x1b[16;1H")!=quote.npos&&
        quote.find("\x1b[17;1H")!=quote.npos&&quote.find("Third quote row")==quote.npos,"quote repaints the two highlight rows and heading but leaves other rows in place");
    check(after("Discard this message?").find("\x1b[0;30;47m Yes - Discard ")!=std::string::npos,"discard confirmation repaints selected control");
    check(std::any_of(writes.begin(),writes.end(),[](const auto& frame){return frame.find("> Second quote row")!=frame.npos;}),"selected quote appears in the restored draft");
    check(scalar(settings.data_directory/"SFX.DB","SELECT count(*) FROM sfx_local_messages")==1,"declined save and confirmed discard do not post a reply");
    check(store.shutdown().succeeded(),"editor popup fixture closed");
}
void selection_checks(db::SqliteStorage& store,app::MessageActor alice){
    auto first=app::first_local_message_echo(store,alice);check(first && first.value && first.value->has_value() && (**first.value).echo.id.value==1,"reconnect first accessible echo, not arbitrary mailbox");
    sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text("1\r\n2\r\nbo\r\nY\r\nQ\r\n");app::StreamCallerTerminal terminal(stream);
    app::MessageSelectionPorts ports{store,terminal,[&]{return std::optional<app::MessageActor>{alice};}};
    const auto selected=app::select_local_message_echo(ports);check(selected.outcome==app::MessageSelectionOutcome::selected && selected.selection->group.id.value==1 && selected.selection->echo.conference_number==2,"group then echo explicit stable selection");
    const auto recipient=app::select_local_message_recipient(ports);check(recipient.outcome==app::MessageSelectionOutcome::selected && recipient.recipient->account==3,"partial recipient explicit Yes binds stable ID");
    check(app::select_local_message_echo(ports).outcome==app::MessageSelectionOutcome::cancelled,"selector Q cancels without a replacement");
    for(const auto& keys:{std::string("dr. te\r\nx\r\n"),std::string("not present\r\n")}){
        sfx::test::DeterministicInteractiveByteStreamFake absent;absent.script_non_secret_text(keys);
        absent.script_non_secret_text("\x1b");absent.script_read_outcome(app::InteractiveStreamOutcome::timed_out);absent.script_non_secret_text("\r\n");
        app::StreamCallerTerminal absent_terminal(absent);app::MessageSelectionPorts absent_ports{store,absent_terminal,[&]{return std::optional<app::MessageActor>{alice};}};
        check(app::select_local_message_recipient(absent_ports).outcome==app::MessageSelectionOutcome::cancelled,"missing recipient waits through non-Enter input and Escape then returns safely on Enter");
        check(absent.output().find("No such user in the caller database\r\n")!=std::string::npos && absent.output().find("<enter>")!=std::string::npos,"missing recipient exact statement and visible Enter prompt");
    }
}
void reader_scan_checks(db::SqliteStorage& store,const std::filesystem::path& file,app::MessageActor alice,app::MessageActor bob){
    auto preview=store.preview_group_scan_column(bob,{1},app::MessageScanColumn::unread);
    check(preview && preview.value->entries.size()==2 && preview.value->enable,"mixed group scan preview -> On");
    const auto progress=store.message_scan_preference(bob,{2}).value->highest_opened;
    auto applied=store.toggle_group_scan_column(bob,id(500),*preview.value);check(applied && *applied.value,"atomic group toggle On");
    check(store.toggle_group_scan_column(bob,id(500),*preview.value).value.value(),"group toggle reconciliation cannot toggle twice");
    auto altered=*preview.value;altered.column=app::MessageScanColumn::to_you;check(!store.toggle_group_scan_column(bob,id(500),altered),"group operation identity mismatch denied");
    auto on=store.message_scan_preference(bob,{2});check(on.value->unread && on.value->to_you && on.value->highest_opened==progress,"bulk preference preserves other column and progress");
    preview=store.preview_group_scan_column(bob,{1},app::MessageScanColumn::unread);check(preview && !preview.value->enable,"all On -> Off preview");
    auto concurrent=store.set_message_scan_preference(bob,{2},on.value->revision,app::MessageScanColumn::to_you,false);check(static_cast<bool>(concurrent),"concurrent independent column edit");
    check(!store.toggle_group_scan_column(bob,id(501),*preview.value) && store.message_scan_preference(bob,{1}).value->unread,"stale later echo rejects entire group batch before writes");
    preview=store.preview_group_scan_column(bob,{1},app::MessageScanColumn::unread);applied=store.toggle_group_scan_column(bob,id(502),*preview.value);check(applied && !*applied.value && !store.message_scan_preference(bob,{1}).value->unread,"fresh all-On group toggles Off");
    auto p=store.message_scan_preference(bob,{2});store.set_message_scan_preference(bob,{2},p.value->revision,app::MessageScanColumn::to_you,true);
    preview=store.preview_group_scan_column(bob,{1},app::MessageScanColumn::unread);check(static_cast<bool>(store.toggle_group_scan_column(bob,id(503),*preview.value)),"restore readable group scan");
    app::MessageQueryOptions ordinary;ordinary.echo={2};auto context=store.begin_message_query(bob,ordinary);
    const auto first=store.message_number_header(bob,*context.value,1);const auto second=store.message_number_header(bob,*context.value,2);
    check(first && second && !store.message_number_header(bob,*context.value,999999),"scoped number/gap resolution without read state");
    auto previous=second.value->position;previous.reverse=true;
    const auto reverse=store.query_message_headers(bob,*context.value,previous,1);check(reverse && reverse.value->entries.front().identity.id==first.value->identity.id,"stateless Previous ordinary keyset");
    check(!store.open_query_message(alice,*context.value,first.value->identity.id,false,false),"scoped open owner identity cannot be substituted");
    auto mailbox=ordinary;mailbox.mode=app::MessageQueryMode::inbox;mailbox.echo={};auto inbox=store.begin_message_query(bob,mailbox);
    check(!store.message_number_header(bob,*inbox.value,first.value->identity.id.value),"numeric inbox reference never reveals public echo");
    auto named=request(store,alice,{app::MessageKind::public_echo,{2},3,{}},510);const auto unread=store.commit_message(named);
    auto regular=ordinary;regular.mode=app::MessageQueryMode::regular_new;const auto scan=store.begin_message_query(bob,regular);
    const auto candidates=store.query_message_headers(bob,*scan.value,{},50);check(candidates && !candidates.value->entries.empty(),"regular scan entry candidates");
    const auto h=store.message_number_header(bob,*context.value,1);check(static_cast<bool>(h),"old fenced ordinary header remains visible");
    check(static_cast<bool>(store.open_query_message(bob,*scan.value,unread.value->id,true,false)),"traversal open marks inside authoritative writer");
    check(!store.open_query_message(bob,*scan.value,unread.value->id,true,false),"concurrently opened unread no longer traversal eligible");
    auto reverse_scan=store.quote_source(bob,unread.value->id).value->header.position;reverse_scan.reverse=true;reverse_scan.message_id=0;
    const auto revisit=store.query_message_headers(bob,*scan.value,reverse_scan,50);
    check(revisit && std::any_of(revisit.value->entries.begin(),revisit.value->entries.end(),[&](const auto& header){return header.identity.id==unread.value->id;}),"entry progress permits Previous revisit without unbounded ID list");
    auto later=request(store,alice,{app::MessageKind::public_echo,{2},{},{}},511);const auto after=store.commit_message(later);
    check(!store.open_query_message(bob,*scan.value,after.value->id,false,false),"even manual scoped open excludes beyond entry arrival fence");
    unsigned replies{};
    const auto run=[&](std::string_view script,app::MessageQueryOptions query,bool headers,std::optional<std::int64_t> number={}){
        sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text(script);app::StreamCallerTerminal terminal(stream);
        app::MessageReaderPorts ports{store,terminal,[&]{return std::optional<app::MessageActor>{bob};},[&](const app::OpenedMessage& message){++replies;return message.header.target.kind==app::MessageKind::public_echo && message.header.subject=="Subject";},[](std::int64_t){return std::string("test BBS date");},[]{return true;},24,false};
        return app::read_local_messages(ports,query,headers,number);
    };
    check(run("R\r\n-\r\nQ\r\n",ordinary,false,2)==app::MessageReaderOutcome::cancelled && replies==1,"reader stable Reply and Previous dispatch");
    check(run("Q\r\n",ordinary,true)==app::MessageReaderOutcome::cancelled,"browse cancel never forces open or continues across conferences");
    check(run("1\r\nQ\r\n",ordinary,true)==app::MessageReaderOutcome::cancelled,"explicit browse row enters privacy-checked reader");
    auto mail=request(store,alice,{app::MessageKind::private_local,{},3,{}},512);const auto private_message=store.commit_message(mail);
    check(run("D\r\n\r\n",mailbox,false)==app::MessageReaderOutcome::completed,"reader default-Yes own private inbox removal");
    check(store.quote_source(alice,private_message.value->id).value->header.inbox_removed && !store.quote_source(bob,private_message.value->id),"reader deletion preserves sender body but hides recipient inbox");
    check(store.message_scan_preference(bob,{2}).value->highest_opened>=progress,"reader Previous never rewinds durable position");
    check(scalar(file,"SELECT count(*) FROM sfx_message_scan_receipts")==3,"successful bulk changes retain matching receipts only");
}
void session_context_checks(db::SqliteStorage& store,const std::filesystem::path& file,app::MessageActor alice,app::MessageActor bob){
    const auto area=store.message_area(bob,{2});check(area && area.value->group.id.value==1 && area.value->echo.conference_number==2,"session refresh retains stable readable echo");
    const auto counters=store.message_caller_statistics(bob,app::LocalEchoId{2});check(counters && counters.value->echo_count && counters.value->new_messages,"selected readable counters authoritative");
    const auto fence=counters.value->last_arrival;
    auto mail=request(store,alice,{app::MessageKind::private_local,{},3,{}},650);const auto private_post=store.commit_message(mail);
    auto pub=request(store,alice,{app::MessageKind::public_echo,{2},3,{}},651);const auto public_post=store.commit_message(pub);
    auto all=request(store,alice,{app::MessageKind::public_echo,{2},{},{}},652);check(static_cast<bool>(store.commit_message(all)),"All Callers fixture commit not broadcast");
    const auto arrivals=store.message_arrival_page(bob,fence,1);check(arrivals && arrivals.value->arrivals.size()==1 && arrivals.value->arrivals.front().message==private_post.value->id && arrivals.value->next_after,"bounded direct arrival identity continuation");
    const auto next=store.message_arrival_page(bob,*arrivals.value->next_after,100);check(next && next.value->arrivals.size()==1 && next.value->arrivals.front().message==public_post.value->id && !next.value->next_after && next.value->fence>public_post.value->arrival_ordinal,"arrival fence includes All post without broadcasting it");
    check(!store.message_arrival_eligible(alice,private_post.value->id).value.value(),"sender cannot receive another caller arrival");
    auto pref=store.message_scan_preference(bob,{2});store.set_message_scan_preference(bob,{2},pref.value->revision,app::MessageScanColumn::to_you,false);
    check(store.message_arrival_eligible(bob,public_post.value->id).value.value(),"To-you Off is not online direct-delivery optout");
    store.open_message(bob,private_post.value->id);
    check(!store.has_addressed_unread(bob,true).value.value() && store.has_addressed_unread(bob,false).value.value(),"login selection differs from direct unread witness");
    check(!store.message_arrival_eligible(bob,private_post.value->id).value.value(),"opened recipient work no longer eligible");
    app::LocalMessageAdministration admin(store,app::LocalAccountAuthority::installation_owner),recovery(store,app::LocalAccountAuthority::elevated_administrator);
    check(recovery.snapshot({}).code==app::LocalAccountResultCode::unauthorized,"elevated recovery is not message configuration authority");
    const auto snapshot=admin.snapshot({});check(snapshot.code==app::LocalAccountResultCode::ok && snapshot.catalog.line_limits.size()==2 && snapshot.catalog.line_limits.front().maximum_lines==200,"local host returns active finite limits");
    sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text("\r\nQ\r\n");app::StreamCallerTerminal terminal(stream);
    unsigned operation=660;app::MessageComposePorts compose{store,terminal,[&]{return std::optional<app::MessageActor>{bob};},[&]{return id(operation++);},[]{return app::MessageEditorDimensions{false,80,24};},[&]{return terminal.read_editor_byte();},[](std::string_view){return true;},{}};
    app::MessageCallerSession session({compose,[]{return std::string("Bob");},[](std::int64_t){return std::string("2026-10-06");},[]{return true;},{}});
    check(session.initialize() && session.selection() && session.selection()->echo.id.value==1,"message session reconnect initializes first accessible echo");
    check(session.login_scan() && !store.quote_source(bob,public_post.value->id).value->header.recipient_opened,"empty login scan leaves excluded public message unopened");
    const auto before=session.selection()->echo.id;
    check(session.action(app::MenuAction::message_change).has_value() && session.selection()->echo.id==before,"C cancellation preserves stable current selection");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE message_id>0")>0,"session controls do not delete messages");
}
void login_summary_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    auto settings=options(root);db::SqliteStorage store(settings,paths);
    check(store.start().succeeded() && store.shutdown().succeeded(),"login summary fresh fixture");
    const auto file=root/"SFX.DB";seed(file);check(store.start().succeeded(),"login summary actors ready");
    const app::MessageActor alice{id(2),2,1,1},bob{id(3),3,1,1};
    const auto post=[&](app::MessageActor author,app::MessageTarget target,unsigned operation){return store.commit_message(request(store,author,target,operation)).value->id;};
    post(alice,{app::MessageKind::public_echo,{2},{},{}},900);
    const auto named=post(alice,{app::MessageKind::public_echo,{2},3,{}},901);
    const auto self=post(bob,{app::MessageKind::public_echo,{2},3,{}},902);
    const auto received=post(alice,{app::MessageKind::private_local,{},3,{}},903);
    post(bob,{app::MessageKind::private_local,{},2,{}},904);
    post(alice,{app::MessageKind::private_local,{},1,{}},905);
    auto summary=store.login_message_scan_summary(bob);
    check(summary && summary.value->scanned==5 && summary.value->unread_to_you==3 && summary.value->read_to_you==0 && summary.value->from_you==2,"summary authorized totals self counted once and unrelated mail hidden");
    const auto before=store.message_scan_preference(bob,{2}).value->highest_opened;
    check(store.login_message_scan_summary(bob).value->unread_to_you==3 && store.message_scan_preference(bob,{2}).value->highest_opened==before && !store.quote_source(bob,named).value->header.recipient_opened,"summary no read updates");
    check(store.open_message(bob,named).succeeded(),"open named before login");
    summary=store.login_message_scan_summary(bob);
    check(summary.value->unread_to_you==2 && summary.value->read_to_you==1,"read/unread counts separate");
    auto page=store.query_message_headers(bob,summary.value->query,{},50);
    check(page && page.value->entries.size()==2 && !page.value->next && page.value->entries.front().identity.id==self,"login no history phase");
    check(!store.open_query_message(bob,summary.value->query,named,false,false),"manual login open cannot escape into prior history");
    app::MessageQueryCursor history;history.history=true;check(!store.query_message_headers(bob,summary.value->query,history,50),"login history cursor rejected");
    const auto later=post(alice,{app::MessageKind::public_echo,{2},3,{}},906);
    check(!store.open_query_message(bob,summary.value->query,later,false,false),"summary/read same fence");
    check(store.open_query_message(bob,summary.value->query,self,true,false).succeeded() && store.open_query_message(bob,summary.value->query,received,true,false).succeeded(),"actual opens consume unread");
    page=store.query_message_headers(bob,summary.value->query,{},50);
    check(page && page.value->entries.empty() && !page.value->next,"no forward repeat or history fallback");
    auto menu=summary.value->query.options;menu.unread_only=false;
    auto menu_context=store.begin_message_query(bob,menu);auto unread=store.query_message_headers(bob,*menu_context.value,{},50);
    check(unread.value->next && unread.value->next->history && !store.query_message_headers(bob,*menu_context.value,*unread.value->next,50).value->entries.empty(),"menu history retained");
    auto preference=store.message_scan_preference(bob,{2});store.set_message_scan_preference(bob,{2},preference.value->revision,app::MessageScanColumn::to_you,false);
    summary=store.login_message_scan_summary(bob);
    check(summary.value->scanned==2 && summary.value->unread_to_you==0 && summary.value->read_to_you==1 && summary.value->from_you==1,"To-you Off excludes public not own mail");
    check(store.remove_private_inbox_item(bob,received).succeeded(),"remove received fixture");
    summary=store.login_message_scan_summary(bob);check(summary.value->scanned==1 && summary.value->read_to_you==0,"removed inbox excluded");
    preference=store.message_scan_preference(bob,{2});store.set_message_scan_preference(bob,{2},preference.value->revision,app::MessageScanColumn::to_you,true);
    edit(file,"UPDATE sfx_local_echoes SET reading_minimum=999 WHERE echo_id=2");
    check(store.login_message_scan_summary(bob).value->scanned==1,"read access excludes public counts including own posts");
    auto forged=bob;forged.session=alice.session;check(!store.login_message_scan_summary(forged),"summary identity checked");
    edit(file,"UPDATE sfx_message_configuration_state SET revision=revision+1 WHERE singleton_id=1");
    check(!store.query_message_headers(bob,summary.value->query,{},50),"stale summary catalog rejected");
    auto invalid=menu;invalid.mode=app::MessageQueryMode::inbox;invalid.unread_only=true;check(!store.begin_message_query(bob,invalid),"login flag not general policy");
    check(store.shutdown().succeeded() && !store.login_message_scan_summary(bob),"unavailable never zero summary");
}
void lifecycle_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    auto settings=options(root);db::SqliteStorage store(settings,paths);check(store.start().succeeded(),"lifecycle fresh schema");check(store.shutdown().succeeded(),"lifecycle fixture stop");
    const auto file=root/"SFX.DB";seed(file);
    edit(file,"INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(4,'Replacement','replacement',999,'active',1,1,1700000000,1700000000); INSERT INTO sfx_account_statistics(account_id) VALUES(4); DELETE FROM sfx_account_sessions WHERE account_id=1");
    check(store.start().succeeded(),"lifecycle restart");unsigned operation=750;
    const auto audit=[&](app::AuditReasonCode reason){const auto n=operation++;return app::AuditContext{id(n),id(n+1000),{},1791278000,reason};};
    const auto protected_result=[&](const app::MutationResult& r){return !r && r.status.code==app::StorageErrorCode::system_sysop_protected;};
    check(protected_result(store.assign_security_level({1,1,1,10,audit(app::AuditReasonCode::sysop_action)})),"direct level assignment protects designated Sysop");
    app::SaveAccountRequest save;save.account_id=1;save.expected_revision=1;save.expected_policy_revision=1;save.display_name="Changed Sysop";save.security_level=10;save.purge_allowed=true;save.audit=audit(app::AuditReasonCode::sysop_action);
    const auto edited=store.save_account(save);if(!protected_result(edited))std::cerr<<"EDITOR REFUSAL code="<<static_cast<unsigned>(edited.status.code)<<" operation="<<edited.status.operation<<'\n';
    check(protected_result(edited) && store.find_account_by_id(1).value->display_name=="Sysop","whole user-editor transaction rolls back rename and demotion");
    for(const auto state:{app::AccountState::locked,app::AccountState::deletion_scheduled}){
        app::ChangeAccountStateRequest change{1,1,state,{},audit(app::AuditReasonCode::sysop_action)};if(state==app::AccountState::deletion_scheduled)change.deletion_scheduled_utc_seconds=1791279000;
        check(protected_result(store.change_account_state(change)),"locking/scheduled deletion protects assigned Sysop");
    }
    check(protected_result(store.tombstone_account({1,1,app::AuditReasonCode::sysop_action,audit(app::AuditReasonCode::sysop_action)})),"tombstone cannot invalidate assignment");
    const auto assign=[&](app::AccountId account){const auto before=store.message_configuration({true,{}},{},{});app::MessageConfigurationChange c;c.operation=id(operation++);c.action=app::MessageConfigurationAction::system_sysop;c.expected_configuration_revision=before.value->revision;c.expected_assignment_revision=before.value->assignment_revision;c.sysop=account;c.expected_account_revision=store.find_account_by_id(account).value->revision;return store.change_message_configuration({true,{}},c);};
    check(static_cast<bool>(assign(4)),"explicit eligible replacement assignment");
    check(store.change_account_state({1,1,app::AccountState::locked,{},audit(app::AuditReasonCode::sysop_action)}).succeeded(),"formerly assigned Sysop may be locked after replacement");
    check(store.tombstone_account({1,2,app::AuditReasonCode::sysop_action,audit(app::AuditReasonCode::sysop_action)}).succeeded(),"formerly assigned account may be tombstoned without transferring mail");
    auto policy=*store.security_policy().value;app::PublishSecurityPolicyRequest change;change.expected_revision=policy.revision;change.levels=*store.security_levels().value;change.new_user_level=10;change.sysop_threshold=10;change.policy_hash=change.published_policy_hash=std::string(64,'b');change.audit=audit(app::AuditReasonCode::security_policy);
    check(store.publish_security_policy(change).succeeded(),"lower threshold allows another eligible designated account");
    check(static_cast<bool>(assign(2)),"designation binds numeric identity, not bootstrap name");
    change.expected_revision=store.security_policy().value->revision;change.sysop_threshold=999;change.audit=audit(app::AuditReasonCode::security_policy);
    const auto denied=store.publish_security_policy(change);check(!denied && denied.code==app::StorageErrorCode::system_sysop_protected && store.security_policy().value->sysop_threshold==10,"global threshold guard rolls back policy and preserves designation");
    for(auto& level:change.levels)if(level.level==10)level.state=app::SecurityLevelState::retired;
    change.new_user_level=999;change.reassignments={{10,999}};change.audit=audit(app::AuditReasonCode::security_policy);
    check(store.publish_security_policy(change).succeeded() && store.find_account_by_id(2).value->security_level==999,"retired-level reassignment preserves eligible System Sysop atomically");
    check(store.find_account_by_id(1).value->state==app::AccountState::tombstoned && *store.message_configuration({true,{}},{},{}).value->system_sysop==2,"bootstrap tombstone never changes current designation");
    check(store.shutdown().succeeded(),"lifecycle fixture stopped");
}
void concurrency_fault_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    auto settings=options(root);db::SqliteStorage store(settings,paths);
    check(store.start().succeeded() && store.shutdown().succeeded(),"fresh concurrency fixture");
    const auto file=settings.data_directory/"SFX.DB";seed(file);check(store.start().succeeded(),"concurrent fixture restart");
    const app::MessageActor alice{id(2),2,1,1},bob{id(3),3,1,1};
    const app::MessageTarget public_target{app::MessageKind::public_echo,{2},{},{}},named{app::MessageKind::public_echo,{2},3,{}};
    auto same=request(store,alice,public_target,900);
    std::vector<std::future<app::StorageResult<app::CommittedMessage>>> operations;
    for(unsigned n=0;n<16;++n)operations.push_back(std::async(std::launch::async,[&]{return store.commit_message(same);}));
    std::optional<app::MessageId> single;
    for(auto& pending:operations){const auto result=pending.get();check(result && (!single || result.value->id==*single),"simultaneous same-operation retry returns one identity");single=result.value->id;}
    check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==1 && scalar(file,"SELECT count(*) FROM sfx_message_commit_receipts")==1,"concurrent retry counts and receipt once");
    operations.clear();
    for(unsigned n=0;n<31;++n){auto different=same;different.operation=id(901+n);operations.push_back(std::async(std::launch::async,[&,different]{return store.commit_message(different);}));}
    std::set<std::int64_t> numbers{1};
    for(auto& pending:operations){const auto result=pending.get();check(result && numbers.insert(result.value->area_number).second,"concurrent distinct commit allocates unique monotonic number");}
    check(numbers.size()==32 && *numbers.rbegin()==32 && scalar(file,"SELECT next_message_number FROM sfx_local_echoes WHERE echo_id=2")==33,"no committed numbers reused or renumbered");
    const auto fail=request(store,alice,named,950);
    edit(file,"CREATE TRIGGER fixture_message_receipt_failure BEFORE INSERT ON sfx_message_commit_receipts BEGIN SELECT RAISE(ABORT,'synthetic late failure'); END");
    check(!store.commit_message(fail),"late receipt failure truthful after allocations/body/recipient work");
    check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==32 && scalar(file,"SELECT next_message_number FROM sfx_local_echoes WHERE echo_id=2")==33 && scalar(file,"SELECT count(*) FROM sfx_local_messages")==32 && scalar(file,"SELECT count(*) FROM sfx_message_arrival_intents")==0 && scalar(file,"SELECT count(*) FROM sfx_message_recipient_state")==0 && scalar(file,"SELECT messages_entered FROM sfx_caller_message_statistics WHERE account_id=2")==32,"late failure rolls back number, body, counters, recipient state, intent and receipt together");
    const auto absence=store.message_commit_outcome(fail);check(absence && !absence.value->committed,"rolled-back receipt absence confirmed, not unknown success");
    edit(file,"DROP TRIGGER fixture_message_receipt_failure");
    const auto retry=store.commit_message(fail);check(retry && retry.value->area_number==33,"same operation retries rolled-back allocation safely");
    check(store.reconcile_message(fail).value->id==retry.value->id && store.commit_message(fail).value->id==retry.value->id,"lost result reconciles/retries without duplicate post");
    check(store.open_message(bob,retry.value->id).value->header.recipient_opened,"recipient state commits with successfully retried post");
    check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==33 && scalar(file,"SELECT count(*) FROM sfx_message_arrival_intents")==1,"success-only post/arrival accounting after faults");
    const auto full=request(store,alice,public_target,951);
    edit(file,"UPDATE sfx_local_echoes SET next_message_number=9223372036854775807 WHERE echo_id=2");
    const auto exhausted=store.commit_message(full);check(!exhausted && exhausted.status.code==app::StorageErrorCode::full,"real adapter SQLite FULL outcome truthful; finite allocation exhaustion injection");
    check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==33 && !store.message_commit_outcome(full).value->committed,"FULL result creates no content/counter/receipt");
    edit(file,"UPDATE sfx_local_echoes SET next_message_number=34 WHERE echo_id=2");
    const auto recovered=store.commit_message(full);check(recovered && recovered.value->area_number==34,"same draft/operation recovers after FULL without duplicate counter");
    const auto busy_request=request(store,alice,public_target,952);
    {internal::SqliteConnection held(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);held.exec("BEGIN IMMEDIATE","hold only synthetic fixture writer");
        const auto blocked=store.commit_message(busy_request);check(!blocked && blocked.status.code==app::StorageErrorCode::busy,"real competing SQLite writer returns bounded busy rather than claimed commit");held.exec("ROLLBACK","release only test transaction");}
    check(!store.message_commit_outcome(busy_request).value->committed && scalar(file,"SELECT total_committed FROM sfx_message_counters")==34,"busy outcome confirms absence and leaves success accounting unchanged");
    check(store.commit_message(busy_request).value->area_number==35,"busy same-operation retry commits once after lock release");
    edit(file,"CREATE TRIGGER fixture_message_read_failure BEFORE UPDATE ON sfx_caller_echo_state BEGIN SELECT RAISE(ABORT,'synthetic read-state failure'); END");
    check(!store.open_message(bob,recovered.value->id) && scalar(file,"SELECT highest_opened FROM sfx_caller_echo_state WHERE account_id=3 AND echo_id=2")==33,"read-state failure returns no successful open and preserves monotonic position");
    edit(file,"DROP TRIGGER fixture_message_read_failure");
    check(store.shutdown().succeeded() && store.start().succeeded(),"reopen durable message/receipt/sequence state");
    check(store.reconcile_message(fail).value->id==retry.value->id && store.message_scan_preference(bob,{2}).value->highest_opened==33,"restart preserves receipt identity and monotonic recipient progress");
    check(store.shutdown().succeeded(),"concurrency fixture stopped");
}
void io_fault_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    check(sfx::test::install_sqlite_write_fault_vfs("section8a-io-fault"),"install scoped delegating write-fault VFS");
    try{
        auto settings=options(root/"section8a-io-fault");db::SqliteStorage store(settings,paths);
        check(store.start().succeeded() && store.shutdown().succeeded(),"fresh write-fault fixture");
        const auto file=settings.data_directory/"SFX.DB";seed(file);check(store.start().succeeded(),"write-fault actors ready");
        const app::MessageActor alice{id(2),2,1,1},bob{id(3),3,1,1};
        const app::MessageTarget target{app::MessageKind::private_local,{},3,{}};
        unsigned operation=980;
        for(const auto code:{SQLITE_FULL,SQLITE_IOERR_WRITE}){
            const auto draft=request(store,alice,target,operation++);
            const auto before=scalar(file,"SELECT total_committed FROM sfx_message_counters");
            sfx::test::inject_next_sqlite_write_fault(code);
            const auto failed=store.commit_message(draft);
            check(!failed && failed.status.code==(code==SQLITE_FULL?app::StorageErrorCode::full:app::StorageErrorCode::io) && sfx::test::pending_sqlite_write_fault()==SQLITE_OK,"actual production WAL xWrite fault is consumed and truthful");
            check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==before && scalar(file,"SELECT count(*) FROM sfx_local_messages")==before && scalar(file,"SELECT count(*) FROM sfx_message_arrival_intents")==before,"failed physical write publishes no message/counter/arrival");
            const auto absent=store.message_commit_outcome(draft);check(absent && !absent.value->committed,"write failure reconciles known absence");
            const auto retried=store.commit_message(draft);check(retried && store.commit_message(draft).value->id==retried.value->id,"retained operation retries once after WAL write failure");
        }
        const auto snapshot=store.message_configuration({true,{}},{},{});
        app::MessageConfigurationChange change;change.operation=id(985);change.action=app::MessageConfigurationAction::group;change.expected_configuration_revision=snapshot.value->revision;change.group=snapshot.value->groups.front();change.group.name="Not published on failure";
        sfx::test::inject_next_sqlite_write_fault(SQLITE_IOERR_WRITE);
        const auto failure=store.change_message_configuration({true,{}},change);
        check(!failure && sfx::test::pending_sqlite_write_fault()==SQLITE_OK,"configuration write/activation failure is not claimed published");
        const auto unchanged=store.message_configuration({true,{}},{},{});
        check(unchanged.value->revision==snapshot.value->revision && unchanged.value->groups.front().name=="Local","failed configuration keeps authoritative gate/revision unchanged");
        const auto applied=store.change_message_configuration({true,{}},change);check(applied && store.change_message_configuration({true,{}},change).value->configuration_revision==applied.value->configuration_revision,"configuration operation replay reconciles one published revision");
        const auto pref=store.message_scan_preference(bob,{2});check(static_cast<bool>(pref),"read-state fixture initialized before failure");
        const auto message=store.message_commit_outcome(request(store,alice,target,986));check(message && !message.value->committed,"unused operation absent");
        const auto private_id=store.commit_message(request(store,alice,target,987)).value->id;
        sfx::test::inject_next_sqlite_write_fault(SQLITE_FULL);
        check(!store.open_message(bob,private_id) && sfx::test::pending_sqlite_write_fault()==SQLITE_OK,"read-state WAL failure yields no successful open");
        check(!store.quote_source(alice,private_id).value->header.recipient_opened,"failed open leaves recipient unread");
        check(store.open_message(bob,private_id).value->header.recipient_opened,"read-state retry durably opens once");
        check(store.shutdown().succeeded(),"write-fault fixture stopped");
    }catch(...){sfx::test::uninstall_sqlite_write_fault_vfs();throw;}
    sfx::test::uninstall_sqlite_write_fault_vfs();
}
void administration_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    auto settings=options(root);db::SqliteStorage store(settings,paths);check(store.start().succeeded() && store.shutdown().succeeded(),"new administration fixture");
    const auto file=root/"SFX.DB";seed(file);check(store.start().succeeded(),"admin actors restart");
    const app::MessageActor alice{id(2),2,1,1},bob{id(3),3,1,1};const app::MessageConfigurationActor owner{true,{}};
    unsigned next=1200;
    const auto snapshot=[&](app::MessageGroupId group){return store.message_configuration(owner,group,{}).value.value();};
    const auto change=[&](app::MessageConfigurationChange c){c.operation=id(next++);c.expected_configuration_revision=snapshot({1}).revision;return store.change_message_configuration(owner,c);};
    auto parent=request(store,alice,{app::MessageKind::public_echo,{2},3,{}},1100);const auto post=store.commit_message(parent);check(static_cast<bool>(post),"purge addressed fixture");
    auto self=request(store,bob,{app::MessageKind::public_echo,{2},3,{}},1101);const auto self_post=store.commit_message(self);check(static_cast<bool>(self_post),"self addressed public fixture");
    auto other=request(store,alice,{app::MessageKind::public_echo,{2},2,{}},1102);check(static_cast<bool>(store.commit_message(other)),"other public direction fixture");
    auto private_request=request(store,alice,{app::MessageKind::private_local,{},3,{}},1103);const auto mail=store.commit_message(private_request);check(static_cast<bool>(mail),"private isolation fixture");
    for(const auto direction:{app::MessageCallerDirection::to,app::MessageCallerDirection::from,app::MessageCallerDirection::both}){
        app::MessageQueryOptions o;o.mode=app::MessageQueryMode::addressed;o.echo={2};o.addressed_to=3;o.caller_direction=direction;
        const auto query=store.begin_message_query(alice,o);const auto headers=store.query_message_headers(alice,*query.value,{},100);
        check(headers && headers.value->entries.size()==(direction==app::MessageCallerDirection::from?std::size_t{1}:std::size_t{2}),"To From Both include already-read public union without private mail or self duplicates");
        check(!store.open_query_message(alice,*query.value,mail.value->id,false,false),"S scoped body rejects private IDs");
    }
    check(store.resolve_message_recipient(alice,"3",{},true).value->entries.empty(),"caller numeric ID not a name");
    edit(file,"INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(4,'3','3',10,'active',1,1,1700000000,1700000000)");
    check(store.resolve_message_recipient(alice,"3",{},true).value->entries.front().account==4 && store.resolve_message_recipient(alice,"3").value->entries.front().account==3,"numeric names and internal numeric identity remain distinct");
    check(static_cast<bool>(store.open_message(bob,post.value->id)),"read progress before ordering");const auto progress=store.message_scan_preference(bob,{2}).value.value();
    const auto before=snapshot({1});const auto anchor=before.echoes[1];app::MessageCatalogCursor cursor;cursor.configuration_revision=before.revision;
    app::MessageConfigurationChange c;c.action=app::MessageConfigurationAction::insert_echo;c.echo.group={1};c.echo.name="SFX Support Echo";c.insert_before=anchor.id;c.insert_before_revision=anchor.revision;
    const auto inserted=change(c);check(static_cast<bool>(inserted),"insert before stable echo");const app::LocalEchoId support{inserted.value->entity_id};
    auto visible=store.local_echoes(alice,{1});check(visible && visible.value->size()==3 && (*visible.value)[1].id==support && (*visible.value)[1].conference_number==2 && (*visible.value)[2].id.value==2 && (*visible.value)[2].conference_number==3,"inserted caller gap-free group order");
    check(!store.local_echo_page(alice,{1},cursor),"old number page fails closed after insertion");
    check(store.message_scan_preference(bob,{2}).value->highest_opened==progress.highest_opened,"insertion preserves stable echo read progress");
    auto fresh=snapshot({1});c={};c.action=app::MessageConfigurationAction::move_echo_down;c.echo=fresh.echoes[1];check(static_cast<bool>(change(c)),"explicit down swaps adjacent echoes");
    visible=store.local_echoes(alice,{1});check((*visible.value)[1].id.value==2 && (*visible.value)[2].id==support,"swap changes only presentation identity order");
    fresh=snapshot({1});c.action=app::MessageConfigurationAction::move_echo_up;c.echo=fresh.echoes.back();check(static_cast<bool>(change(c)),"explicit up restores order");
    fresh=snapshot({1});auto support_echo=*std::find_if(fresh.echoes.begin(),fresh.echoes.end(),[&](const auto& e){return e.id==support;});
    c={};c.action=app::MessageConfigurationAction::echo;c.echo=support_echo;c.echo.permissions.reading=999;check(static_cast<bool>(change(c)),"restrict reading without replacing identity");
    visible=store.local_echoes(alice,{1});check(visible.value->size()==2 && visible.value->back().id.value==2 && visible.value->back().conference_number==2,"caller excludes unreadable echo without numbering gap");
    const app::MessageActor sysop{id(1),1,1,1};check(store.local_echoes(sysop,{1}).value->size()==3 && store.message_area(sysop,{2}).value->echo.conference_number==3,"different callers get group-readable numbering");
    check(store.open_message(bob,post.value->id).value->header.conference_number==2,"header uses same caller number as list and selected area");
    c={};c.action=app::MessageConfigurationAction::group;c.group.name="Other";auto group=change(c);check(static_cast<bool>(group),"cross-group fixture");
    fresh=snapshot({1});support_echo=*std::find_if(fresh.echoes.begin(),fresh.echoes.end(),[&](const auto& e){return e.id==support;});
    c={};c.action=app::MessageConfigurationAction::echo;c.echo=support_echo;c.echo.group={group.value->entity_id};c.echo.permissions.reading=1;check(static_cast<bool>(change(c)),"cross-group move keeps stable identity");
    check(store.local_echoes(alice,c.echo.group).value->front().conference_number==1 && store.message_area(alice,{2}).value->echo.conference_number==2,"numbering restarts per group after move");
    // A surviving private reply retains its bytes but loses a purged parent link.
    const auto link_sql="INSERT INTO sfx_local_messages(kind,author_account_id,recipient_account_id,subject,body,encoding,body_lines,arrival_ordinal,created_utc_seconds,parent_message_id) SELECT 1,author_account_id,recipient_account_id,subject,body,encoding,body_lines,100,created_utc_seconds,"+std::to_string(post.value->id.value)+" FROM sfx_local_messages WHERE message_id="+std::to_string(mail.value->id.value);
    edit(file,link_sql.c_str());edit(file,"UPDATE sfx_message_counters SET last_arrival_ordinal=100,total_committed=total_committed+1");
    bool protected_body=false;try{edit(file,"DELETE FROM sfx_local_messages WHERE echo_id=2");}catch(const internal::StorageFailure&){protected_body=true;}check(protected_body,"ordinary message deletion remains prohibited");
    const auto backup=root/"before-permanent-delete.db";check(store.create_backup(backup).status.succeeded(),"whole DB backup before destructive operation");
    const auto original_count=scalar(file,"SELECT count(*) FROM sfx_local_messages");const auto original_total=scalar(file,"SELECT total_committed FROM sfx_message_counters");
    fresh=snapshot({1});c={};c.action=app::MessageConfigurationAction::delete_echo;c.echo=*std::find_if(fresh.echoes.begin(),fresh.echoes.end(),[](const auto& e){return e.id.value==2;});c.operation=id(next++);c.expected_configuration_revision=fresh.revision;
    check(!store.change_message_configuration({false,alice},c) && scalar(file,"SELECT count(*) FROM sfx_local_messages")==original_count,"non-Sysop deletion refused without collateral loss");
    auto stale=c;stale.echo.revision=999;check(!store.change_message_configuration(owner,stale) && scalar(file,"SELECT count(*) FROM sfx_message_purge_scope")==0,"stale delete refuses before purge context");
    edit(file,"CREATE TRIGGER test_echo_delete_abort BEFORE DELETE ON sfx_local_echoes WHEN OLD.echo_id=2 BEGIN SELECT RAISE(ABORT,'injected final purge failure'); END");
    check(!store.change_message_configuration(owner,c) && scalar(file,"SELECT count(*) FROM sfx_local_messages")==original_count && scalar(file,"SELECT count(*) FROM sfx_message_purge_scope")==0 && scalar(file,"SELECT count(*) FROM sfx_message_purged_receipts")==0,"late purge failure rolls back every body, receipt and guard");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE kind=1 AND parent_message_id IS NOT NULL")==1,"late failure restores detached surviving parent link too");
    edit(file,"DROP TRIGGER test_echo_delete_abort");
    const auto deleted=store.change_message_configuration(owner,c);check(deleted && scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE echo_id=2")==0 && scalar(file,"SELECT count(*) FROM sfx_local_echoes WHERE echo_id=2")==0,"permanent selected echo and public content removed");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE kind=1")==2 && scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE kind=1 AND parent_message_id IS NOT NULL")==0,"private bodies survive with only purged parent detached");
    check(store.open_message(bob,mail.value->id).value->body.canonical_lf==private_request.body,"private mailbox item bytes preserved");
    check(scalar(file,"SELECT count(*) FROM sfx_caller_echo_state WHERE echo_id=2")==0 && scalar(file,"SELECT count(*) FROM sfx_message_purge_scope")==0 && scalar(file,"SELECT total_committed FROM sfx_message_counters")==original_total,"dependent scan state purged and historical counters retained");
    check(store.change_message_configuration(owner,c).value->replayed,"delete idempotent receipt returns original outcome");
    check(!store.commit_message(parent) && !store.reconcile_message(parent) && !store.message_commit_outcome(parent),"purged commit retry never resurrects or reports not-committed success");
    check(store.local_echoes(alice,{1}).value->size()==1 && store.local_echoes(alice,{1}).value->front().conference_number==1,"caller numbering gap-free after removal");
    app::MessageConfigurationChange replacement;replacement.action=app::MessageConfigurationAction::echo;replacement.echo.group={1};replacement.echo.name="Later echo";
    const auto later=change(replacement);check(later && later.value->entity_id>support.value,"AUTOINCREMENT never reuses a deleted echo identity");
    check(store.shutdown().succeeded() && store.validate_backup(backup).succeeded() && store.restore_backup(backup).succeeded() && store.start().succeeded(),"production storage restore of pre-delete recovery");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==original_count && store.reconcile_message(parent).value->id==post.value->id,"restore recovers echo messages and original receipts");
    check(store.shutdown().succeeded(),"administration fixture stopped");
}
void group_migration_faults(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    for(const char* point:{"before-message-group-migration","before-message-group-migration-commit","after-message-group-migration-commit"}){
        auto s=options(root/point);auto prior_plan=db::sfx_storage_migrations();prior_plan.resize(9);
        {db::SqliteStorage prior(s,policy,prior_plan);check(prior.start().succeeded() && prior.shutdown().succeeded(),"exact v9 group migration predecessor");}
        seed(s.data_directory/"SFX.DB");const auto backup=s.data_directory/"prior-v9.db";
        {db::SqliteStorage prior(s,policy,prior_plan);check(prior.start().succeeded() && prior.create_backup(backup).status.succeeded() && prior.shutdown().succeeded(),"populated v9 actual recovery backup");}
        s.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"group migration fault"}:app::StorageStatus::success();};
        {db::SqliteStorage failed(s,policy);check(!failed.start(),"group migration failure truthful");}
        {internal::SqliteConnection c(s.data_directory/"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(c.get(),true),"group fault preserves exact recognized schema");check(c.scalar_int64("PRAGMA user_version","atomic group migration")== (std::string_view(point)=="after-message-group-migration-commit"?10:9),"group migration ledger atomic");}
        s.bootstrap_boundary_hook={};db::SqliteStorage retry(s,policy);check(retry.start().succeeded() && retry.health().schema_version==31 && retry.shutdown().succeeded(),"group migration retry once");
        check(retry.validate_backup(backup).succeeded() && retry.restore_backup(backup).succeeded() && retry.start().succeeded(),"actual v9 backup restore and remigration");
        check(scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_accounts")==3 && scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_schema_migrations")==31,"group upgrade preserves accounts and thirty-one exact ledger entries");
        check(retry.shutdown().succeeded(),"group migration recovery stopped");
    }
}
void group_delete_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto s=options(root);db::SqliteStorage store(s,policy);check(store.start().succeeded() && store.shutdown().succeeded(),"empty-group fixture bootstrapped");
    const auto file=root/"SFX.DB";seed(file);check(store.start().succeeded(),"group delete actors ready");
    const app::MessageConfigurationActor owner{true,{}};const app::MessageActor alice{id(2),2,1,1};unsigned op=1100;
    const auto catalog=[&]{return *store.message_configuration(owner,{},{}).value;};
    const auto change=[&](app::MessageConfigurationChange c){c.operation=id(op++);c.expected_configuration_revision=catalog().revision;return store.change_message_configuration(owner,c);};
    app::MessageConfigurationChange create;create.action=app::MessageConfigurationAction::group;create.group.name="Accidental group";auto created=change(create);check(static_cast<bool>(created),"create empty group through typed authority");
    app::MessageConfigurationChange del;del.action=app::MessageConfigurationAction::delete_group;del.group.id={created.value->entity_id};del.group.revision=created.value->entity_revision;del.expected_configuration_revision=catalog().revision;del.operation=id(op++);
    check(!store.change_message_configuration({false,alice},del),"ordinary caller cannot delete group");auto stale=del;stale.group.revision=999;check(!store.change_message_configuration(owner,stale),"stale group revision refused");
    const auto backup=root/"before-group-delete.db";check(store.create_backup(backup).status.succeeded(),"group deletion recovery backup");
    edit(file,"CREATE TRIGGER group_delete_fault BEFORE DELETE ON sfx_message_groups WHEN OLD.name='Accidental group' BEGIN SELECT RAISE(ABORT,'late group failure'); END");
    check(!store.change_message_configuration(owner,del) && catalog().revision==del.expected_configuration_revision && scalar(file,"SELECT count(*) FROM sfx_message_groups")==2,"group transaction failure preserves group and revision");
    edit(file,"DROP TRIGGER group_delete_fault");const auto removed=store.change_message_configuration(owner,del);check(removed && removed.value->entity_id==del.group.id.value && scalar(file,"SELECT count(*) FROM sfx_message_groups")==1,"confirmed empty group removed only");
    check(store.change_message_configuration(owner,del).value->replayed,"group delete retry reconciles deleted entity without recreation");auto substituted=del;++substituted.group.id.value;check(!store.change_message_configuration(owner,substituted),"group delete OperationId cannot redirect");
    auto next=change(create);check(next && next.value->entity_id>del.group.id.value,"removed group identity never reused");
    app::MessageConfigurationChange child;child.action=app::MessageConfigurationAction::echo;child.echo.group={next.value->entity_id};child.echo.name="Disabled child";child.echo.enabled=false;auto echoed=change(child);check(static_cast<bool>(echoed),"disabled child created in group");
    del.group.id={next.value->entity_id};del.group.revision=next.value->entity_revision;del.operation=id(op++);del.expected_configuration_revision=catalog().revision;
    const auto before=del.expected_configuration_revision;check(!store.change_message_configuration(owner,del) && catalog().revision==before && scalar(file,"SELECT count(*) FROM sfx_local_echoes")==3,"disabled child blocks group delete atomically without cascade");
    del.operation=id(op++);del.group=catalog().groups.front();del.expected_configuration_revision=catalog().revision;check(!store.change_message_configuration(owner,del),"populated Local group refused");
    del.group.id={next.value->entity_id};del.group.revision=next.value->entity_revision;--del.expected_configuration_revision;check(!store.change_message_configuration(owner,del),"new child invalidates captured catalog selection");
    check(scalar(file,"SELECT count(*) FROM sfx_message_configuration_audit WHERE action=9")==1,"one deletion audit only; failed operations produce no receipt");
    check(store.shutdown().succeeded() && store.restore_backup(backup).succeeded() && store.start().succeeded(),"restore deleted group from production whole DB backup");
    check(scalar(file,"SELECT count(*) FROM sfx_message_groups")==2 && scalar(file,"SELECT count(*) FROM sfx_local_echoes")==2,"group restore recovers prior catalog exactly");check(store.shutdown().succeeded(),"group deletion fixture stopped");
}
#include "message_delete_checks.inc"
#include "message_reader_color_checks.inc"
#include "message_footer_checks.inc"
#include "message_source_checks.inc"
#include "beta8_private_defaults_checks.inc"
#include "message_upload_checks.inc"
#include "message_maintenance_checks.inc"
#include "message_retention_execution_checks.inc"
#include "message_retention_fault_checks.inc"
#include "message_area_deletion_checks.inc"
#include "message_compaction_checks.inc"
#include "message_group_deletion_checks.inc"
static void beta13_preview_checks(db::SqliteStorage& store,app::MessageActor sysop,
    app::MessageActor alice,app::MessageActor bob,app::MessageId public_id,app::MessageId private_id){
    app::MessageQueryOptions echo;echo.echo={2};
    const auto sysop_context=store.begin_message_query(sysop,echo);
    const auto bob_context=store.begin_message_query(bob,echo);
    check(sysop_context && bob_context,"capture scoped Preview contexts");
    const auto allowed=store.open_query_message(sysop,*sysop_context.value,public_id,false,false,true);
    const auto refused=store.open_query_message(bob,*bob_context.value,public_id,false,false,true);
    check(allowed && allowed.value->header.identity.id==public_id &&
        store.message_scan_preference(sysop,{2}).value->highest_opened==0,"Preview returns authorized body without advancing public progress");
    check(!refused && refused.status.code==app::StorageErrorCode::permission_denied,"Preview requires the configured System Sysop");
    app::MessageQueryOptions inbox;inbox.mode=app::MessageQueryMode::inbox;
    const auto inbox_context=store.begin_message_query(sysop,inbox);
    check(bool(inbox_context),"capture Sysop's own inbox scope");
    const auto preview_mail=store.open_query_message(sysop,*inbox_context.value,private_id,false,false,true);
    check(preview_mail && !preview_mail.value->header.recipient_opened &&
        !store.quote_source(sysop,private_id).value->header.recipient_opened,"Preview leaves original private mail unread");
    app::MessageQueryOptions from;from.mode=app::MessageQueryMode::correspondence;
    from.caller_direction=app::MessageCallerDirection::from;
    const auto correspondence=store.begin_message_query(alice,from);
    check(bool(correspondence),"capture From You scope");
    const auto headers=store.query_message_headers(alice,*correspondence.value,{},10);
    check(headers && headers.value->entries.size()==2 && headers.value->entries[0].identity.id==public_id &&
        headers.value->entries[1].identity.id==private_id,"From You lists the caller's public post and private sent copy");

}

static void beta13_reader_workflows(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    const auto fixture=options(root);
    db::SqliteStorage store(fixture,policy);
    check(store.start().succeeded() && store.shutdown().succeeded(),"fresh reader workflow store");
    seed(fixture.data_directory/"SFX.DB");
    check(store.start().succeeded(),"reader workflow actors ready");
    const app::MessageActor sysop{id(1),1,1,1},alice{id(2),2,1,1},bob{id(3),3,1,1};
    auto posted=request(store,alice,{app::MessageKind::public_echo,{2},{},{}},770);
    posted.subject="Beta13 listing subject";
    const auto public_post=store.commit_message(posted);
    const auto private_post=store.commit_message(request(store,alice,{app::MessageKind::private_local,{},1,{}},771));
    check(public_post && private_post,"reader workflow fixtures posted");
    beta13_preview_checks(store,sysop,alice,bob,public_post.value->id,private_post.value->id);

    const auto run=[&](app::MessageActor actor,app::MenuAction action,std::string_view keys,bool ansi=false,unsigned columns=80){
        check(store.remember_message_area(actor,{2}).succeeded(),"set initial echo for reader workflow");
        sfx::test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text(keys);
        app::StreamCallerTerminal terminal(stream);
        terminal.set_text_style(ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
        const auto output=[&](std::string_view value){return stream.write_some(value).bytes_written==value.size();};
        app::MessageQuestionTerminal framed(terminal,[&]{return ansi;},output,[&]{return columns;});
        app::MessageComposePorts compose{store,framed,[&]{return std::optional<app::MessageActor>{actor};},
            []{return id(772);},[&]{return app::MessageEditorDimensions{ansi,columns,24};},
            [&]{return terminal.read_editor_byte();},output};
        app::MessageCallerSession session({compose,[]{return std::string("Fixture");},[](std::int64_t){return std::string("2026-11-22");},
            []{return true;},[]{return app::TerminalDisplayPagingPolicy{false,std::uint16_t{24}};}});
        check(session.initialize() && session.action(action).has_value(),"caller workflow returns normally");
        return std::string(stream.output());
    };
    const auto preview=run(sysop,app::MenuAction::message_read,"T\r\nL\r\n1\r\nY\r\nQ\r\n");
    check(preview.find("do you wish to preview messages")!=preview.npos &&
        preview.find("Beta13 listing subject")!=preview.npos &&
        store.message_scan_preference(sysop,{2}).value->highest_opened==0,
        "Read This opens a listed body with configured Sysop Preview and preserved position");
    const auto browse=run(bob,app::MenuAction::message_browse,"\r\n1\r\n\r\nQ\r\n");
    const auto details=browse.find("Subject: Beta13 listing subject");
    check(details!=browse.npos && browse.find("Number, [P] Previous",details)!=browse.npos &&
        browse.find("do you wish to preview messages")==browse.npos &&
        store.message_scan_preference(bob,{2}).value->highest_opened==1,
        "normal caller reads a listed body and Enter returns to the numbered list");
    const auto filter=run(sysop,app::MenuAction::message_personal,"YLQ",true);
    const auto visible=[](std::string_view text){
        std::string result;
        for(std::size_t i=0;i<text.size();++i){
            if(text[i]=='\x1b' && i+1<text.size() && text[i+1]=='['){
                i+=2;
                while(i<text.size() && (text[i]<'@' || text[i]>'~'))++i;
            }else result+=text[i];
        }
        return result;
    };
    const auto filter_text=visible(filter);
    check(filter_text.find("Private Inbox\r\n")!=filter_text.npos &&
        filter_text.find("[S] Sent Private/NetMail, [Q] Quit [B]")!=filter_text.npos &&
        filter_text.find("[T] To You",filter_text.find("[Q] Quit [B]"))==filter_text.npos,
        "personal filter uses a second line and Quit exits the scan");
    const auto selection=run(sysop,app::MenuAction::message_read,"TL\r1\rYQ",true,64);
    const auto question=selection.find("do you wish to preview messages?");
    const auto anchor=selection.rfind("\x1b[22;1H\x1b[0;37;40m\x1b[2K",question);
    check(question!=selection.npos && anchor!=selection.npos && question-anchor<100,
        "ANSI header selection anchors Preview at the input row");
    const auto handoff=run(sysop,app::MenuAction::message_read,"T1\rYLQQ",true,64);
    std::ofstream(root/"ansi-reader-handoff.txt",std::ios::binary)<<handoff;
    const auto first_body=handoff.find("Subject: ");
    const auto reopened_headers=handoff.find("Page 1   Number:",first_body);
    const auto restored=handoff.find("Subject: ",reopened_headers);
    check(first_body!=handoff.npos && reopened_headers!=handoff.npos && restored!=handoff.npos &&
        visible(handoff.substr(restored)).find("[Q] Quit")!=handoff.npos,
        "nested List Quit restores the body before the reader command footer");
    check(visible(handoff).find("[P]/[N] Page [D] Delete, [Q] Quit")!=handoff.npos,
        "narrow ANSI lightbar keeps Delete and Quit visible together");
    check(store.shutdown().succeeded(),"reader workflow fixture closed");
}

#include "message_list_delete_checks.inc"

int main(int argc,char** argv){try{
    check(argc==2 || argc==3,"new fixture parent and optional real Setup backup required");auto root=std::filesystem::absolute(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());sfx::test::FakeStoragePathPolicy policy;
    if(argc==3 && std::string_view(argv[2])=="--beta13-reader"){
        beta13_reader_workflows(root/"beta13-reader",policy);
        std::cout<<"PASS "<<checks<<" beta.13 SQLite message query and caller workflow checks\n";
        return 0;
    }
    if(argc==3&&std::string_view(argv[2])=="--reader-colors"){reader_color_checks(root/"reader-colors",policy);std::cout<<"PASS "<<checks<<" actual reader color and navigation checks\n";return 0;}
    if(argc==3 && std::string_view(argv[2])=="--list-delete"){
        message_list_delete_checks(root/"list-delete",policy);
        std::cout<<"PASS "<<checks<<" durable message list delete and undelete checks\n";
        return 0;
    }
    if(argc==3&&std::string_view(argv[2])=="--editor-popups"){
        editor_popup_checks(root/"editor-popups",policy);
        std::cout<<"PASS "<<checks<<" editor popup navigation and retained draft checks\n";
        return 0;
    }
    if(argc==3&&std::string_view(argv[2])=="--reader-authority"){individual_delete_checks(root/"reader-authority",policy);maintenance_permanent_and_sent_checks(root/"permanent-authority",policy);std::cout<<"PASS "<<checks<<" focused actual reader Delete/Permanent authority and default behavior checks\n";return 0;}
    if(argc==3&&std::string_view(argv[2])=="--retention-faults"){retention_fault_checks(root/"retention-physical-faults",policy);std::cout<<"PASS isolated retention physical-fault/recovery checks\n";return 0;}
    if(argc==3&&std::string_view(argv[2])=="--beta8-private-defaults"){beta8_private_default_checks(root/"beta8-private-defaults",policy);std::cout<<"PASS "<<checks<<" fresh/private saved-choice transaction checks\n";return 0;}
    if(argc==3&&std::string_view(argv[2])=="--selection"){auto s=options(root/"selection");db::SqliteStorage store(s,policy);check(store.start().succeeded()&&store.shutdown().succeeded(),"selection current fixture");seed(s.data_directory/"SFX.DB");check(store.start().succeeded(),"selection actors ready");selection_checks(store,{id(2),2,1,1});check(store.shutdown().succeeded(),"selection fixture stopped");std::cout<<"PASS isolated caller selection/real Escape idle checks\n";return 0;}
    if(argc==3 && std::string_view(argv[2])=="--source-compose"){
        source_compose_checks(root/"source-compose",policy);std::cout<<"FIXTURE "<<root.string()<<"\nPASS "<<checks<<" source compose checks\n";return 0;
    }
    if(argc==3){
        const auto backup=std::filesystem::absolute(argv[2]);auto settings=options(root/"restore-real-setup-backup");db::SqliteStorage restored(settings,policy);
        check(restored.start().succeeded() && restored.shutdown().succeeded(),"fresh isolated real-backup recovery target");
        check(restored.validate_backup(backup).succeeded() && restored.restore_backup(backup).succeeded(),"real shipping Setup whole-DB backup verifies and restores through production adapter");
        check(restored.start().succeeded() && restored.health().schema_version==31,"real restored shipping database opens with exact lineage");
        const auto file=settings.data_directory/"SFX.DB";
        check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==scalar(backup,"SELECT count(*) FROM sfx_local_messages") && scalar(file,"SELECT count(*) FROM sfx_message_commit_receipts")==scalar(backup,"SELECT count(*) FROM sfx_message_commit_receipts"),"real backup message and receipt counts round-trip");
        check(scalar(file,"SELECT account_id FROM sfx_system_sysop")==1 && scalar(file,"SELECT count(*) FROM sfx_accounts")==5,"real backup trusted assignment and unrelated accounts retained");
        check(restored.shutdown().succeeded(),"real-backup recovery fixture stopped");
        std::cout<<"RESTORED "<<file.string()<<"\nPASS "<<checks<<" real Setup backup adapter checks\n";return 0;
    }
    maintenance_migration_checks(root/"maintenance-migration",policy);maintenance_configuration_checks(root/"maintenance-configuration",policy);
    maintenance_permanent_and_sent_checks(root/"maintenance-permanent-sent",policy);
    retention_count_checks(root/"retention-count",policy);
    retention_protection_and_age_checks(root/"retention-age-permanent",policy);
    retention_private_and_hold_checks(root/"retention-private-hold",policy);
    retention_fault_checks(root/"retention-physical-faults",policy);
    area_deletion_checks(root/"whole-area-deletion",policy);
    compaction_checks(root/"physical-compaction",policy);
    group_deletion_checks(root/"populated-group-deletion",policy);
    migration_faults(root/"migration",policy);administration_migration_faults(root/"admin-migration",policy);lifecycle_checks(root/"lifecycle",policy);concurrency_fault_checks(root/"concurrent",policy);io_fault_checks(root/"write-faults",policy);login_summary_checks(root/"login-summary",policy);administration_checks(root/"administration",policy);auto s=options(root/"current");db::SqliteStorage store(s,policy);
    group_migration_faults(root/"group-migration",policy);group_delete_checks(root/"group-delete",policy);
    individual_migration_checks(root/"individual-migration",policy);individual_delete_checks(root/"individual-delete",policy);individual_write_fault_checks(root/"individual-write-fault",policy);
    preview_notice_format_checks(root/"preview-format",policy);
    beta8_private_default_checks(root/"beta8-private-defaults",policy);
    source_migration_checks(root/"source-migration",policy);source_storage_checks(root/"source-storage",policy);source_fault_checks(root/"source-fault",policy);
    source_authority_checks(root/"source-authority",policy);
    source_compose_checks(root/"source-compose",policy);
    auto start=store.start();if(!start)std::cerr<<start.operation<<'\n';check(static_cast<bool>(start) && store.health().schema_version==31,"fresh current schema starts");check(static_cast<bool>(store.shutdown()),"stop for fixture actors");
    const auto file=s.data_directory/"SFX.DB";seed(file);check(static_cast<bool>(store.start()),"fixture restart");
    app::MessageActor sysop{id(1),1,1,1},alice{id(2),2,1,1},bob{id(3),3,1,1};
    check(scalar(file,"SELECT account_id FROM sfx_system_sysop")==1,"trusted first-run designation");check(store.message_groups(alice).value->size()==1,"Local visible");check(store.local_echoes(alice,{1}).value->size()==2,"starter echoes visible");
    app::MessageTarget chat{app::MessageKind::public_echo,{2},{},{}},notices{app::MessageKind::public_echo,{1},{},{}},mail{app::MessageKind::private_local,{},1,{}};
    check(!store.begin_message(alice,notices),"ordinary caller cannot post Notices");auto r=request(store,alice,chat,10);auto public_post=store.commit_message(r);check(static_cast<bool>(public_post) && public_post.value->area_number==1,"public number starts one");
    check(store.commit_message(r).value->id==public_post.value->id && store.reconcile_message(r).value->id==public_post.value->id,"receipt retry/reconciliation once");
    check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==1 && scalar(file,"SELECT messages_entered FROM sfx_caller_message_statistics WHERE account_id=2")==1,"success counters once");auto bad=r;bad.subject="Different";check(!store.commit_message(bad),"conflicting OperationId refused");
    auto priv=request(store,alice,mail,11);priv.body=std::string("Private ")+static_cast<char>(0xdb)+"\n";auto private_post=store.commit_message(priv);check(static_cast<bool>(private_post),"private commit");
    beta13_preview_checks(store,sysop,alice,bob,public_post.value->id,private_post.value->id);
    check(!store.quote_source(bob,private_post.value->id) && !store.open_message(bob,private_post.value->id),"hidden ID quote/read denied");check(store.quote_source(alice,private_post.value->id).value->body.canonical_lf==priv.body,"CP437 sender copy exact");
    check(!store.quote_source(sysop,private_post.value->id).value->header.recipient_opened,"quote does not read");check(!store.open_message(alice,private_post.value->id).value->header.recipient_opened,"sender open does not read recipient");check(store.open_message(sysop,private_post.value->id).value->header.recipient_opened,"recipient open immediately read");
    check(!store.remove_private_inbox_item(bob,private_post.value->id) && !store.remove_private_inbox_item(alice,public_post.value->id),"no wrong-recipient/public delete");check(static_cast<bool>(store.remove_private_inbox_item(sysop,private_post.value->id)) && static_cast<bool>(store.remove_private_inbox_item(sysop,private_post.value->id)),"recipient removal idempotent");check(static_cast<bool>(store.open_message(alice,private_post.value->id)),"sender copy retained");check(scalar(file,"SELECT count(*) FROM sfx_message_arrival_intents")==1,"one addressed intent no public broadcast");
    auto named=chat;named.recipient=3;auto named_request=request(store,alice,named,12);auto named_post=store.commit_message(named_request);check(static_cast<bool>(named_post),"named public commit");check(!store.open_message(sysop,named_post.value->id).value->header.recipient_opened,"Sysop observer not recipient");check(store.open_message(bob,named_post.value->id).value->header.recipient_opened,"actual named recipient opened");
    auto pref=store.message_scan_preference(bob,{2});check(pref.value->highest_opened==2 && pref.value->unread && pref.value->to_you,"lazy defaults/progress");store.open_message(bob,public_post.value->id);check(store.message_scan_preference(bob,{2}).value->highest_opened==2,"older open cannot rewind");auto off=store.set_message_scan_preference(bob,{2},pref.value->revision,app::MessageScanColumn::unread,false);check(off && !off.value->unread && off.value->to_you && off.value->highest_opened==2,"column CAS preserves other/progress");check(!store.set_message_scan_preference(bob,{2},pref.value->revision,app::MessageScanColumn::to_you,false),"stale preference fails");check(static_cast<bool>(store.open_message(bob,public_post.value->id)),"scan Off not manual permission");
    query_checks(store,file,alice,bob);
    configuration_checks(store,file,alice,bob);
    compose_checks(store,file,alice,bob,public_post.value->id);
    selection_checks(store,alice);
    reader_scan_checks(store,file,alice,bob);
    session_context_checks(store,file,alice,bob);
    auto outcome=store.message_commit_outcome(r);check(outcome && outcome.value->committed && outcome.value->committed->id==public_post.value->id,"explicit committed outcome");
    auto unsaved=r;unsaved.operation=id(200);outcome=store.message_commit_outcome(unsaved);check(outcome && !outcome.value->committed,"confirmed no receipt not storage failure");
    unsaved.actor.session=bob.session;check(!store.message_commit_outcome(unsaved),"unauthorized reconciliation not absence success");
    auto stale=request(store,alice,chat,13);edit(file,"UPDATE sfx_local_echoes SET posting_minimum=999,revision=revision+1 WHERE echo_id=2");check(!store.commit_message(stale),"post restriction during draft rechecked");check(store.reconcile_message(r).value->id==public_post.value->id && store.commit_message(r).value->id==public_post.value->id,"receipt available after post permission loss");auto forged=bob;forged.session=alice.session;check(!store.message_groups(forged),"cross-account session rejected");forged=bob;forged.account_revision=99;check(!store.message_groups(forged),"stale actor rejected");
    edit(file,"UPDATE sfx_message_groups SET access_minimum=999,revision=revision+1 WHERE group_id=1");check(!store.open_message(bob,public_post.value->id) && store.local_echoes(bob,{1}).value->empty(),"group access immediate");check(static_cast<bool>(store.open_message(alice,private_post.value->id)),"mailbox independent of group");bool protected_sysop=false;try{edit(file,"UPDATE sfx_accounts SET account_state='locked',revision=revision+1 WHERE account_id=1");}catch(const internal::StorageFailure&){protected_sysop=true;}check(protected_sysop,"assignment protected across existing SQL path");
    auto backup=root/"messages-v8.db";check(store.create_backup(backup).status.succeeded(),"whole migrated DB backup");
    check(store.validate_backup(backup).succeeded()&&!std::filesystem::exists(backup.string()+"-wal")&&!std::filesystem::exists(backup.string()+"-shm"),"standalone backup validation leaves no WAL/SHM sidecars");
    check(store.shutdown().succeeded(),"stop before restore");check(store.validate_backup(backup).succeeded() && store.restore_backup(backup).succeeded(),"verified restore");check(store.start().succeeded() && store.reconcile_message(priv).value->id==private_post.value->id,"restored receipt/message identity");check(store.message_scan_preference(sysop,{2}).value->highest_opened==2,"restored progress");check(store.shutdown().succeeded(),"new final fixture stopped");
    for(const auto& m:db::sfx_storage_migrations())std::cout<<"MIGRATION "<<m.version<<' '<<m.sql.size()<<' '<<m.content_sha256<<'\n';
    std::cout<<"FIXTURE "<<root.string()<<"\nPASS "<<checks<<" real message storage checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
