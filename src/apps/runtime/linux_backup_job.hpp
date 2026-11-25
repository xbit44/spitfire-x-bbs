#pragma once
#include "linux_runtime_jobs.hpp"
#include "linux_ftn_engine.hpp"
#include "alpha_durable_journal.hpp"
#include "sfx/platform/linux/file_backup.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"

namespace sfx::apps::runtime {
// The host retains exclusive installation/store ownership and joins this job
// before retiring FTN or storage. A stopped BBS has no FTN producer to pause.
LinuxRuntimeJobs::Outcome execute_linux_backup(const std::filesystem::path&,
    adapters::storage::SqliteStorage&,std::shared_ptr<platform::linux_host::LinuxFilePayloadBindings>,
    std::shared_ptr<LinuxFtnEngine>,std::shared_ptr<AlphaOperationalJournal>,std::shared_ptr<AlphaDurableJournal>,std::string source_generation,
    const application::ScheduledRun&,const std::function<bool()>& cancelled,std::filesystem::path* verified_archive=nullptr);
} // namespace sfx::apps::runtime
