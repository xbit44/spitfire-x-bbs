// Read-only access-right probe of the existing disposable UI6 installation.
// No ACL writes, file writes, creates, deletes, renames or installed-root access.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

bool access(const std::filesystem::path& path, DWORD desired, const char* label) {
    SetLastError(ERROR_SUCCESS);
    const HANDLE handle = CreateFileW(path.c_str(), desired,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const DWORD error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    std::cout << label << ": " << (error == ERROR_SUCCESS ? "GRANTED" : "DENIED")
              << " (Win32 " << error << ")\n";
    require(error == ERROR_SUCCESS || error == ERROR_ACCESS_DENIED,
        "unexpected error, not an access-right result");
    return error == ERROR_SUCCESS;
}

int main() {
    try {
        const auto root = std::filesystem::current_path() / "build" / "ui6-work" / "gui-root";
        require(std::filesystem::is_regular_file(std::filesystem::current_path() /
            "test" / "platform" / "consolidation_bin_access_probe.cpp"),
            "run only from this engineering workspace");
        for (auto path = root / "Bin";; path = path.parent_path()) {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            require(attributes != INVALID_FILE_ATTRIBUTES &&
                !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "missing/reparse fixture path");
            if (path.parent_path() == path) break;
        }

        HANDLE token = nullptr;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), "read own token");
        TOKEN_ELEVATION elevation{};
        DWORD bytes = 0;
        const bool ordinary = GetTokenInformation(token, TokenElevation, &elevation,
            sizeof(elevation), &bytes) && !elevation.TokenIsElevated;
        (void)GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<unsigned char> user(bytes);
        const bool user_read = bytes && GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes);
        CloseHandle(token);
        require(ordinary && user_read, "probe requires ordinary non-elevated user");
        std::cout << "Token elevated: 0\n";

        auto native = (root / "Bin").native();
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        require(GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
            &owner, nullptr, nullptr, nullptr, &descriptor) == ERROR_SUCCESS, "read Bin owner");
        const bool same_owner = owner && EqualSid(owner,
            reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
        LocalFree(descriptor);
        require(same_owner, "fixture Bin owner must equal ordinary process user");
        std::cout << "Bin owner equals process user: 1\n";

        const bool direct_write = access(root / "Bin", FILE_ADD_FILE, "Bin add file");
        const bool bin_dacl = access(root / "Bin", WRITE_DAC, "Bin change permissions");
        const bool bin_delete = access(root / "Bin", DELETE, "Bin delete/rename access");
        const bool root_delete_child = access(root, FILE_DELETE_CHILD, "Root delete-child access");
        const bool root_add_child = access(root, FILE_ADD_SUBDIRECTORY, "Root add-subdirectory access");
        const bool root_dacl = access(root, WRITE_DAC, "Root change permissions");
        require(!direct_write && bin_dacl && bin_delete && root_delete_child && root_add_child && root_dacl,
            "expected baseline access pattern not reproduced; inspect rather than infer");
        std::cout << "REPRODUCED: ordinary writes denied, owner ACL/parent replacement rights granted.\n"
                     "This is a security-gate conflict, NOT a secure-deployment PASS.\n"
                     "No permission, file content or directory was changed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Probe inconclusive: " << error.what() << '\n';
        return 1;
    }
}
