#pragma once
#include "sfx/platform/win32/installation_security.hpp"
#include <functional>
#include <optional>

namespace sfx::platform::win32::installation {
// Production resolves these paths from Windows, never command-line input.
// An explicit location lets native qualification use only disposable roots.
struct DoorComponentLocation {Path image,records;};
DoorComponentLocation system_door_component_location(); // empty on native x64
Descriptor shared_component_descriptor();
inline constexpr const char* door_component_compatibility="SFXEXEC-PRIVATE20-21-1";
inline constexpr const char* qualified_door_driver_sha256="D36589BDE11134B7F6019C32667B639BC465CA39CD39E22FEB6E053A2996A3F3";
struct DoorComponentPolicy {
    std::string incoming_sha256;
    std::vector<std::string> compatible_predecessors;
    // Must prove every *other* registered consumer is stopped before replacing
    // a known owned predecessor. No callback means replacement is refused.
    std::function<bool(const Receipt&)> stopped_consumer;
};
// Holds one cross-process exclusive file lock throughout the surrounding Setup
// transaction. Journal and previous image survive process loss. Destructor
// never guesses whether Setup committed: finish or rollback is explicit.
class DoorComponentChange final {
    struct Impl;std::unique_ptr<Impl> impl_;
    explicit DoorComponentChange(std::unique_ptr<Impl>);
public:
    ~DoorComponentChange();
    DoorComponentChange(const DoorComponentChange&)=delete;
    DoorComponentChange& operator=(const DoorComponentChange&)=delete;
    static std::unique_ptr<DoorComponentChange> prepare(const DoorComponentLocation&,
        Receipt consumer,const std::vector<std::uint8_t>& image,const DoorComponentPolicy&,
        void* protected_descriptor,bool remove=false);
    const DoorComponentIdentity& identity() const;
    bool image_retained() const;
    void commit();
    void rollback();
    // Recovery decision comes from exact protected installation receipts, not
    // a timer or the mere presence of a process/file. Unknown state stays held.
    static void recover(const DoorComponentLocation&,void* protected_descriptor,
        const std::function<std::optional<bool>(const Receipt&,bool remove)>& committed);
};
} // namespace sfx::platform::win32::installation
