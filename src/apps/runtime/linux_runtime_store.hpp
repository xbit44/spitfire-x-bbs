#pragma once

#include "runtime_bootstrap.hpp"
#include "sfx/adapters/authentication/sodium_authentication.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/platform/linux/other_board_text_file.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include <mutex>

namespace sfx::apps::runtime {

// Installation-owning part of the Runtime composition. Opening management is
// deliberately separate from starting caller/background services. The host
// must retire those services before close_for_maintenance; no repository or
// authentication reference may outlive that operation.
class LinuxRuntimeStore final {
public:
    LinuxRuntimeStore(std::filesystem::path installation_root,
        application::ISessionRevocationSink& revocations, application::IClock& clock,
        std::function<bool()> verified_starter_doors);
    ~LinuxRuntimeStore();
    LinuxRuntimeStore(const LinuxRuntimeStore&) = delete;
    LinuxRuntimeStore& operator=(const LinuxRuntimeStore&) = delete;

    application::AlphaControlResponse open();
    application::AlphaControlResponse bootstrap(application::AlphaBootstrapRequest&);
    application::AlphaControlResponse close_for_maintenance();
    // Host has joined every storage-using worker and retains its operation lock.
    // Keep installation ownership and the storage object's identity throughout.
    adapters::storage::SqliteCompactionResult compact_stopped_storage(const std::filesystem::path&,const std::filesystem::path& verified_backup,const std::function<bool()>&);
    // Composition owner must hold its operation lock and have no live callers.
    // Only explicit copied-folder recovery; never recreates a missing map.
    application::AlphaControlResponse prepare_copied_file_bindings(const std::function<bool()>& cancelled={});
    application::StorageStatus publish_security_snapshot(const application::LocalSecuritySnapshot&);
    void other_board_log(platform::linux_host::LinuxOtherBoardTextStorage::Log log){if(other_board_text_)other_board_text_->set_log(std::move(log));}
    application::AlphaRuntimeStatus stopped_snapshot(application::Identifier128 epoch, std::uint64_t sequence);
    // Only after the caller-service owner has proved the BBS stopped. Live
    // activation belongs to that owner and must never silently restart it.
    application::AlphaConfigurationUpdateResult save_stopped_configuration(
        const application::AlphaConfigurationUpdateRequest&);
    application::AlphaConfigurationUpdateResult check_configuration_update(const application::AlphaConfigurationUpdateRequest&);
    // Host owns live activation and must hold its publication boundary.
    application::AlphaConfigurationUpdateResult save_live_configuration(const application::AlphaConfigurationUpdateRequest&);
    application::AlphaRuntimePresentationState state() const noexcept { return state_; }
    bool owns_installation() const noexcept { return bool(ownership_); }
    const std::string& diagnostic() const noexcept { return diagnostic_; }
    const std::optional<application::OperationalConfiguration>& configuration() const noexcept { return configuration_; }

    // Composition-only access, never a web handler API. The owning host holds
    // its operation lock and prevents maintenance while a service uses these.
    adapters::storage::SqliteStorage* storage() noexcept { return storage_.get(); }
    application::AuthenticationSecurityService* authentication() noexcept { return authentication_.get(); }
    application::IPasswordKdf& password_kdf() noexcept { return kdf_; }
    platform::linux_host::LinuxSecureIdentifierGenerator& identifiers() noexcept { return identifiers_; }
    std::shared_ptr<platform::linux_host::LinuxFilePayloadBindings> file_bindings() const noexcept { return file_bindings_; }

private:
    application::AlphaConfigurationUpdateResult update_configuration(const application::AlphaConfigurationUpdateRequest&,bool publish);
    application::AlphaControlResponse recover(std::string diagnostic);
    application::AlphaControlResponse refresh();
    application::StorageStatus publish_configuration(const application::OperationalConfiguration&);
    std::optional<application::OperationalConfiguration> read_configuration();
    void provision();
    bool persistent_state() const;
    bool boundary_intact() const;
    void finish_first_run_clamav_policy();

    std::filesystem::path root_;
    application::ISessionRevocationSink& revocations_;
    application::IClock& clock_;
    std::function<bool()> verified_starter_doors_;
    platform::linux_host::InstallationLock ownership_;
    platform::linux_host::FileDescriptor root_directory_, config_directory_, data_directory_;
    platform::linux_host::LinuxLocalStoragePathPolicy path_policy_;
    platform::linux_host::LinuxSecureIdentifierGenerator identifiers_;
    adapters::authentication::SodiumPasswordKdf kdf_{2,16};
    adapters::authentication::PinnedSecListsBlocklist blocklist_;
    std::shared_ptr<platform::linux_host::LinuxOtherBoardTextStorage> other_board_text_;
    std::shared_ptr<platform::linux_host::LinuxFilePayloadBindings> file_bindings_;
    std::unique_ptr<adapters::storage::SqliteStorage> storage_;
    std::unique_ptr<application::AuthenticationSecurityService> authentication_;
    std::optional<application::OperationalConfiguration> configuration_;
    application::AlphaRuntimePresentationState state_{application::AlphaRuntimePresentationState::stopped};
    std::string diagnostic_{"management_store_closed"};
    bool fresh_setup_allowed_{false};
};

} // namespace sfx::apps::runtime
