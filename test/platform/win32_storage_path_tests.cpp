#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "path tests require an existing local directory");
        const sfx::platform::win32::WindowsLocalStoragePathPolicy policy;
        const std::filesystem::path local = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(local);
        const auto accepted = policy.assess(local);
        require(accepted.status && accepted.canonical_directory.is_absolute(), "fixed NTFS path accepted");
        require(!policy.assess(std::filesystem::path("relative-storage")).status, "relative path rejected");
        require(!policy.assess(std::filesystem::path(L"\\\\server\\share\\SFX")).status, "UNC path rejected");
        require(!policy.assess(std::filesystem::path(L"\\\\?\\UNC\\server\\share\\SFX")).status,
                "extended UNC path rejected");
        sfx::platform::win32::WindowsSecureIdentifierGenerator identifiers;
        const auto first = identifiers.generate();
        const auto second = identifiers.generate();
        require(first.valid() && second.valid() && first != second,
                "Windows system RNG generates nonzero distinct 128-bit storage identifiers");
        std::filesystem::remove(local);
        std::cout << "all SFX Win32 storage path tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Win32 storage path test failure: " << error.what() << '\n';
        return 1;
    }
}
