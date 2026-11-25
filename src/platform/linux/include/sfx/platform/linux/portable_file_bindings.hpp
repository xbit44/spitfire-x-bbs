#pragma once
#include "sfx/platform/linux/file_payload_store.hpp"
namespace sfx::platform::linux_host {
struct FileBindingMoveProof {
    std::filesystem::path from,to;
    application::FilePayloadIdentity payload;
};
struct FileBindingRemovalProof {
    std::filesystem::path from,final_path;
    application::FilePayloadIdentity payload,final_payload;
};
struct FileBindingImportProof {
    std::filesystem::path source,configured;
    application::FilePayloadIdentity payload;
};
struct FileBindingPrepareProofs {
    std::vector<FileBindingMoveProof> moves;
    std::vector<FileBindingRemovalProof> removals;
    // Explicit durable Sysop purge intent, not an inferred missing file.
    std::vector<std::pair<std::filesystem::path,application::FilePayloadIdentity>> deletions;
    std::vector<FileBindingImportProof> imports;
};
// One stopped/owning Runtime context. The authenticated Linux map is separate
// from Windows file-identities.v1; neither representation silently replaces the
// other. Native birth/device/inode identify custody, never the portable ID.
// The authenticated v2 row representation accepts the original board/hex IDs
// and a board/decimal uint64 namespace for newly created FTN objects, matching
// the existing FTN database shape. Both retain their IDs through moves/Prepare;
// existing rows are never renumbered. Ordinary new file-area IDs stay hex.
class LinuxFilePayloadBindings final:public IFilePayloadBindings {
public:
    // Trusted incomplete-first-run composition only; never an absent-map repair.
    static std::shared_ptr<LinuxFilePayloadBindings> initialize_fresh(const std::filesystem::path&);
    static std::shared_ptr<LinuxFilePayloadBindings> open(const std::filesystem::path&);
    // Trusted stopped core-restore transaction only. Preserve every logical
    // record and durably require explicit Prepare when core payloads moved.
    static bool mark_restored_core(const std::filesystem::path&);
    // Explicit stopped-board Prepare. Unique matching payloads are rebound;
    // missing/changed/ambiguous payloads retain expected evidence as deferred,
    // never a live old identity. Authentication/cancel/publication failures
    // preserve the prior map. Prepare never changes payloads or business state.
    static std::shared_ptr<LinuxFilePayloadBindings> prepare(const std::filesystem::path&,
        const std::function<bool()>& cancelled={},const FileBindingPrepareProofs& proofs={});
    ~LinuxFilePayloadBindings();
    application::FileObjectIdentity existing(const std::filesystem::path&,int) override;
    application::FileObjectIdentity created(const std::filesystem::path&,int) override;
    void moved(const std::filesystem::path&,const std::filesystem::path&,int,const application::FileObjectIdentity&) override;
    void removed(const std::filesystem::path&,const application::FileObjectIdentity&) override;
    void checkpoint(const std::filesystem::path&,int,const application::FilePayloadIdentity&) override;
    bool can_move(const std::filesystem::path&,const std::filesystem::path&) const override;
    // Quiescent stopped/backup capture; no new logical IDs or business receipts.
    void refresh(const std::function<bool()>& cancelled={});
    // Authenticated historical install roots, restricted to owned roles.
    // External absolute paths are retained; unsafe relative aliases fail.
    std::filesystem::path resolve_path(const std::filesystem::path&) const override;
    std::filesystem::path logical_path(const std::filesystem::path&) const;
    std::size_t deferred_records() const;
    // Explicit Sysop import capability only. Preview derives an unregistered
    // object's stable candidate ID without publishing it. Adoption requires a
    // durable import intent plus the exact held preview identity and bytes.
    application::FileObjectIdentity preview_unregistered(const std::filesystem::path&,int,const std::optional<application::FilePayloadIdentity>& confirmed={});
    void adopt_unregistered(const std::filesystem::path&,int,const application::FilePayloadIdentity&,const std::function<bool()>& cancelled={},const std::function<void(bool protected_permissions)>& boundary={});
    // Native recovery composition only, after exact durable operation proof.
    // No new identities/content acceptance; ambiguous native evidence is kept.
    void reconcile_recorded_move(const std::filesystem::path& from,const std::filesystem::path& to,
        const application::FilePayloadIdentity&);
    void reconcile_recorded_removal(const std::filesystem::path&,const application::FilePayloadIdentity&);
private:
    struct Impl;explicit LinuxFilePayloadBindings(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
