#include "../support/include/sfx/test/linux_layout_fixture.hpp"
// Actual private Linux/SQLite custody and native expiry. Session provenance is
// a synthetic host port; no hub, installed Runtime or physical-platform claim.
#define main retained_expiry_file_fixture_main
#include "../integration/file_storage_tests.cpp"
#undef main
#include "sfx/platform/linux/ftn_native_toss.hpp"
#include "sfx/platform/linux/ftn_processed_expiry.hpp"
#include "sfx/platform/linux/ftn_secrets.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "sfx/test/sqlite_write_fault_vfs.hpp"
#include "sqlite_internal.hpp"
#include <sodium.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <map>
namespace native=sfx::platform::linux_host;
#include "linux_ftn_retention_faults.inc"
#ifdef SFX_FTN_EXPIRY_CLOSE_QUALIFICATION
namespace sfx::platform::linux_host {void qualification_expiry_protect_next_close();}
#endif
namespace {
app::FtnDigest expiry_hash(std::string_view v){app::FtnDigest out{};check(sodium_init()>=0&&crypto_hash_sha256(out.data(),reinterpret_cast<const unsigned char*>(v.data()),v.size())==0,"actual fixture SHA256");return out;}
app::FtnPacket expiry_packet(unsigned n){app::FtnPacket p;p.header.origin=*app::parse_ftn_address("2:100/2@test");p.header.destination=*app::parse_ftn_address("2:100/1@test");p.header.year=2026;p.header.month=10;p.header.day=29;std::copy_n("packkey",7,p.header.password.begin());app::FtnPackedMessage m;m.origin_net=m.destination_net=100;m.origin_node=2;m.destination_node=1;m.date="29 Oct 26  12:00:00";m.from="Remote Person";m.to="All";m.subject="Retained original";m.text="AREA:TEST.ECHO\r\1MSGID: 2:100/2 "+std::to_string(n)+"\r\1CHRS: CP437 2\rDurable imported content.\r";p.messages.push_back(m);return p;}
}
int main(int argc,char** argv){try{
    const bool external_mode=argc==3&&std::string_view(argv[2])=="external";check(argc==2||external_mode||(argc==3&&std::string_view(argv[2])=="--no-birth"),"fresh native fixture and optional no-birth mode required");expiry_without_birth=argc==3&&!external_mode;const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve previous fixtures");std::filesystem::create_directories(root.parent_path());check(::mkdir(root.c_str(),0700)==0,"private native toss fixture");
    for(const auto* role:{"config","data","ftn","ftn/inbound","ftn/outbound","ftn/bad","ftn/processed","ftn/nonsecure","ftn/inbound/test","ftn/outbound/test","ftn/bad/test","ftn/processed/test"})check(::mkdir((root/role).c_str(),0700)==0,"private FTN fixture role");
    {auto config=native::open_owned_directory(root/"config");const std::string intent="SFX_LINUX_FIRST_RUN=1\n";native::publish_private_file(config.get(),"first-run.v1",{intent.begin(),intent.end()},false);}
    check(bool((sfx::test::mark_provisioned_linux_fixture(root),native::initialize_fresh_portable_board_key(root))),"portable toss fixture board key");
    auto bindings=native::LinuxFilePayloadBindings::initialize_fresh(root);
    native::FtnInstallation installation{root,bindings};

    check(sfx::test::install_sqlite_write_fault_vfs(root.filename().u8string()),"install exact-fixture delegating VFS");
    sfx::test::FakeStoragePathPolicy paths;auto s=settings(root/L"data");s.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;std::int64_t now=1800000000;s.utc_now=[&]{return now;};s.checkpoint.passive_interval=std::chrono::hours(1);const auto database=s.data_directory/L"sfx.db";
    storage::SqliteStorage db(s,paths);check(bool(db.start())&&bool(db.shutdown()),"initialize current schema");seed(database);check(bool(db.start()),"owner fixture starts");const app::MessageConfigurationActor owner{true,{}};
    auto c=*db.ftn_configuration(owner).value;if(external_mode)c.settings.mode=app::FtnMailerMode::external_binkd;app::FtnNetwork n;n.code="TEST";n.name="Native Retention";n.address="2:100/1@test";n.enabled=true;n.keep_processed=false;c.networks.push_back(n);app::FtnLink l;l.network="TEST";l.code="PEER";l.name="Synthetic Session";l.address="2:100/2@test";l.host="127.0.0.1";l.enabled=true;l.credentials.session=*native::protect_ftn_secret(root.u8string(),n.code,l.code,native::FtnCredentialKind::session,"session-key");l.credentials.packet=*native::protect_ftn_secret(root.u8string(),n.code,l.code,native::FtnCredentialKind::packet,"packkey");c.links.push_back(l);app::FtnMessageEcho echo;echo.network="TEST";echo.tag="TEST.ECHO";echo.description="Retention fixture";c.message_echoes.push_back(echo);c.permissions.push_back({"TEST","PEER","TEST.ECHO",false,true,false,false});unsigned operation=100000;check(bool(db.change_ftn_configuration(owner,id(operation++).bytes,c)),"new Off policy applies only to future successful originals");c=*db.ftn_configuration(owner).value;
    auto incoming=native::FilePayloadDirectory::private_store(root/L"ftn/inbound/test",bindings);auto processed=native::FilePayloadDirectory::private_store(root/L"ftn/processed/test",bindings);const auto live=[]{return true;};
    std::map<std::array<std::uint8_t,16>,std::string> processed_paths;
    const auto processed_name=[&](const app::FtnRetainedOriginal& original){return processed_paths.at(original.ingress.bytes);};
    const auto candidate=[&](app::Identifier128 wanted){app::Identifier128 after;for(;;){const auto page=db.ftn_retention_candidates(owner,after,4);check(bool(page),"bounded retention cursor");for(const auto& r:*page.value)if(r.ingress==wanted)return r;if(page.value->size()<4)throw std::runtime_error("retained candidate absent");after=page.value->back().ingress;}};
    const auto produce=[&](unsigned number){
        c=*db.ftn_configuration(owner).value;const auto bytes=*app::encode_ftn_packet(expiry_packet(number));app::FtnIngressReceipt r;r.id=id(number);r.session=id(number+10000);r.identity=expiry_hash("expiry-"+std::to_string(number));r.network="TEST";r.link="PEER";r.proof=external_mode?app::FtnIngressProof::external_packet:app::FtnIngressProof::inbound_cram;r.configuration_revision=c.revision;r.file={"mail-"+std::to_string(number)+".pkt",bytes.size(),1700000000+number};
        // Retention cases test genuinely absent reconciliation. Another original
        // at the same friendly name correctly yields changed_held instead.
        {auto receiver=native::FtnNativeReceive::open(incoming,db,r,live);receiver.append(bytes);receiver.finish();r=receiver.receipt();}
        const auto tossed=native::toss_ftn_native_receipt(db,c,installation,r,live);if(!tossed.processed)std::cerr<<tossed.reason<<'\n';check(tossed.processed&&tossed.imported==1,"actual successful native import to Processed");const auto retained=candidate(r.id);
        // Observe the actual current friendly/collision name by native proof;
        // the older fixture's unconditional legacy .dat assumption was stale.
        for(const auto& name:processed.names())try{const auto held=processed.read(name);const auto object=held.object_identity();if(object.volume==retained.native.volume&&object.file_id==retained.native.file_id){processed_paths[retained.ingress.bytes]=name;return retained;}}catch(...){ }
        throw std::runtime_error("fixture could not locate its just-processed native original");
    };
    const auto prepare=[&](const app::FtnRetainedOriginal& original){app::FtnExpiryIntent request;request.operation=id(operation++);request.dispatch=id(operation++);request.original=original;const auto saved=db.prepare_ftn_expiry(owner,request);if(!saved)std::cerr<<"prepare: "<<saved.status.operation<<'\n';check(saved&&saved.value->prepared_utc==now,"exact intent durably committed after full exclusive verification");return *saved.value;};
    auto cancellation=std::make_shared<std::atomic_bool>(false);
    const auto execute=[&](const app::MessageConfigurationActor& actor,const std::shared_ptr<app::FtnRetentionRemoval>& capability){
        const auto result=db.execute_ftn_expiry(actor,capability);
        if(!result)static_cast<void>(native::FtnProcessedExpiry::restore_after_dispatch(capability));
        return result;
    };
    auto original=produce(1);const auto imports=scalar(database,"SELECT count(*) FROM sfx_local_messages");
    bindings->refresh();check(bindings->deferred_records()==0,"retained original recorded before actual expiry");
    {auto reader=processed.read(processed_name(original));bool held=false;try{auto probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);}catch(const std::exception&){held=true;}check(held&&reader.identity().sha256==original.native.sha256,"competing ordinary reader prevents exclusive expiry without mutation");}
    auto probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);check(probe->observation()==native::FtnProcessedExpiry::Observation::exact,"fresh exclusive full identity/hash verified");
    {native::FileDescriptor other(open((processed.path()/processed_name(original)).c_str(),O_RDONLY|O_CLOEXEC));check(other&&flock(other.get(),LOCK_SH|LOCK_NB)<0,"exclusive lease refuses a cooperating later reader");}

    auto intent=prepare(original);auto removal=probe->seal(intent);check(bool(execute(owner,removal))&&removal->result().state==app::FtnExpiryNativeState::closed,"single dedicated transaction invokes private capture disposition and checked close");check(probe->record_confirmed_removal(removal),"confirmed host outcome retires logical proof outside commit-critical callback");check(processed.absent(processed_name(original)),"exact original absent after confirmed native close");check(!execute(owner,removal),"same sealed dispatch cannot invoke twice");check(scalar(database,"SELECT count(*) FROM sfx_ftn_expiry_intents WHERE outcome=1")==1&&scalar(database,"SELECT count(*) FROM sfx_local_messages")==imports&&scalar(database,"SELECT count(*) FROM sfx_ftn_import_receipts")==1,"confirmed expiry retains imported content and immutable provenance");removal.reset();probe.reset();
    bindings->refresh();check(bindings->deferred_records()==0,"confirmed successful expiry retires portable binding");

    original=produce(2);probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);intent=prepare(original);removal=probe->seal(intent);*cancellation=true;check(!execute(owner,removal)&&removal->result().state==app::FtnExpiryNativeState::not_invoked,"cancel before queued dispatch never calls native mutation");removal.reset();probe.reset();*cancellation=false;check(!processed.absent(processed_name(original)),"cancel retains exact original");
    auto pending=db.pending_ftn_expiry(owner,original.ingress);check(pending&&pending.value->has_value(),"cancelled committed intent available for reconciliation");probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);check(probe->observation()==native::FtnProcessedExpiry::Observation::exact&&bool(db.reconcile_ftn_expiry(owner,**pending.value,app::FtnExpiryOutcome::present_reconciled)),"present original resolves old dispatch without replay or fabricated deletion");intent=prepare(original);removal=probe->seal(intent);
    c=*db.ftn_configuration(owner).value;c.networks.front().keep_processed=true;c.networks.front().retention_days=0;const auto token=id(operation++).bytes;check(bool(db.preview_ftn_retention(owner,token,c))&&bool(db.change_ftn_configuration(owner,token,c)),"earlier durable Forever Save wins against queued shorter expiry");check(!execute(owner,removal)&&removal->result().state==app::FtnExpiryNativeState::not_invoked,"writer rechecks persisted lengthening before mutation");removal.reset();probe.reset();check(!processed.absent(processed_name(original)),"later protection preserved");
    c=*db.ftn_configuration(owner).value;c.networks.front().keep_processed=false;check(bool(db.change_ftn_configuration(owner,id(operation++).bytes,c)),"new Off leaves previous Forever protection intact");
    original=produce(3);probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);intent=prepare(original);removal=probe->seal(intent);sfx::test::inject_next_sqlite_wal_write_fault(SQLITE_IOERR_WRITE);const auto uncertain=execute(owner,removal);check(!uncertain&&sfx::test::pending_sqlite_write_fault()==0&&removal->result().state==app::FtnExpiryNativeState::closed,"post-native WAL failure never reruns the already-closed dispatch");check(processed.absent(processed_name(original)),"SQL rollback cannot undo native removal");removal.reset();probe.reset();pending=db.pending_ftn_expiry(owner,original.ingress);check(pending&&pending.value->has_value(),"earlier committed exact intent survives failed completion commit");probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);check(probe->observation()==native::FtnProcessedExpiry::Observation::missing&&bool(db.reconcile_ftn_expiry(owner,**pending.value,app::FtnExpiryOutcome::absent_uncertain)),"missing after uncertain expiry recorded without fabricated confirmed deletion");check(scalar(database,"SELECT count(*) FROM sfx_ftn_expiry_intents WHERE outcome=2")==1&&scalar(database,"SELECT count(*) FROM sfx_ftn_expiry_intents WHERE outcome=1")==1,"confirmed and uncertain native outcomes remain distinct");
    original=produce(5);probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);intent=prepare(original);probe.reset();check(bool(db.shutdown())&&bool(db.start()),"restart after durable intent before native invocation");pending=db.pending_ftn_expiry(owner,original.ingress);check(pending&&pending.value->has_value(),"pending intent survives actual reopen");
    const auto original_name=processed_name(original);auto bad=native::FilePayloadDirectory::private_store(root/L"ftn/bad/test",bindings);std::vector<std::uint8_t> original_bytes;
    {auto retained=processed.read(original_name,true);original_bytes=retained.read(0,65536);retained.rename_to(bad,original_name);}
    app::FilePayloadIdentity replacement_identity;{auto replacement=processed.create(original_name,original.native.bytes);replacement.append(original_bytes.data(),original_bytes.size());replacement_identity=replacement.identity();}
    check(replacement_identity.sha256==original.native.sha256&&replacement_identity.file_id!=original.native.file_id,"same-name same-bytes replacement is a distinct native object");probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);check(probe->observation()==native::FtnProcessedExpiry::Observation::changed&&bool(db.reconcile_ftn_expiry(owner,**pending.value,app::FtnExpiryOutcome::changed_held)),"uncertain original replacement stays held, never adopted");probe.reset();check(processed.read(original_name).identity().file_id==replacement_identity.file_id&&bad.read(original_name).identity().file_id==original.native.file_id,"replacement and retained displaced fixture untouched");
    #include "linux_ftn_retention_writer_checks.inc"
    #include "linux_ftn_retention_processor_checks.inc"
    #include "linux_ftn_retention_reference_checks.inc"
    #include "linux_ftn_retention_capsule_checks.inc"
    check(bool(db.shutdown()),"native expiry store stopped");sfx::test::uninstall_sqlite_write_fault_vfs();std::cout<<"PASS "<<checks<<" native retention exclusive custody / singleton / policy / cancellation / uncertain commit / Linux capsule / replacement / writer checks; no installed or hub claim\n";return 0;
}catch(const std::exception& e){sfx::test::uninstall_sqlite_write_fault_vfs();std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
