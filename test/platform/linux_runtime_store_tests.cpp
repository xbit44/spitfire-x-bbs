#include "linux_runtime_store.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace host=sfx::platform::linux_host;
namespace fs=std::filesystem;
using Code=app::LocalControlResponseCode;
using State=app::AlphaRuntimePresentationState;
unsigned checks{};
void require(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}
struct Clock final:app::IClock {
    std::int64_t unix_seconds() const noexcept override {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    std::uint64_t monotonic_millis() const noexcept override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
};
struct Revocations final:app::ISessionRevocationSink {
    unsigned system_changes{};
    bool revoke(const app::SessionRevocationCommand& command) override {
        // This fixture owns no caller services. Verify the bootstrap revocation
        // request rather than rejecting a successfully committed first secret.
        if (command.kind!=app::SessionRevocationKind::sysop_level_all) return false;
        ++system_changes;
        return true;
    }
};
void directory(const fs::path& path) {
    require(fs::create_directory(path),"exclusive fixture directory");
    require(chmod(path.c_str(),0700)==0,"private fixture directory");
}
void write(const fs::path& path,const std::string& text) {
    std::ofstream file(path,std::ios::binary);
    file<<text;file.close();
    require(bool(file) && chmod(path.c_str(),0600)==0,"write private fixture text");
}
std::string read(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),{}};
}
app::AlphaBootstrapRequest request() {
    app::AlphaBootstrapRequest value;
    value.configuration.bbs_name="Native bootstrap fixture";
    value.configuration.bbs_time_zone="Etc/UTC";
    value.configuration.telnet.bind_address="127.0.0.1";
    value.configuration.ssh.bind_address="127.0.0.1";
    value.sysop_name="Fixture Sysop";
    value.sysop_password="Native harness first setup 83";
    value.system_password="Native harness system key 71";
    return value;
}
void run(const fs::path& root,const fs::path& tzdata) {
    require(geteuid()!=0,"normal account");
    require(host::LinuxBbsTimeZoneRuleProvider::initialize(tzdata),"private pinned time zone data");
    directory(root);
    Clock clock;Revocations revocations;
    const auto board=root/"board";directory(board);
    directory(board/"management");
    write(board/"management/tls-placeholder","management-only fixture\n");
    rt::LinuxRuntimeStore store(board,revocations,clock,[]{return false;});
    require(store.open().code==Code::setup_required,"fresh management requires setup");
    require(store.state()==State::setup_required,"truthful setup state");
    rt::LinuxRuntimeStore competitor(board,revocations,clock,[]{return false;});
    require(competitor.open().code==Code::recovery_required,"second control host refused");
    require(!competitor.storage(),"second owner never opens database");
    require(competitor.close_for_maintenance().code==Code::ok,"failed contender releases only own resources");
    require(store.close_for_maintenance().code==Code::ok,"close empty management before setup");
    require(!store.storage() && !store.authentication(),"maintenance really destroys database and authentication ownership");
    require(store.open().code==Code::setup_required,"fresh management may close and reopen without false recovery");

    auto setup=request();
    setup.sysop_name="";
    require(store.bootstrap(setup).code==Code::invalid_request,"invalid initial name refused");
    require(!store.storage()->first_run_state().value->staged_sysop_account_id,"invalid request did not stage Sysop");
    setup=request();setup.configuration.bbs_time_zone="Invalid/Zone";
    require(store.bootstrap(setup).code==Code::configuration_failure,"unknown time zone refused");
    setup=request();
    const auto bootstrapped=store.bootstrap(setup);
    if (bootstrapped.code!=Code::ok) std::cerr<<"Bootstrap diagnostic: "<<bootstrapped.diagnostic_code<<'\n';
    require(bootstrapped.code==Code::ok,"shared shipping setup transaction completes");
    require(revocations.system_changes==1,"System Password change requested actual revocation boundary");
    require(store.diagnostic()=="ready_bbs_stopped","bootstrap does not imply caller services online");
    require(store.file_bindings()!=nullptr,"first-run owns authenticated file bindings");
    require(store.prepare_copied_file_bindings().code==Code::invalid_request,"normal ready board cannot invoke recovery Prepare");
    for(const auto* relative:{"work","ftn","files","files/general","files/uploads","files/uploadtosysop","data/fileintake","data/fileremoved"}){
        struct stat status{};
        require(lstat((board/relative).c_str(),&status)==0&&S_ISDIR(status.st_mode)&&status.st_uid==geteuid()&&(status.st_mode&0077)==0,"first-run creates private native file roots");
    }
    directory(board/"files/Public");
    app::FilePayloadIdentity payload_identity;
    {auto files=host::FilePayloadDirectory::private_store(board/"files/Public",store.file_bindings());auto file=files.create("retained.txt",256);const std::string body="retained Runtime payload";file.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());payload_identity=file.identity();}
    const auto truth=store.storage()->first_run_state();
    require(truth && truth.value && truth.value->complete && truth.value->staged_sysop_account_id,"durable first run complete");
    const auto account=*truth.value->staged_sysop_account_id;
    const auto system=store.storage()->system_password();
    require(system && system.value,"real System Password credential");
    const auto configuration=store.configuration();
    require(configuration && configuration->security_policy_revision>0,"published policy binding");
    app::AlphaConfigurationUpdateRequest update;
    update.expected_revision=configuration->configuration_revision;update.configuration=*configuration;
    ++update.configuration.configuration_revision;update.configuration.bbs_name="Updated native fixture";
    auto forged=update;++forged.configuration.security_policy_revision;
    require(store.save_stopped_configuration(forged).code==Code::invalid_request,"settings cannot forge security policy binding");
    forged=update;forged.configuration.ownership_mode=app::RuntimeOwnershipMode::service;
    require(store.save_stopped_configuration(forged).code==Code::invalid_request,"ordinary settings cannot change Runtime ownership");
    forged=update;forged.configuration.bbs_time_zone="Unknown/Zone";
    require(store.save_stopped_configuration(forged).code==Code::invalid_request,"saved settings require available time zone");
    const auto saved=store.save_stopped_configuration(update);
    require(saved.code==Code::ok && saved.activation==app::AlphaConfigurationActivation::saved_next_start &&
        saved.saved_revision==update.configuration.configuration_revision,"typed stopped save is durable and does not claim running activation");
    require(store.save_stopped_configuration(update).diagnostic_code=="configuration_revision_conflict","stale editor save refused");
    const auto text=read(board/"config/sfx.conf");
    require(!text.empty() && text.find(setup.sysop_password)==std::string::npos &&
        text.find(setup.system_password)==std::string::npos,"configuration contains no setup credentials");
    require(store.bootstrap(setup).diagnostic_code=="bootstrap_already_complete","repeat setup is idempotent");
    require(store.close_for_maintenance().code==Code::ok,"completed board closes for maintenance");
    require(competitor.open().code==Code::ok,"another host acquires actual released lock and database");
    require(competitor.storage()->first_run_state().value->staged_sysop_account_id==account,"restart preserves Sysop identity");
    require(competitor.configuration()->bbs_name==update.configuration.bbs_name &&
        competitor.configuration()->configuration_revision==update.configuration.configuration_revision,
        "restart loads exact new saved settings revision");
    require(competitor.storage()->system_password().value->credential.verifier==system.value->credential.verifier &&
        competitor.storage()->system_password().value->credential.salt==system.value->credential.salt,
        "restart preserves credential bytes");
    require(competitor.close_for_maintenance().code==Code::ok,"second host closes cleanly");

    const auto moved=root/"moved-board";
    fs::rename(board,moved);
    rt::LinuxRuntimeStore relocated(moved,revocations,clock,[]{return false;});
    require(relocated.open().diagnostic_code=="linux_file_map_prepare_required","whole folder relocation requests explicit verified Prepare");
    const auto moved_map=read(moved/"config/file-identities-linux.v1");
    require(relocated.prepare_copied_file_bindings([]{return true;}).code==Code::recovery_required,"cancelled Prepare stays in recovery");
    require(read(moved/"config/file-identities-linux.v1")==moved_map&&relocated.state()==State::recovery_required,"cancel preserves exact map and stopped recovery state");
    require(relocated.prepare_copied_file_bindings().code==Code::ok,"explicit verified Prepare releases moved board");
    {auto files=host::FilePayloadDirectory::private_store(moved/"files/Public",relocated.file_bindings());auto file=files.read("retained.txt");const auto id=file.identity();require(id.volume==payload_identity.volume&&id.file_id==payload_identity.file_id&&id.sha256==payload_identity.sha256,"Runtime Prepare preserves logical file ID and content");}
    require(relocated.storage()->first_run_state().value->staged_sysop_account_id==account,"relocation retains identity");
    require(relocated.close_for_maintenance().code==Code::ok,"relocated host closes");
    const auto missing_payload=root/"copied-missing-payload";fs::copy(moved,missing_payload,fs::copy_options::recursive);fs::remove(missing_payload/"files/Public/retained.txt");
    rt::LinuxRuntimeStore retry(missing_payload,revocations,clock,[]{return false;});
    require(retry.open().diagnostic_code=="linux_file_map_prepare_required","copied missing payload enters explicit Prepare");
    require(retry.prepare_copied_file_bindings().code==Code::ok&&retry.diagnostic()=="ready_bbs_stopped_file_attention"&&retry.file_bindings()->deferred_records()==1,"missing payload preserves evidence and reports attention without dead-ending board");
    require(!fs::exists(missing_payload/"files/Public/retained.txt"),"Prepare never recreates missing payload bytes");
    require(retry.close_for_maintenance().code==Code::ok&&retry.open().code==Code::ok&&retry.diagnostic()=="ready_bbs_stopped_file_attention","attention state reopens without repeated recovery prompt");
    require(retry.prepare_copied_file_bindings([]{return true;}).code==Code::invalid_request&&retry.state()==State::ready&&retry.file_bindings()->deferred_records()==1,"cancelled stopped Retry preserves ready attention state and binding owner");
    write(missing_payload/"files/Public/retained.txt","retained Runtime payload");
    require(retry.prepare_copied_file_bindings().code==Code::ok&&retry.diagnostic()=="ready_bbs_stopped"&&retry.file_bindings()->deferred_records()==0,"explicit stopped Retry resolves restored deferred content");
    require(retry.close_for_maintenance().code==Code::ok,"retried copied board closes");
    fs::rename(moved/"config/file-identities-linux.v1",moved/"config/file-identities-linux.v1.retained");
    require(relocated.open().code==Code::recovery_required,"completed board with missing file map enters recovery");
    require(!fs::exists(moved/"config/file-identities-linux.v1")&&!relocated.file_bindings(),"missing completed file map is never regenerated");
    require(relocated.prepare_copied_file_bindings().code==Code::invalid_request,"Prepare cannot invent missing file bindings");
    require(relocated.close_for_maintenance().code==Code::ok,"missing map recovery releases owner");
    fs::rename(moved/"config/file-identities-linux.v1.retained",moved/"config/file-identities-linux.v1");
    fs::rename(moved/"config/board.key",moved/"config/board.key.retained");
    require(relocated.open().code==Code::recovery_required,"completed board with missing portable key enters recovery");
    require(!fs::exists(moved/"config/board.key")&&relocated.storage()->first_run_state().value->complete,
        "missing key never regenerates or resets completed installation");
    require(relocated.close_for_maintenance().code==Code::ok,"missing-key recovery releases ownership");
    fs::rename(moved/"config/board.key.retained",moved/"config/board.key");
    fs::rename(moved/"config/portable-credentials.v1",moved/"config/portable-credentials.v1.retained");
    require(relocated.open().code==Code::recovery_required,"completed board with missing key marker enters recovery");
    require(!fs::exists(moved/"config/portable-credentials.v1"),"persistent first-run intent cannot repair an established missing marker");
    require(relocated.close_for_maintenance().code==Code::ok,"missing-marker recovery releases ownership");
    fs::rename(moved/"config/portable-credentials.v1.retained",moved/"config/portable-credentials.v1");
    require(relocated.open().code==Code::ok,"restored exact key and marker permit ordinary reopen");
    require(relocated.close_for_maintenance().code==Code::ok,"repaired board closes normally");
    fs::rename(moved/"config/sfx.conf",moved/"config/SFX.conf.retained");
    require(relocated.open().code==Code::recovery_required,"lost configuration never resets completed board");
    require(!fs::exists(moved/"config/sfx.conf"),"missing configuration not regenerated from defaults");
    require(read(moved/"config/SFX.conf.retained")==text,"lost-config evidence preserved");
    require(relocated.close_for_maintenance().code==Code::ok,"recovery ownership releases for repair");
    fs::rename(moved/"config/SFX.conf.retained",moved/"config/sfx.conf");
    require(relocated.open().code==Code::ok,"explicit exact-file restoration reopens board");
    require(relocated.close_for_maintenance().code==Code::ok,"restored board closes");
    fs::rename(moved/"data/sfx.db",moved/"data/SFX.DB.retained");
    require(relocated.open().code==Code::recovery_required,"missing database refused");
    require(!fs::exists(moved/"data/sfx.db"),"startup never recreates lost existing database");
    require(relocated.close_for_maintenance().code==Code::ok,"missing database recovery closes");

    const auto partial=root/"interrupted";directory(partial);
    rt::LinuxRuntimeStore interrupted(partial,revocations,clock,[]{return false;});
    require(interrupted.open().code==Code::setup_required,"new interrupted fixture");
    auto bad_system=request();bad_system.system_password="";
    require(interrupted.bootstrap(bad_system).code==Code::invalid_request,"invalid System Password refused after Sysop stage");
    auto staged=interrupted.storage()->first_run_state();
    require(staged && staged.value && !staged.value->complete && staged.value->staged_sysop_account_id,
        "staged Sysop retained without false completion");
    require(interrupted.close_for_maintenance().code==Code::ok,"partial setup closes without losing staged identity");
    require(interrupted.open().code==Code::setup_required,"restart resumes verified incomplete first run");
    auto wrong_resume=request();wrong_resume.sysop_password="Wrong harness secret 42";
    require(interrupted.bootstrap(wrong_resume).diagnostic_code=="staged_sysop_password_rejected",
        "resume requires original staged Sysop credential");
    auto proper_resume=request();
    require(interrupted.bootstrap(proper_resume).code==Code::ok,"explicit correct setup retry after restart succeeds");
    require(interrupted.storage()->first_run_state().value->staged_sysop_account_id==staged.value->staged_sysop_account_id,
        "retry did not replace staged identity");
    require(interrupted.close_for_maintenance().code==Code::ok,"resumed setup closes");

    const auto failed=root/"publication-failure";directory(failed);
    rt::LinuxRuntimeStore failed_store(failed,revocations,clock,[]{return false;});
    require(failed_store.open().code==Code::setup_required,"publication failure fixture starts");
    write(failed/"config/sfx.conf","retained unexpected file\n");
    auto candidate=request();
    require(failed_store.bootstrap(candidate).code==Code::configuration_failure,"publication cannot overwrite competing configuration");
    require(!failed_store.storage()->first_run_state().value->complete,"publication failure never claims first-run complete");
    require(read(failed/"config/sfx.conf")=="retained unexpected file\n","competing configuration preserved exactly");
    require(failed_store.state()==State::recovery_required,"publication failure is recovery state");
    require(failed_store.close_for_maintenance().code==Code::ok,"publication failure closes for recovery");
    require(failed_store.open().code==Code::recovery_required,"restart preserves invalid-state refusal");
    require(read(failed/"config/sfx.conf")=="retained unexpected file\n","restart does not replace bad config");

    const auto missing_seed=root/"missing-seed-verifier";directory(missing_seed);
    rt::LinuxRuntimeStore unbound(missing_seed,revocations,clock,{});
    require(unbound.open().code==Code::setup_required,"missing callback fixture starts");
    candidate=request();
    require(unbound.bootstrap(candidate).diagnostic_code=="bootstrap_ports_unavailable","unwired package verifier refuses setup");
    require(!unbound.storage()->first_run_state().value->staged_sysop_account_id,"unwired setup made no identity mutation");

    const auto interrupted_publish=root/"published-not-complete";directory(interrupted_publish);
    bool verify_seeds=false;
    rt::LinuxRuntimeStore staged_publish(interrupted_publish,revocations,clock,[&] {
        if (!verify_seeds) throw std::runtime_error("fixture_seed_verification_failure");
        return false;
    });
    require(staged_publish.open().code==Code::setup_required,"published-but-incomplete fixture starts");
    candidate=request();
    require(staged_publish.bootstrap(candidate).code==Code::recovery_required,"seed verification failure refuses completion");
    require(fs::exists(interrupted_publish/"config/sfx.conf") &&
        !staged_publish.storage()->first_run_state().value->complete,"published config alone is not completed setup");
    const auto published=read(interrupted_publish/"config/sfx.conf");
    require(staged_publish.close_for_maintenance().code==Code::ok,"published-but-incomplete setup closes");
    require(staged_publish.open().code==Code::setup_required,"published incomplete setup resumes after restart");
    auto changed=request();changed.configuration.bbs_name="Different setup";
    require(staged_publish.bootstrap(changed).diagnostic_code=="published_configuration_does_not_match_resume",
        "resume cannot silently change published configuration");
    changed=request();changed.system_password="Wrong existing system credential";
    require(staged_publish.bootstrap(changed).diagnostic_code=="system_password_resume_rejected",
        "published setup still proves existing System Password");
    verify_seeds=true;candidate=request();
    require(staged_publish.bootstrap(candidate).code==Code::ok,"explicit verified published-setup resume completes");
    require(read(interrupted_publish/"config/sfx.conf")==published,"resume preserves exact published configuration");

    const auto unknown=root/"unknown-state";directory(unknown);directory(unknown/"data");
    write(unknown/"data/owner.txt","retained owner state\n");
    rt::LinuxRuntimeStore unknown_store(unknown,revocations,clock,[]{return false;});
    require(unknown_store.open().code==Code::recovery_required,"unknown persistent data is not fresh setup");
    require(!unknown_store.storage() && !fs::exists(unknown/"data/sfx.db"),"unknown state never creates replacement database");
    require(read(unknown/"data/owner.txt")=="retained owner state\n","unknown user state preserved exactly");

    const auto link=root/"linked-config";directory(link);
    fs::create_directory_symlink(moved/"config",link/"config");
    rt::LinuxRuntimeStore linked(link,revocations,clock,[]{return false;});
    require(linked.open().code==Code::recovery_required,"linked persistent root refused");
    require(!linked.storage(),"linked persistent root never opens SQLite");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3,"fresh root and pinned timezone input required");
        run(argv[1],argv[2]);
        std::cout<<"PASS "<<checks<<" Linux Runtime store/bootstrap checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';
        return 1;
    }
}
