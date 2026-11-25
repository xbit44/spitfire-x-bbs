#pragma once
#include "sfx/platform/win32/file_administration_client.hpp"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
namespace sfx::file_manager {
namespace app=application;namespace win=platform::win32;
enum class ClientCommandKind { connect,configuration,read,start,status,cancel,disconnect,hatch_catalog };
struct ClientCommand {
    std::uint64_t serial{};
    ClientCommandKind kind{ClientCommandKind::read};
    app::FileManagementReadRequest query;
    app::FileManagementTask task;
    app::Identifier128 job;
};
struct ClientReply {
    std::uint64_t serial{};
    ClientCommandKind kind{ClientCommandKind::read};
    bool connected{},online{}; // typed client exists; individual RPC may still fail
    std::string diagnostic;
    std::optional<app::FileConfiguration> configuration;
    std::optional<app::FtnHatchCatalog> hatch_catalog;
    app::LocalAccountResultCode read_code{app::LocalAccountResultCode::unavailable};
    std::vector<app::ManagedFile> records; // bounded32-row native UI page
    app::FileId next;
    std::vector<app::FileManagementPending> pending;
    app::Identifier128 pending_next;
    app::FileManagementJobStatus status;
};
// One serialized transport thread. UI never blocks on named-pipe/DB/file IO,
// never owns SQL/storage, and cannot race two calls through one owner client.
// The injectable typed factory is for native model tests, not a shipping CLI
// authority switch; normal entry always uses the protected production factory.
class ClientWorker final {
public:
    using Factory=std::function<std::unique_ptr<win::IFileAdministrationClient>(const std::filesystem::path&,const std::wstring&,std::string&)>;
    ClientWorker(std::filesystem::path,std::wstring,Factory = win::open_file_administration_client);
    ~ClientWorker();
    ClientWorker(const ClientWorker&)=delete;
    bool submit(ClientCommand);
    std::optional<ClientReply> poll();
    bool busy() const;
    void stop();
private:
    void run();
    ClientReply perform(const ClientCommand&);
    std::filesystem::path root_;
    std::wstring owner_;
    Factory factory_;
    std::unique_ptr<win::IFileAdministrationClient> client_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<ClientCommand> command_;
    std::optional<ClientReply> reply_;
    bool active_{},stopping_{};
    std::thread thread_;
};
enum class JobTracking { idle,running,unknown };
// Unknown start/status is NOT failure or success. Keep the exact task and job
// identity, freeze new actions and offer same-ID status/retry/cancel only.
class JobTracker final {
public:
    bool begin(const app::FileManagementTask&);
    bool observe(const app::FileManagementJobStatus&,bool start_response);
    void link_failed() noexcept;
    JobTracking state() const noexcept{return state_;}
    bool blocks_edits() const noexcept{return state_!=JobTracking::idle;}
    const std::optional<app::FileManagementTask>& exact_task()const noexcept{return task_;}
private:
    JobTracking state_{JobTracking::idle};
    std::optional<app::FileManagementTask> task_;
};
} // namespace sfx::file_manager
