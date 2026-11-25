#include "fixture_paths.hpp"
#include <filesystem>
inline std::filesystem::path network_setup_qualification_root(){
    return (sfx::test::fixture_checkout() / L"build/backup-permissions-fixtures-20261113-r1/smb-root/installed-board-r2");
}
#define SFX_NETWORK_SETUP_QUALIFICATION 1
#include "ftn_setup_tests.cpp"
