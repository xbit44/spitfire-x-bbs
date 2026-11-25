// I07: actual Setup source preservation refused while its coordinator held an
// existing Data lock. Isolate only the same NTFS rename/child-handle combination
// in a uniquely created repository-build fixture. No ACL, receipt or installed
// object changes; no elevation, arbitrary root input or external process needed.
#include "sfx/platform/win32/installation_security.hpp"
#include <iostream>

namespace install = sfx::platform::win32::installation;
int main() {
    try {
        install::require(!install::elevated(), "fixture must be ordinary user");
        const auto build = std::filesystem::current_path() / L"build" / L"installer-v002";
        install::require(std::filesystem::is_directory(build), "run only from repository with disposable build area");
        const auto identity = install::unique_id();
        const auto fixture = build / (L"rename-lock-" + identity);
        const auto source = fixture / L"source";
        const auto target = fixture / L"preserved";
        install::require(CreateDirectoryW(fixture.c_str(), nullptr) && CreateDirectoryW(source.c_str(), nullptr) &&
            CreateDirectoryW((source / L"Data").c_str(), nullptr), "create new fixture only");
        const auto lock_path = source / L"Data" / L"SFXRuntime.lock";
        install::Handle seed(CreateFileW(lock_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr));
        install::require(seed.get() != INVALID_HANDLE_VALUE, "create exact fixture file");
        seed.reset();
        const auto mutex_name = L"Local\\SFX-Setup-Rename-Fixture-" + identity;
        install::Handle guard(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
        install::require(guard && GetLastError() != ERROR_ALREADY_EXISTS &&
            WaitForSingleObject(guard.get(), 0) == WAIT_OBJECT_0, "retain unique installation-presence guard");
        auto held = install::open_object(lock_path, false, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE);
        const auto first = MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
        const auto error = first ? ERROR_SUCCESS : GetLastError();
        std::cout << "held_child_rename=" << (first ? "success" : "refused") << " win32_error=" << error << '\n';
        held.reset();
        // Same decision used by RuntimeInstanceGuard::acquire: an existing
        // named mutex means refuse startup, regardless of the data-file lock.
        install::Handle other(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
        install::require(other && GetLastError() == ERROR_ALREADY_EXISTS,
            "Runtime-style duplicate detection survives source file-handle retirement");
        if (!first) install::require(MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
            "rename after child-handle retirement must succeed");
        install::require(ReleaseMutex(guard.get()) != FALSE, "release held test mutex after rename");
        other.reset(); guard.reset();
        install::require(DeleteFileW((target / L"Data" / L"SFXRuntime.lock").c_str()) &&
            RemoveDirectoryW((target / L"Data").c_str()) && RemoveDirectoryW(target.c_str()) &&
            RemoveDirectoryW(fixture.c_str()), "remove only exact newly created empty fixture objects");
        std::cout << "SOURCE RENAME PASS: released-child rename, retained Runtime-style presence guard and exact fixture cleanup; failed=0 skipped=0 exit 0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SOURCE RENAME DIAGNOSTIC FAIL: " << error.what() << '\n'; return 1;
    }
}
