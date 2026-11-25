#include "linux_runtime_store.hpp"
#include "runtime_configuration_update.hpp"
#include "linux_security_publication.hpp"
#include "sfx/platform/linux/installation_layout.hpp"
#include <charconv>
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "sfx/platform/linux/file_intake.hpp"
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace sfx::apps::runtime {
namespace {
namespace app = application;
namespace host = platform::linux_host;
namespace native_layout = host::layout;
using Code = app::LocalControlResponseCode;
using State = app::AlphaRuntimePresentationState;

bool same_directory(int pinned, const std::filesystem::path& path) {
    auto current = host::open_owned_directory(path);
    struct stat before{}, now{};
    return fstat(pinned,&before)==0 && fstat(current.get(),&now)==0 &&
        before.st_dev==now.st_dev && before.st_ino==now.st_ino;
}
bool directory_has_entries(int directory, std::string_view ignored = {}) {
    // A separate open description avoids sharing directory iteration offsets.
    const int descriptor = openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (descriptor<0) throw std::runtime_error("installation_state_scan_failed");
    DIR* stream=fdopendir(descriptor);
    if (!stream) { close(descriptor); throw std::runtime_error("installation_state_scan_failed"); }
    bool found=false;
    errno=0;
    while (const auto* entry=readdir(stream)) {
        const std::string_view name(entry->d_name);
        if (name!="." && name!=".." && name!=ignored) { found=true; break; }
    }
    const int error=errno;
    closedir(stream);
    if (error) throw std::runtime_error("installation_state_scan_failed");
    return found;
}
std::optional<std::string> canonical_zone(std::string_view name) {
    auto zone=host::LinuxBbsTimeZoneRuleProvider::create(name);
    if (!zone) return std::nullopt;
    return zone->configuration().canonical_windows_registry_key;
}
}

LinuxRuntimeStore::LinuxRuntimeStore(std::filesystem::path root,
    app::ISessionRevocationSink& revocations, app::IClock& clock,
    std::function<bool()> verified_starter_doors)
    : root_(std::move(root)), revocations_(revocations), clock_(clock),
      verified_starter_doors_(std::move(verified_starter_doors)) {}

LinuxRuntimeStore::~LinuxRuntimeStore() {
    (void)close_for_maintenance();
    kdf_.shutdown();
}

app::AlphaControlResponse LinuxRuntimeStore::recover(std::string diagnostic) {
    state_=State::recovery_required;
    diagnostic_=std::move(diagnostic);
    return {Code::recovery_required,diagnostic_};
}

bool LinuxRuntimeStore::boundary_intact() const {
    return same_directory(root_directory_.get(),root_) &&
        same_directory(config_directory_.get(),root_/"config") &&
        same_directory(data_directory_.get(),root_/"data");
}

void LinuxRuntimeStore::provision() {
    if (geteuid()==0) throw std::runtime_error("runtime_normal_account_required");
    root_directory_=host::open_owned_directory(root_);
    native_layout::preflight(root_);
    if (!ownership_.acquire(root_,".sfx-runtime.lock"))
        throw std::runtime_error("installation_already_owned_or_lock_unavailable");
    native_layout::initialize_or_verify(root_);
    for (const auto* name:{"config","data","display","doors","logs","botgate","backup"}) {
        if (mkdirat(root_directory_.get(),name,0700)!=0 && errno!=EEXIST)
            throw std::runtime_error("installation_directory_create_failed");
        // Existing permissions and links are assessed, never repaired silently.
        auto checked=host::open_owned_directory(root_/name);
        (void)checked;
    }
    if (fsync(root_directory_.get())!=0)
        throw std::runtime_error("installation_directory_flush_failed");
    config_directory_=host::open_owned_directory(root_/"config");
    data_directory_=host::open_owned_directory(root_/"data");
    if (!boundary_intact()) throw std::runtime_error("installation_boundary_changed");
}

bool LinuxRuntimeStore::persistent_state() const {
    // These are persistent data roots, not installation payload roots. Unknown
    // or interrupted state is conservatively retained for explicit recovery.
    // The exact verified schema marker is not pre-existing board state.
    return directory_has_entries(config_directory_.get(),"linux-layout.v1") || directory_has_entries(data_directory_.get());
}

std::optional<app::OperationalConfiguration> LinuxRuntimeStore::read_configuration() {
    if (!boundary_intact()) throw std::runtime_error("installation_boundary_changed");
    struct stat info{};
    if (fstatat(config_directory_.get(),"sfx.conf",&info,AT_SYMLINK_NOFOLLOW)!=0) {
        if (errno==ENOENT) return std::nullopt;
        throw std::runtime_error("configuration_inspection_failed");
    }
    const auto bytes=host::read_private_file(config_directory_.get(),"sfx.conf",app::kOperationalConfigurationMaximumBytes);
    const auto parsed=app::parse_operational_configuration(std::string(bytes.begin(),bytes.end()));
    if (!parsed.valid()) throw std::runtime_error("configuration_invalid");
    const auto zone=canonical_zone(parsed.configuration->bbs_time_zone);
    if (!zone || *zone!=parsed.configuration->bbs_time_zone)
        throw std::runtime_error("configuration_time_zone_unavailable");
    return parsed.configuration;
}

app::StorageStatus LinuxRuntimeStore::publish_configuration(const app::OperationalConfiguration& candidate) {
    try {
        if (!boundary_intact()) throw std::runtime_error("installation_boundary_changed");
        const auto text=app::serialize_operational_configuration(candidate);
        if (text.empty()) return {app::StorageErrorCode::invalid_argument,"configuration_invalid"};
        // Shared bootstrap has proved the staged Sysop/System Password. Publish
        // the portable identity before the first configuration can become ready.
        const auto first=storage_->first_run_state();
        if(!fresh_setup_allowed_||!first||!first.value||first.value->complete)
            return {app::StorageErrorCode::invalid_argument,"portable_key_incomplete_first_run_required"};
        const auto key=host::initialize_fresh_portable_board_key(root_);
        if(!key)return {app::StorageErrorCode::io,key.diagnostic};
        // First-run completion only. Never reconstruct missing established
        // library/recovery folders during ordinary open or caller login.
        for(const auto* name:{"work","ftn","files","files/general","files/uploads","files/uploadtosysop","data/fileintake","data/fileremoved"}){
            const std::filesystem::path relative(name);
            auto parent=host::open_owned_directory(relative.has_parent_path()?root_/relative.parent_path():root_);
            if(mkdirat(parent.get(),relative.filename().c_str(),0700)!=0&&errno!=EEXIST)
                throw std::runtime_error("fresh_file_directory_create_failed");
            auto checked=host::open_owned_directory(root_/relative);(void)checked;
            if(fsync(parent.get())!=0)throw std::runtime_error("fresh_file_directory_flush_failed");
        }
        if(!file_bindings_){
            struct stat map{};
            if(fstatat(config_directory_.get(),"file-identities-linux.v1",&map,AT_SYMLINK_NOFOLLOW)==0)
                file_bindings_=host::LinuxFilePayloadBindings::open(root_);
            else if(errno==ENOENT)
                file_bindings_=host::LinuxFilePayloadBindings::initialize_fresh(root_);
            else throw std::runtime_error("linux_file_map_inspection_failed");
        }
        host::publish_private_file(config_directory_.get(),"sfx.conf",{text.begin(),text.end()},false);
        if (!boundary_intact()) throw std::runtime_error("installation_boundary_changed");
        return {};
    } catch (const std::exception& error) {
        return {app::StorageErrorCode::io,error.what()};
    }
}

app::AlphaControlResponse LinuxRuntimeStore::open() {
    if (ownership_ || storage_) return {Code::invalid_request,"management_store_already_open"};
    state_=State::starting;
    try {
        provision();
        struct stat compaction_marker{};
        if(fstatat(config_directory_.get(),"compaction-pending.v1",&compaction_marker,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT)
            return recover("interrupted_compaction_requires_verified_recovery");
        const bool pristine=!persistent_state();
        // This portable intent record distinguishes management's first empty
        // database from an existing installation with lost configuration.
        // It never permits replacing a missing/invalid database, or completing
        // an interrupted staged Sysop transaction without credential proof.
        const std::string intent="SFX_LINUX_FIRST_RUN=1\n";
        fresh_setup_allowed_=pristine;
        if (pristine) {
            host::publish_private_file(config_directory_.get(),"first-run.v1",
                {intent.begin(),intent.end()},false);
        } else {
            struct stat marker{};
            if (fstatat(config_directory_.get(),"first-run.v1",&marker,AT_SYMLINK_NOFOLLOW)==0) {
                const auto bytes=host::read_private_file(config_directory_.get(),"first-run.v1",128);
                fresh_setup_allowed_=std::string(bytes.begin(),bytes.end())==intent;
            } else if (errno!=ENOENT) return recover("first_run_intent_unavailable");
        }
        configuration_=read_configuration();
        if (!configuration_ && !fresh_setup_allowed_) return recover("configuration_missing");
        host::FileDescriptor database_pin;
        if (!pristine) {
            // Metadata-only identity handle: closing an ordinary descriptor
            // after SQLite opens would drop this process's POSIX DB locks.
            database_pin=host::FileDescriptor(openat(data_directory_.get(),"sfx.db",O_PATH|O_NOFOLLOW|O_CLOEXEC));
            struct stat info{};
            if (!host::private_regular_file(database_pin.get(),UINT64_MAX) ||
                fstat(database_pin.get(),&info)!=0 || info.st_size==0)
                return recover("installation_database_missing_or_invalid");
        }
        auto settings=native_layout::storage_settings(root_);
        host::initialize_other_board_text(root_);
        other_board_text_=std::make_shared<host::LinuxOtherBoardTextStorage>(root_,host::LinuxOtherBoardTextStorage::Log{});
        settings.other_board_text=other_board_text_;
        settings.utc_now=[this] { return clock_.unix_seconds(); };
        storage_=std::make_unique<adapters::storage::SqliteStorage>(settings,path_policy_,
            adapters::storage::sfx_storage_migrations());
        const auto started=storage_->start();
        if (!started) return recover(started.operation);
        if (!boundary_intact()) return recover("installation_boundary_changed");
        // A renamed/replaced database must not silently become a different BBS.
        if (database_pin) {
            struct stat pinned{}, named{};
            if (fstat(database_pin.get(),&pinned)!=0 ||
                fstatat(data_directory_.get(),"sfx.db",&named,AT_SYMLINK_NOFOLLOW)!=0 ||
                pinned.st_dev!=named.st_dev || pinned.st_ino!=named.st_ino)
                return recover("installation_database_changed");
        }
        authentication_=std::make_unique<app::AuthenticationSecurityService>(
            *storage_,*storage_,kdf_,&blocklist_,revocations_);
        return refresh();
    } catch (const std::exception& error) { return recover(error.what()); }
}

app::AlphaControlResponse LinuxRuntimeStore::refresh() {
    configuration_=read_configuration();
    const auto first=storage_->first_run_state();
    const auto system=storage_->system_password();
    const auto policy=storage_->security_policy();
    if (!first || !first.value || !policy || !policy.value ||
        (!system && system.status.code!=app::StorageErrorCode::not_found))
        return recover("bootstrap_state_unavailable");
    const bool system_present=system.succeeded() && system.value.has_value();
    if(configuration_) {
        const auto key=host::load_portable_board_key(root_);
        if(!key)return recover(key.diagnostic);
        if(!file_bindings_)file_bindings_=host::LinuxFilePayloadBindings::open(root_);
    }
    if (first.value->complete) {
        if(configuration_){
            LinuxSecurityPublication publication;
            const auto truth=app::LocalSecurityAdministration(*storage_,publication,identifiers_,clock_,app::LocalAccountAuthority::installation_owner).snapshot();
            const auto published=publish_security_snapshot(truth);if(!published)return recover(published.operation);
        }
        if (!configuration_ || !system_present || !first.value->staged_sysop_account_id ||
            policy.value->published_revision!=policy.value->revision ||
            policy.value->published_policy_hash!=policy.value->policy_hash ||
            policy.value->revision!=configuration_->security_policy_revision ||
            policy.value->policy_hash!=configuration_->security_policy_hash)
            return recover("completed_bootstrap_mismatch");
        finish_first_run_clamav_policy();
        state_=State::ready;
        diagnostic_=file_bindings_->deferred_records()?"ready_bbs_stopped_file_attention":"ready_bbs_stopped";
        return {Code::ok,diagnostic_};
    }
    if (fresh_setup_allowed_ &&
        ((!first.value->staged_sysop_account_id && !system_present && !configuration_) ||
         first.value->staged_sysop_account_id)) {
        // A known incomplete first run is resumable. The shared transaction
        // proves the same Sysop name/password, System Password when present,
        // and exact already-published configuration before completing it.
        state_=State::setup_required;
        diagnostic_="setup_required";
        return {Code::setup_required,diagnostic_};
    }
    return recover("bootstrap_interrupted_recovery_required");
}

app::StorageStatus LinuxRuntimeStore::publish_security_snapshot(const app::LocalSecuritySnapshot& truth){
    try{
        if(!configuration_||!boundary_intact())throw std::runtime_error("security_snapshot_boundary");
        const auto saved=read_configuration();
        if(!saved||!app::equivalent_operational_configuration(*saved,*configuration_))throw std::runtime_error("configuration_changed_outside_owner");
        const auto& policy=truth.policy;const auto definition=app::security_policy_definition(truth.levels,policy.new_user_level,policy.sysop_threshold);
        // The shared revision-one schema has a historical seed fingerprint.
        // Match Windows reconciliation: validate its published binding rather
        // than retroactively hashing it with a newer definition serializer.
        if(truth.code!=app::LocalAccountResultCode::ok||definition.empty()||policy.revision<1||policy.published_revision!=policy.revision||policy.published_policy_hash!=policy.policy_hash||
            !app::valid_sha256_text(policy.policy_hash)||saved->security_policy_revision>policy.revision||
            (saved->security_policy_revision==policy.revision&&saved->security_policy_hash!=policy.policy_hash))throw std::runtime_error("security_snapshot_binding_mismatch");
        const std::string prefix="SFX_SECURITY_POLICY=1\nrevision=";
        const auto expected=prefix+std::to_string(policy.revision)+"\nhash="+policy.policy_hash+"\n"+definition;
        struct stat info{};const bool exists=fstatat(config_directory_.get(),"sfxsecurity.conf",&info,AT_SYMLINK_NOFOLLOW)==0;
        if(!exists&&errno!=ENOENT)throw std::runtime_error("security_snapshot_inspection_failed");
        bool stale=!exists;
        if(exists){
            const auto bytes=host::read_private_file(config_directory_.get(),"sfxsecurity.conf",1024U*1024U);const std::string text(bytes.begin(),bytes.end());
            if(text.compare(0,prefix.size(),prefix)!=0)throw std::runtime_error("security_snapshot_invalid");
            const auto end=text.find('\n',prefix.size());if(end==std::string::npos)throw std::runtime_error("security_snapshot_invalid");
            std::int64_t revision{};const auto parsed=std::from_chars(text.data()+prefix.size(),text.data()+end,revision);
            if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+end||revision<1||revision>policy.revision||(revision==policy.revision&&text!=expected))throw std::runtime_error("security_snapshot_mismatch");
            stale=revision<policy.revision;
        }
        if(stale){
            host::publish_private_file(config_directory_.get(),"sfxsecurity.conf",{expected.begin(),expected.end()},exists);
            const auto bytes=host::read_private_file(config_directory_.get(),"sfxsecurity.conf",1024U*1024U);
            if(std::string(bytes.begin(),bytes.end())!=expected)throw std::runtime_error("security_snapshot_verify_failed");
        }
        if(saved->security_policy_revision<policy.revision){
            if(saved->configuration_revision==UINT64_MAX)throw std::runtime_error("security_snapshot_configuration_revision_exhausted");
            auto replacement=*saved;++replacement.configuration_revision;replacement.security_policy_revision=policy.revision;replacement.security_policy_hash=policy.policy_hash;
            const auto text=app::serialize_operational_configuration(replacement);if(text.empty())throw std::runtime_error("security_snapshot_configuration_invalid");
            host::publish_private_file(config_directory_.get(),"sfx.conf",{text.begin(),text.end()},true);configuration_=std::move(replacement);
        }
        if(!boundary_intact())throw std::runtime_error("security_snapshot_boundary_changed");
        return {};
    }catch(const std::exception& error){return {app::StorageErrorCode::io,error.what()};}
}

void LinuxRuntimeStore::finish_first_run_clamav_policy(){
    constexpr const char* name="clamav-first-run.v1";const std::string expected="SFX-LINUX-CLAMAV-FIRST-RUN-1\n";
    host::FileDescriptor intent(openat(config_directory_.get(),name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
    if(!intent){if(errno==ENOENT)return;throw std::runtime_error("clamav_first_run_intent_unavailable");}
    const auto bytes=host::read_private_file(config_directory_.get(),name,128);
    if(!host::private_regular_file(intent.get(),128)||std::string(bytes.begin(),bytes.end())!=expected)throw std::runtime_error("clamav_first_run_intent_invalid");
    auto files=storage_->file_configuration({true,{}});if(!files||!files.value)throw std::runtime_error("clamav_first_run_policy_unavailable");
    const auto& policy=files.value->policy;
    // The first-run transaction advances untouched file-area defaults to2.
    // This durable intent is created only from revision1/catalog1 with the
    // optional component present, before that transaction. It is not a guess
    // that every existing revision2 board has an untouched scanning choice.
    // An explicit Off save after a rejected bootstrap also produces revision2.
    // Only automatic completion changes all three untouched starter areas to
    // these revision2 permissions without another catalog revision. A typed
    // explicit save/customization cannot manufacture that same result at2/2.
    bool starter_defaults=files.value->areas.size()==3;
    for(const auto& configured:files.value->areas){const auto& area=configured.area;const auto id=area.id.value;
        starter_defaults=starter_defaults&&id>=1&&id<=3&&area.revision==2&&area.group.value==1&&area.enabled&&area.permissions.uploads_enabled&&
            area.permissions.access==(id==3?999:10)&&area.permissions.upload==(id==1?999:10)&&area.permissions.download==(id==1?10:999);}
    if(policy.revision==2&&policy.catalog_revision==2&&!policy.scan_uploads&&starter_defaults){
        app::FileConfigurationChange change;change.operation=identifiers_.generate();change.action=app::FileConfigurationAction::policy;
        change.expected_catalog_revision=policy.catalog_revision;change.policy=policy;change.policy.scan_uploads=true;
        if(!storage_->change_file_configuration({true,{}},change))throw std::runtime_error("clamav_first_run_policy_unavailable");
    }
    // Any explicit saved choice wins. Retire this one-time default intent rather
    // than blocking a usable board or resetting an Off choice on a later retry.
    struct stat held{},named{};
    if(fstat(intent.get(),&held)!=0||fstatat(config_directory_.get(),name,&named,AT_SYMLINK_NOFOLLOW)!=0||held.st_dev!=named.st_dev||held.st_ino!=named.st_ino||unlinkat(config_directory_.get(),name,0)!=0||fsync(config_directory_.get())!=0)
        throw std::runtime_error("clamav_first_run_intent_retirement_failed");
}
app::AlphaControlResponse LinuxRuntimeStore::bootstrap(app::AlphaBootstrapRequest& request) {
    if (state_==State::recovery_required) return {Code::recovery_required,diagnostic_};
    if (!storage_ || !authentication_) return {Code::storage_failure,"storage_unavailable"};
    if (state_==State::ready) return {Code::ok,"bootstrap_already_complete"};
    if (state_!=State::setup_required) return {Code::shutting_down,"runtime_not_accepting_bootstrap"};
    RuntimeBootstrapPorts ports;
    ports.strong_passwords_ready=[this] { return blocklist_.ready(); };
    ports.canonical_time_zone=canonical_zone;
    ports.now_utc_seconds=[this] { return clock_.unix_seconds(); };
    ports.account_audit=[this](auto reason,auto actor) -> app::AuditContext {
        return {identifiers_.generate(),identifiers_.generate(),actor,clock_.unix_seconds(),reason};
    };
    ports.authentication_audit=[this](auto actor) -> app::AuthenticationAuditContext {
        return {identifiers_.generate(),identifiers_.generate(),actor,clock_.unix_seconds()};
    };
    ports.publish_configuration=[this](const auto& candidate) { return publish_configuration(candidate); };
    ports.verified_starter_doors=verified_starter_doors_;
    try {
        if (!boundary_intact()) return recover("installation_boundary_changed");
        struct stat bin_info{};
        if(fstatat(root_directory_.get(),"bin",&bin_info,AT_SYMLINK_NOFOLLOW)==0){
            auto bin=host::open_owned_directory(root_/"bin");struct stat component{};
            if(fstatat(bin.get(),"clamav",&component,AT_SYMLINK_NOFOLLOW)==0){
                const auto files=storage_->file_configuration({true,{}});if(!files||!files.value)return recover("clamav_first_run_policy_unavailable");
                if(files.value->policy.revision==1&&files.value->policy.catalog_revision==1){
                    const std::string intent="SFX-LINUX-CLAMAV-FIRST-RUN-1\n";struct stat existing{};
                    if(fstatat(config_directory_.get(),"clamav-first-run.v1",&existing,AT_SYMLINK_NOFOLLOW)!=0){
                        if(errno!=ENOENT)return recover("clamav_first_run_intent_unavailable");
                        host::publish_private_file(config_directory_.get(),"clamav-first-run.v1",{intent.begin(),intent.end()},false);
                    }else {const auto bytes=host::read_private_file(config_directory_.get(),"clamav-first-run.v1",128);if(std::string(bytes.begin(),bytes.end())!=intent)return recover("clamav_first_run_intent_invalid");}
                }
            }else if(errno!=ENOENT)return recover("clamav_component_presence_unavailable");
        }else if(errno!=ENOENT)return recover("clamav_component_presence_unavailable");
        const auto result=run_runtime_bootstrap_transaction(*storage_,*authentication_,request,
            configuration_,state_,diagnostic_,ports);
        if (result.code!=Code::ok) return result;
        return refresh();
    } catch (const std::exception& error) { return recover(error.what()); }
}

adapters::storage::SqliteCompactionResult LinuxRuntimeStore::compact_stopped_storage(const std::filesystem::path& temporary,const std::filesystem::path& verified_backup,const std::function<bool()>& cancelled){
    adapters::storage::SqliteCompactionResult result;
    if(state_!=State::ready||!storage_||!boundary_intact()){result.status={app::StorageErrorCode::shutting_down,"compaction_store_not_ready"};return result;}
    try{const auto marker="SFX_LINUX_COMPACTION_PENDING=1\nVerified backup: "+verified_backup.string()+"\nTemporary work: "+temporary.string()+"\n";
        host::publish_private_file(config_directory_.get(),"compaction-pending.v1",{marker.begin(),marker.end()},false);
    }catch(...){result.status={app::StorageErrorCode::io,"compaction_recovery_marker_unavailable"};(void)recover("compaction_recovery_marker_unavailable");return result;}
    const auto stopped=storage_->shutdown();if(!stopped){result.status=stopped;(void)recover("compaction_storage_shutdown_failed");return result;}
    result=storage_->compact_offline(temporary,cancelled);
    if(!result.status||!result.logical_content_verified){(void)recover("compaction_not_verified_recovery_required");return result;}
    const auto started=storage_->start();if(!started){result.status=started;(void)recover("compaction_storage_reopen_failed");return result;}
    if(unlinkat(config_directory_.get(),"compaction-pending.v1",0)!=0||fsync(config_directory_.get())!=0){result.status={app::StorageErrorCode::io,"compaction_recovery_marker_retirement_failed"};(void)recover("compaction_recovery_marker_retirement_failed");}
    return result;
}
app::AlphaControlResponse LinuxRuntimeStore::close_for_maintenance() {
    // The host has already joined caller/helper workers. A successful response
    // means SQLite and the installation lock really are closed, not just that
    // caller listeners stopped. Web process lifetime is independent.
    if (storage_) {
        const auto closed=storage_->shutdown();
        if (!closed) return recover(closed.operation);
    }
    authentication_.reset();
    storage_.reset();
    other_board_text_.reset();
    file_bindings_.reset();
    configuration_.reset();
    data_directory_=host::FileDescriptor{};
    config_directory_=host::FileDescriptor{};
    root_directory_=host::FileDescriptor{};
    ownership_.release();
    fresh_setup_allowed_=false;
    state_=State::stopped;
    diagnostic_="management_store_closed";
    return {Code::ok,diagnostic_};
}

app::AlphaControlResponse LinuxRuntimeStore::prepare_copied_file_bindings(const std::function<bool()>& cancelled) {
    bool retry{};
    try{retry=state_==State::ready&&file_bindings_&&file_bindings_->deferred_records()!=0;}
    catch(const std::exception& error){return recover(error.what());}
    if((!retry&&(state_!=State::recovery_required||diagnostic_!="linux_file_map_prepare_required"||file_bindings_))||
        !ownership_||!storage_||(retry&&file_bindings_.use_count()!=1))
        return {Code::invalid_request,"copied_file_prepare_not_available"};
    try {
        if(!boundary_intact())return recover("installation_boundary_changed");
        const auto first=storage_->first_run_state();
        const auto current=read_configuration();
        if(!first||!first.value||!first.value->complete||!current||!configuration_||
            !app::equivalent_operational_configuration(*current,*configuration_))
            return recover("copied_file_prepare_state_changed");
        file_bindings_.reset();file_bindings_=host::prepare_file_intake_bindings(*storage_,{true,{}},root_,cancelled);
        return refresh();
    } catch(const std::exception& error) {
        // Cancel or failed payload validation leaves the retained map available
        // for an explicit retry. It must never make this board ready.
        if(retry){try{file_bindings_=host::LinuxFilePayloadBindings::open(root_);}catch(const std::exception& reopen){return recover(reopen.what());}}
        return {retry?Code::invalid_request:Code::recovery_required,error.what()};
    }
}

app::AlphaRuntimeStatus LinuxRuntimeStore::stopped_snapshot(app::Identifier128 epoch,std::uint64_t sequence) {
    app::AlphaRuntimeStatus result;
    result.runtime_epoch=epoch;result.status_sequence=sequence;
    result.state=state_==State::ready ? State::stopped : state_;
    result.lifecycle.state=state_==State::recovery_required ? app::RuntimeState::recovery_required : app::RuntimeState::stopped;
    result.lifecycle.configured_nodes=configuration_ ? configuration_->logical_nodes : 4;
    result.configuration_loaded=configuration_.has_value();
    result.bootstrap_complete=state_==State::ready;
    result.diagnostic_code=diagnostic_;
    if (storage_) {
        result.storage=storage_->health();
        const auto policy=storage_->security_policy();
        if (policy && policy.value) {
            result.security_policy_revision=policy.value->revision;
            result.security_policy_hash=policy.value->policy_hash;
        }
    }
    if (configuration_) {
        const auto& value=*configuration_;
        result.configuration_revision=value.configuration_revision;
        result.bbs_name=value.bbs_name;result.bbs_time_zone=value.bbs_time_zone;
        result.telnet_configured=value.telnet.enabled;result.telnet_bind=value.telnet.bind_address;
        result.telnet_port=value.telnet.port;result.telnet_bot_gate_enabled=value.telnet.bot_gate_enabled;
        result.ssh_configured=value.ssh.enabled;result.ssh_bind=value.ssh.bind_address;result.ssh_port=value.ssh.port;
        result.allow_new_users=value.allow_new_users;result.hostname_lookup=value.hostname_lookup;
    }
    // No fabricated online nodes: only an actual caller service may publish them.
    return result;
}

app::AlphaConfigurationUpdateResult LinuxRuntimeStore::save_stopped_configuration(
    const app::AlphaConfigurationUpdateRequest& request) {return update_configuration(request,true);}
app::AlphaConfigurationUpdateResult LinuxRuntimeStore::save_live_configuration(const app::AlphaConfigurationUpdateRequest& request){return update_configuration(request,true);}
app::AlphaConfigurationUpdateResult LinuxRuntimeStore::check_configuration_update(const app::AlphaConfigurationUpdateRequest& request){return update_configuration(request,false);}
app::AlphaConfigurationUpdateResult LinuxRuntimeStore::update_configuration(const app::AlphaConfigurationUpdateRequest& request,bool publish) {
    app::AlphaConfigurationUpdateResult result;
    result.active_revision=configuration_ ? configuration_->configuration_revision : 1;
    result.saved_revision=result.active_revision;
    result.code=Code::configuration_failure;
    result.diagnostic_code="configuration_runtime_not_ready";
    result.detail="Refused - Runtime is not ready";
    if (state_!=State::ready || !configuration_) return result;
    try {
        const auto saved=read_configuration();
        if (!saved || !app::equivalent_operational_configuration(*saved,*configuration_)) {
            result.diagnostic_code="configuration_changed_outside_owner";
            result.detail="Refused - saved settings changed; reopen before saving";
            (void)recover(result.diagnostic_code);return result;
        }
        if (const auto invalid=validate_runtime_configuration_update(request,*saved)) {
            result.code=Code::invalid_request;result.diagnostic_code=invalid->diagnostic;
            result.detail=invalid->detail;return result;
        }
        const auto& candidate=request.configuration;
        const auto zone=canonical_zone(candidate.bbs_time_zone);
        if (!zone || *zone!=candidate.bbs_time_zone) {
            result.code=Code::invalid_request;result.diagnostic_code="configuration_time_zone_invalid";
            result.detail="Refused - select an available BBS time zone";return result;
        }
        if (candidate.password_mode==app::PasswordMode::strong && !blocklist_.ready()) {
            result.diagnostic_code="strong_password_blocklist_unavailable";
            result.detail="Refused - strong password checks are unavailable";return result;
        }
        if(!publish){result.code=Code::ok;result.diagnostic_code="configuration_validated";return result;}
        const auto text=app::serialize_operational_configuration(candidate);
        host::publish_private_file(config_directory_.get(),"sfx.conf",{text.begin(),text.end()},true);
        if (!boundary_intact()) throw std::runtime_error("installation_boundary_changed");
        configuration_=candidate;
        result.code=Code::ok;result.diagnostic_code="configuration_saved_next_start";
        result.activation=app::AlphaConfigurationActivation::saved_next_start;
        result.active_revision=candidate.configuration_revision;result.saved_revision=candidate.configuration_revision;
        result.detail="Saved - BBS is stopped; these settings will be used when it starts";
        return result;
    } catch (const std::exception& error) {
        result.diagnostic_code="configuration_publication_failed";
        result.detail="Save failed - review recovery status before retrying";
        (void)recover(error.what());return result;
    }
}
} // namespace sfx::apps::runtime
