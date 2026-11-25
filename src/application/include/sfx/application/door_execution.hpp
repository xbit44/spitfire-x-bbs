#pragma once
#include "sfx/application/door.hpp"
#include "sfx/application/terminal_stream.hpp"
#include <array>
#include <functional>
#include <memory>
#include <mutex>

namespace sfx::application {
enum class RloginEnd : std::uint8_t;
enum class DoorEnd : std::uint8_t {
    returned, escaped, operator_terminated, caller_disconnected, shutdown,
    authority_lost, calling_time, inactivity, launch_failed, handshake_failed,
    transport_failed, backpressure_timeout, busy, unavailable, game_disconnect
};
DoorEnd door_end(RloginEnd) noexcept;
const char* door_result_code(DoorEnd) noexcept;
struct DoorChannel {
    std::function<InteractiveStreamReadResult(std::size_t)> read;
    std::function<InteractiveStreamWriteResult(std::string_view)> write;
    std::function<std::optional<DoorEnd>()> stop;
    std::function<void()> keyboard;
};
struct DoorProgramResult {
    DoorEnd end{DoorEnd::launch_failed};
    std::uint32_t exit_code{};
    bool cleanup_complete{true}, cleanup_command_failed{};
    std::string diagnostic;
    bool work_preserved{};
    std::string retained_location; // safe relative job-folder description only
};
// Runtime-wide ownership, not a visible node-number lock. A live multiuser
// lease still blocks a later single-user snapshot, and vice versa.
class DoorReservations final {
    struct Slot {unsigned count{};bool exclusive{},quarantined{};};
    struct Shared {std::mutex mutex;std::array<Slot,1000> slots;};
    std::shared_ptr<Shared> shared_{std::make_shared<Shared>()};
public:
    class Lease final {
        std::shared_ptr<Shared> shared_;DoorId id_{};bool released_{};
        friend class DoorReservations;
        Lease(std::shared_ptr<Shared> s,DoorId id):shared_(std::move(s)),id_(id){}
    public:
        Lease(const Lease&)=delete;Lease& operator=(const Lease&)=delete;
        ~Lease();
        void release_after_cleanup() noexcept;
    };
    std::unique_ptr<Lease> acquire(DoorId,bool allow_multiple);
};
struct DoorCommandPaths {std::string node,drop,startup,code;unsigned node_number{};};
// Substitutes only administrator-owned paths/code and the actual node number.
// No caller name/password is interpolated into a command or shell expression.
std::optional<std::string> expand_door_command(std::string_view,const DoorCommandPaths&);
} // namespace sfx::application
