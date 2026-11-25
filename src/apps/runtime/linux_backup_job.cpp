#include "linux_backup_job.hpp"
#include "../product_version.h"
#include "sfx/archive/managed_journal_snapshot.hpp"
#include "sfx/platform/linux/ftn_installation.hpp"
#include "sfx/platform/linux/installation_layout.hpp"
#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sfx::apps::runtime {
namespace {
namespace app=application;
namespace native=platform::linux_host;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string bounded(std::string text){text.erase(std::remove(text.begin(),text.end(),'\r'),text.end());std::replace(text.begin(),text.end(),'\n',' ');if(text.size()>2048)text.resize(2048);return text;}
}
LinuxRuntimeJobs::Outcome execute_linux_backup(const std::filesystem::path& root,
    adapters::storage::SqliteStorage& storage,std::shared_ptr<native::LinuxFilePayloadBindings> bindings,
    std::shared_ptr<LinuxFtnEngine> mailer,std::shared_ptr<AlphaOperationalJournal> journal,
    std::shared_ptr<AlphaDurableJournal> durable,std::string generation,
    const app::ScheduledRun& run,const std::function<bool()>& cancelled,std::filesystem::path* verified_archive){
    struct Attention {std::shared_ptr<AlphaOperationalJournal> log;std::string operation;bool done{};
        ~Attention(){if(!done&&log)log->problem(app::AttentionKind::work_preserved,"Backup "+operation,"backup_job_incomplete_work_preserved");}} attention{journal,run.run_id};
    struct Hold {std::shared_ptr<LinuxFtnEngine> engine;~Hold(){if(engine)engine->resume_after_backup();}} hold;
    try{
        require(bindings&&run.job_id==app::kBackupJobId&&run.run_id.size()==32,"backup_request_invalid");
        require(std::all_of(run.run_id.begin(),run.run_id.end(),[](char c){return (c>='0'&&c<='9')||(c>='A'&&c<='F');}),"backup_run_identity_invalid");
        require(!cancelled||!cancelled(),"backup_cancelled");
        auto root_fd=native::open_owned_directory(root);struct stat ftn{};bool layout{};
        native::layout::require_current(root);
        if(fstatat(root_fd.get(),"ftn",&ftn,AT_SYMLINK_NOFOLLOW)==0){native::verify_ftn_directory({root,bindings});layout=true;}
        else {require(errno==ENOENT,"backup_ftn_role_unavailable");const auto config=storage.ftn_configuration({true,{}});require(config&&config.value&&config.value->networks.empty(),"backup_ftn_role_missing");}
        if(layout&&mailer){require(mailer->pause_for_backup(),"ftn_backup_could_not_quiesce");hold.engine=std::move(mailer);}
        const auto kind=run.scheduled?archive::BackupKind::scheduled:archive::BackupKind::manual;
        auto collection=native::open_backup_collection(root,bindings,kind,run.backup_destination);
        const auto files=storage.file_configuration({true,{}});require(files&&files.value,"file_backup_configuration_unavailable");
        auto contents=native::FileBackupCapture::begin(root,bindings,*files.value,collection,kind,cancelled);
        const auto work=native::FilePayloadDirectory::private_store(root/"work",bindings);
        const auto database_name=native::layout::owned_name("scheduler-database-"+run.run_id+".sqlite");require(work.absent(database_name),"scheduler_snapshot_destination_exists");
        const auto snapshot=work.path()/database_name;
        std::unique_ptr<native::FileBackupSnapshot> database_cleanup;
        std::vector<std::unique_ptr<native::FileBackupSnapshot>> journal_cleanup;
        std::vector<archive::CreateBackupRequest::SourceOverride> journal_snapshots;
        require(journal&&durable,"backup_coordinated_journal_owners_unavailable");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        const auto capture_progress=[&](auto,auto){return (!cancelled||!cancelled())&&std::chrono::steady_clock::now()<deadline;};
        const auto capture_check=[&]{
            require(!cancelled||!cancelled(),"backup_journal_capture_cancelled_work_preserved");
            require(std::chrono::steady_clock::now()<deadline,"backup_journal_capture_deadline_work_preserved");
        };
        std::string capture_diagnostic;
        const bool captured=durable->capture_backup_files([&]{
            return journal->capture_backup_files([&]{
                capture_check();
                // The caller publisher is between complete publications here.
                // Queued/unprepared DB records remain recoverable after restore.
                const auto copied=storage.create_backup(snapshot);
                require(bool(copied.status),"scheduler_database_snapshot_failed_preserved");
                native::secure_new_backup_snapshot(work,database_name);
                database_cleanup=std::make_unique<native::FileBackupSnapshot>(work,database_name);
                capture_check();
                const auto logs=native::FilePayloadDirectory::private_store(root/"logs",bindings);
                auto log_directory=native::open_owned_directory(root/"logs");
                native::FileDescriptor scan(openat(log_directory.get(),".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
                require(bool(scan),"backup_journal_directory_unavailable");
                const auto duplicate=fcntl(scan.get(),F_DUPFD_CLOEXEC,0);require(duplicate>=0,"backup_journal_directory_dup_failed");
                DIR* raw=fdopendir(duplicate);
                if(!raw){close(duplicate);throw std::runtime_error("backup_journal_directory_scan_failed");}
                struct Close {DIR* value;~Close(){closedir(value);}} close_scan{raw};
                std::vector<std::string> names;std::size_t examined{};
                for(;;){
                    capture_check();errno=0;const auto* entry=readdir(raw);
                    if(!entry){require(errno==0,"backup_journal_directory_scan_failed");break;}
                    const std::string name=entry->d_name;if(name=="."||name=="..")continue;
                    require(++examined<=4096,"backup_journal_directory_entry_bound");
                    if(!archive::is_linux_journal_snapshot_member(native::layout::archive_relative("logs/"+name)))continue;
                    require(names.size()<771,"backup_journal_snapshot_count_bound");
                    names.push_back(name);
                }
                std::sort(names.begin(),names.end());
                require(std::binary_search(names.begin(),names.end(),"callers.log")&&
                    std::binary_search(names.begin(),names.end(),"heysysop.log"),"backup_durable_active_journal_missing");
                for(const auto& name:names){
                    capture_check();
                    try{
                        auto source=logs.read(name);
                        require(source.byte_count()<=app::kDurableJournalMaximumFileBytes,"journal_exceeds_existing_limit");
                        const auto identity=source.identity(capture_progress);
                        const auto leaf=native::layout::owned_name("journal-"+run.run_id+"-"+std::to_string(journal_snapshots.size())+".snapshot");
                        require(work.absent(leaf),"journal_snapshot_destination_exists");
                        {
                            auto output=work.create(leaf,std::max<std::uint64_t>(1,identity.bytes));
                            for(std::uint64_t at=0;at<identity.bytes;){
                                capture_check();const auto bytes=source.read(at);
                                require(!bytes.empty()&&bytes.size()<=identity.bytes-at,"journal_capture_size_changed");
                                output.append(bytes.data(),bytes.size());at+=bytes.size();
                            }
                            output.flush();const auto copy=output.identity(capture_progress);
                            require(copy.bytes==identity.bytes&&copy.sha256==identity.sha256,"journal_capture_mismatch");
                        }
                        journal_cleanup.push_back(std::make_unique<native::FileBackupSnapshot>(work,leaf));
                        journal_snapshots.push_back({native::layout::archive_relative("logs/"+name),work.path()/leaf});
                    }catch(const std::exception& error){throw std::runtime_error("backup_journal_snapshot_failed: logs/"+name+": "+error.what());}
                }
                logs.verify();capture_check();return true;
            });
        },capture_diagnostic);
        if(!captured)throw std::runtime_error(capture_diagnostic.empty()?"backup_coordinated_capture_failed_work_preserved":capture_diagnostic);
        require(bool(database_cleanup),"backup_database_capture_missing");
        require(bool(storage.validate_backup(snapshot)),"scheduler_database_snapshot_validation_failed");
        const auto current=storage.file_configuration({true,{}});require(current&&current.value,"file_backup_configuration_unavailable");
        archive::ManagedBackupRequest request;request.installation_root=root;request.destination_directory=collection.directory;
        request.physical_layout=archive::PhysicalLayout::linux_lowercase_v1;
        request.ftn_layout=layout;request.kind=kind;request.transaction_id=run.run_id;request.source_generation=std::move(generation);
        request.created=archive::current_timestamp();request.database_snapshot=snapshot;request.cancelled=cancelled;
        request.sfx_version=SFX_PRODUCT_VERSION_SHORT_STRING;
        request.linux_journal_snapshot_set=true;request.linux_journal_snapshots=std::move(journal_snapshots);
        contents->bind(request,*current.value);
        bindings->refresh(cancelled);
        const auto map_name=native::layout::owned_name("file-identities-"+run.run_id+".snapshot");
        {const auto config=native::FilePayloadDirectory::private_store(root/"config",bindings);auto source=config.read("file-identities-linux.v1");
            const auto identity=source.identity([&](auto,auto){return !cancelled||!cancelled();});
            auto output=work.create(map_name,std::max<std::uint64_t>(1,identity.bytes));
            for(std::uint64_t at=0;at<identity.bytes;){require(!cancelled||!cancelled(),"backup_identity_capture_cancelled");auto bytes=source.read(at);require(!bytes.empty(),"backup_identity_capture_short_read");output.append(bytes.data(),bytes.size());at+=bytes.size();}
            const auto captured=output.identity();require(captured.bytes==identity.bytes&&captured.sha256==identity.sha256,"backup_identity_capture_mismatch");}
        native::FileBackupSnapshot identity_cleanup(work,map_name);request.linux_portable_identity_snapshot=work.path()/map_name;
        const auto backup=archive::create_managed_backup(request);
        if(!backup.result.ok())throw std::runtime_error("scheduler_archive_failed_work_preserved: "+backup.result.detail);
        native::secure_new_backup_archive(collection,backup.archive_path);
        const auto inspected=archive::inspect_backup(backup.archive_path,request.limits,cancelled);require(inspected.result.ok(),"scheduler_archive_readback_failed");
        (void)archive::inspect_content_index(backup.archive_path,inspected);
        if(verified_archive)*verified_archive=backup.archive_path;
        require(contents->cleanup()&&database_cleanup->cleanup()&&identity_cleanup.cleanup(),"scheduler_snapshot_cleanup_failed_preserved");
        for(auto& journal_snapshot:journal_cleanup)require(journal_snapshot->cleanup(),"scheduler_journal_snapshot_cleanup_failed_preserved");
        attention.done=true;
        std::size_t pruned{};
        try{pruned=native::finish_backup_collection(root,bindings,collection,backup.archive_path,kind,run.retained_backups,cancelled);}
        catch(const std::exception& e){return {app::ScheduledJobResult::failed,bounded("Verified archive retained at "+backup.archive_path.string()+"; retention incomplete: "+e.what())};}
        return {app::ScheduledJobResult::success,bounded(backup.archive_path.string()+"; retained limit="+std::to_string(run.retained_backups)+"; pruned="+std::to_string(pruned))};
    }catch(const std::exception& e){return {app::ScheduledJobResult::failed,bounded(e.what())};}
    catch(...){return {app::ScheduledJobResult::failed,"scheduler_backup_unknown_failure_preserved"};}
}
} // namespace sfx::apps::runtime
