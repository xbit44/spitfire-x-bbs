#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sodium.h>
#include "sfx/archive/qwk_packet_builder.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <map>
#include <deque>
#include "sfx/application/qwk_session.hpp"

namespace app=sfx::application;
namespace storage=sfx::adapters::storage;
namespace internal=storage::internal;
namespace {
unsigned checks{};
void check(bool c,const char* why){++checks;if(!c)throw std::runtime_error(why);}
app::Identifier128 id(unsigned n){app::Identifier128 v;v.bytes[0]=0x8d;v.bytes[14]=static_cast<std::uint8_t>(n>>8);v.bytes[15]=static_cast<std::uint8_t>(n);return v;}
void seed(const std::filesystem::path& file){
    internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
    c.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(1,'Sysop','sysop',999,'active',1,1,1700000000,1700000000),(2,'Alice','alice',10,'active',1,1,1700000000,1700000000),(3,'Bob','bob',10,'active',1,1,1700000000,1700000000)","owned synthetic test actors");
    c.exec("INSERT INTO sfx_account_statistics(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_profiles(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_name_reservations SELECT name_key,account_id,1,display_name,1700000000,NULL FROM sfx_accounts;UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash;UPDATE sfx_installation_state SET staged_sysop_account_id=1,setup_state='complete',completed_utc_seconds=1700000000","complete synthetic account fixture");
    for(unsigned n=1;n<=3;++n){internal::SqliteStatement q(c.get(),"INSERT INTO sfx_account_sessions VALUES(?1,?2,?2,1700000000,0,1700000000,NULL,NULL,?1)","trusted test sessions, not client authentication");q.bind(1,id(n));q.bind(2,static_cast<std::int64_t>(n));q.step();}
}
std::int64_t scalar(const std::filesystem::path& file,const char* sql){internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);return c.scalar_int64(sql,"owned fixture invariant");}
#include "qwk_reply_source_fake.inc"
std::map<std::string,std::string> download_members(std::string_view zip){
    auto* reader=archive_read_new();check(reader&&archive_read_support_format_zip(reader)==ARCHIVE_OK&&archive_read_open_memory(reader,zip.data(),zip.size())==ARCHIVE_OK,"actual prepared ZIP opened for component inspection");
    std::map<std::string,std::string> files;archive_entry* entry{};
    for(;;){const auto status=archive_read_next_header(reader,&entry);if(status==ARCHIVE_EOF)break;check(status==ARCHIVE_OK&&entry,"actual complete ZIP member header");std::string bytes;char block[8192];
        for(;;){const auto n=archive_read_data(reader,block,sizeof(block));check(n>=0,"actual prepared ZIP member CRC/data");if(!n)break;bytes.append(block,static_cast<std::size_t>(n));}
        check(files.emplace(archive_entry_pathname(entry),std::move(bytes)).second,"unique actual prepared member");}
    check(archive_read_free(reader)==ARCHIVE_OK,"owned component ZIP reader closed");return files;
}
}
int main(int argc,char** argv){try{
    check(argc==2,"new owned fixture parent required");const auto root=std::filesystem::absolute(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);
    storage::SqliteStorageSettings settings;settings.data_directory=root;settings.utc_now=[]{return std::int64_t{1791920000};};sfx::test::FakeStoragePathPolicy path;
    storage::SqliteStorage db(settings,path);const auto start=db.start();if(!start)std::cerr<<"startup: "<<start.operation<<" code="<<static_cast<unsigned>(start.code)<<'\n';
    check(start.succeeded() && db.health().schema_version==31,"actual production fresh migration through 31");check(db.shutdown().succeeded(),"stop before trusted fixture actors");
    const auto file=root/"SFX.DB";seed(file);check(db.start().succeeded(),"production populated fixture restart");
    app::MessageActor sysop{id(1),1,1,1},alice{id(2),2,1,1},bob{id(3),3,1,1};app::MessageConfigurationActor owner{true,{}};
    auto policy=db.qwk_policy(owner);check(policy && app::valid_qwk_board_id(policy.value->board_id) && policy.value->maximum_messages==10000 && policy.value->word_wrap_default==false,"stable generated identity/count; owner-selected Off default");
    check(!db.qwk_policy({false,alice}) && !db.qwk_policy({true,sysop}),"ordinary/mixed Config authority refused");
    auto prefs=db.qwk_preferences(alice);check(prefs && prefs.value->preferences.word_wrap==false && prefs.value->revision==1 && scalar(file,"SELECT count(*) FROM sfx_qwk_caller_preferences")==1,"first caller inherits and saves explicit owner Off default");
    auto choices=prefs.value->preferences;choices.word_wrap=false;choices.indexes=false;
    auto saved=db.save_qwk_preferences(alice,1,choices);check(saved && saved.value->revision==2 && !saved.value->preferences.indexes && saved.value->preferences.word_wrap==false,"explicit own choices saved");
    check(!db.save_qwk_preferences(alice,1,choices),"stale preference CAS refused");auto bad=choices;bad.encoding=app::QwkEncoding::utf8;bad.full_headers=false;check(!db.save_qwk_preferences(alice,2,bad),"dependent choice refused without changing either");
    auto forged=bob;forged.session=alice.session;check(!db.qwk_preferences(forged) && !db.save_qwk_preferences(forged,2,choices),"cross-caller session denied");
    auto bob_default=db.qwk_preferences(bob);check(bob_default && bob_default.value->preferences.word_wrap==false && bob_default.value->revision==1 && scalar(file,"SELECT count(*) FROM sfx_qwk_caller_preferences")==2,"other caller independently inherits Off without resetting first caller");
    auto bob_on=bob_default.value->preferences;bob_on.word_wrap=true;
    auto bob_explicit=db.save_qwk_preferences(bob,1,bob_on);check(bob_explicit && bob_explicit.value->revision==2 && bob_explicit.value->preferences.word_wrap==true,"caller may explicitly override initial Off with On");
    auto bob_kept=db.qwk_preferences(bob);check(bob_kept && bob_kept.value->revision==2 && bob_kept.value->preferences.word_wrap==true,"reading policy defaults does not reset an existing explicit On choice");
    check(!db.save_qwk_policy({false,alice},id(100),1,"OTHER",1),"ordinary caller policy save denied");
    auto changed=db.save_qwk_policy(owner,id(101),1,"x-bit",1);check(changed && changed.value->board_id=="X-BIT" && changed.value->maximum_messages==1 && changed.value->revision==2,"authorized canonical policy save");
    auto replay=db.save_qwk_policy(owner,id(101),1,"X-BIT",1);check(replay && replay.value->replayed && replay.value->revision==2,"same canonical policy operation reconciles once");
    check(!db.save_qwk_policy(owner,id(101),1,"OTHER",1) && !db.save_qwk_policy(owner,id(102),1,"OTHER",1),"changed operation/stale revision refused");
    check(!db.save_qwk_policy(owner,id(103),2,"NETMAIL",1) && !db.save_qwk_policy(owner,id(104),2,"SFX1",0),"unsafe identity/count refused");
    app::MessageTarget chat{app::MessageKind::public_echo,{2},{},{}},mail{app::MessageKind::private_local,{},3,{}};
    const auto create=[&](const app::MessageTarget& target,unsigned number){auto context=db.begin_message(alice,target);check(static_cast<bool>(context),"ordinary context reused");auto posted=db.commit_message({alice,id(number),*context.value,"Subject","Body\n"});check(static_cast<bool>(posted),"ordinary commit preserved");return posted.value->id;};
    const auto public_message=create(chat,200),private_message=create(mail,201);auto reference=db.qwk_reference(alice,public_message);
    check(reference && *reference.value==1 && db.qwk_reference(bob,public_message).value==reference.value,"stable public reference shared without renumbering");
    check(!db.qwk_reference(alice,private_message) && !db.qwk_reference(sysop,private_message),"no Sent or Sysop override of another Inbox");
    auto private_reference=db.qwk_reference(bob,private_message);check(private_reference && *private_reference.value==2,"intended received-mail owner reference");
    check(scalar(file,"SELECT opened FROM sfx_message_recipient_state WHERE recipient_account_id=3")==0 && scalar(file,"SELECT last_read_sequence FROM sfx_message_counters")==0,"reference allocation has no recipient Read effect");
    check(scalar(file,"SELECT count(*) FROM sfx_caller_echo_state")==0,"reference allocation has no public pointer effect");
    check(db.shutdown().succeeded() && db.start().succeeded(),"new state survives production restart");prefs=db.qwk_preferences(alice);policy=db.qwk_policy(owner);
    check(prefs && prefs.value->revision==2 && !prefs.value->preferences.indexes && prefs.value->preferences.word_wrap==false && policy.value->board_id=="X-BIT" && policy.value->word_wrap_default==false,"saved explicit Off and initial policy preserved after restart");
    bob_kept=db.qwk_preferences(bob);check(bob_kept && bob_kept.value->revision==2 && bob_kept.value->preferences.word_wrap==true,"existing explicit On survives restart and board/count policy save");
    check(db.qwk_reference(alice,public_message).value==reference.value && db.qwk_reference(bob,private_message).value==private_reference.value,"stable references preserved on restart");
    choices.word_wrap=false;auto bob_choices=choices;auto bob_saved=db.save_qwk_preferences(bob,2,bob_choices);check(bob_saved && bob_saved.value->preferences.word_wrap==false && bob_saved.value->revision==3,"caller can explicitly change On back to Off without resetting other preferences");
    auto plan=db.begin_qwk_packet(bob,id(250));check(plan && plan.value->queues.size()==3 && plan.value->catalog.size()==2 && plan.value->next_queue==0,"finite plan has mail plus two accessible default scan queues");
    check(plan.value->arrival_fence==2 && plan.value->settings.preferences.private_mail==app::QwkPrivateMode::unread,"actual finite current arrival capture");
    const auto find_queue=[&](const app::QwkPacketPlan& p,std::int64_t echo){for(std::size_t n=0;n<p.queues.size();++n)if(p.queues[n].echo.value==echo)return n;throw std::runtime_error("expected queue absent");};
    auto mail_peek=db.next_qwk_message(bob,*plan.value,0);auto public_peek=db.next_qwk_message(bob,*plan.value,find_queue(*plan.value,2));
    check(mail_peek && mail_peek.value->has_value() && mail_peek.value->value().header.identity.id==private_message,"received mail only packet peek");
    check(public_peek && public_peek.value->has_value() && public_peek.value->value().header.identity.id==public_message,"oldest public candidate packet peek");
    check(scalar(file,"SELECT opened FROM sfx_message_recipient_state WHERE recipient_account_id=3")==0 && scalar(file,"SELECT count(*) FROM sfx_caller_echo_state")==0 && scalar(file,"SELECT count(*) FROM sfx_qwk_continuation")==0,"preparation/peek has no Read/progress/continuation effects");
    check(!db.next_qwk_message(alice,*plan.value,0) && !db.next_qwk_message(bob,*plan.value,1000),"cross-owner plan/invalid queue refused");
    auto alice_plan=db.begin_qwk_packet(alice,id(251));check(static_cast<bool>(alice_plan),"author packet plan");auto sent_peek=db.next_qwk_message(alice,*alice_plan.value,0);auto own_peek=db.next_qwk_message(alice,*alice_plan.value,find_queue(*alice_plan.value,2));
    check(sent_peek && !sent_peek.value->has_value() && own_peek && !own_peek.value->has_value(),"no Sent export and own posts default No");
    const auto later=create(chat,252);auto advance=*plan.value;advance.queues[find_queue(advance,2)].after_arrival=public_peek.value->value().header.identity.arrival_ordinal;
    auto fenced=db.next_qwk_message(bob,advance,find_queue(advance,2));check(fenced && !fenced.value->has_value(),"later arrival does not enter captured batch");
    check(db.open_message(bob,public_message).succeeded(),"ordinary online reading remains available during packet prep");
    auto skipped=db.next_qwk_message(bob,*plan.value,find_queue(*plan.value,2));check(skipped && !skipped.value->has_value(),"online public progress reconciles without stale regression");
    auto new_plan=db.begin_qwk_packet(bob,id(253));check(static_cast<bool>(new_plan),"later finite packet plan");auto next_public=db.next_qwk_message(bob,*new_plan.value,find_queue(*new_plan.value,2));
    check(new_plan && new_plan.value->arrival_fence==3 && next_public && next_public.value->value().header.identity.id==later,"new batch sees later arrivals beyond updated online boundary");
    bob_choices.private_mail=app::QwkPrivateMode::all_received;check(db.save_qwk_preferences(bob,bob_saved.value->revision,bob_choices).succeeded(),"All received explicit mode persists");
    check(!db.next_qwk_message(bob,*plan.value,0),"settings mutation invalidates stale plan before output");
    check(db.open_message(bob,private_message).succeeded(),"ordinary recipient open");
    auto all_plan=db.begin_qwk_packet(bob,id(254));auto read_mail=db.next_qwk_message(bob,*all_plan.value,0);check(read_mail && read_mail.value->has_value(),"All received includes already-read own Inbox");
    auto scan=db.message_scan_preference(bob,{2});check(static_cast<bool>(scan),"read shared public scan choice");
    check(db.set_message_scan_preference(bob,{2},scan.value->revision,app::MessageScanColumn::unread,false).succeeded(),"existing online scan mutation");
    check(!db.next_qwk_message(bob,*all_plan.value,0),"any shared selection change invalidates packet plan");
#include "qwk_delivery_checks.inc"
#include "qwk_reply_checks.inc"
#include "qwk_builder_checks.inc"
#include "qwk_session_checks.inc"
#include "qwk_routing_checks.inc"
    check(db.shutdown().succeeded(),"foundation writer stopped");
    std::cout<<"FIXTURE "<<root.string()<<"\nPASS "<<checks<<" production QWK foundation checks; synthetic actors, not client/Config/packet integration\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL QWK storage after "<<checks<<": "<<e.what()<<'\n';return 1;}}
