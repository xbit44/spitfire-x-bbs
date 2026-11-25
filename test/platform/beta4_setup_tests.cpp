#include <filesystem>
inline std::filesystem::path network_setup_qualification_root(){return L"C:\\Program Files\\SFX-Beta4-Test-20261113-r2";}
#define SFX_NETWORK_SETUP_QUALIFICATION 1
#define SFX_BETA1_SETUP_QUALIFICATION 1
#define SFX_BETA4_SETUP_QUALIFICATION 1
#include "ftn_setup_tests.cpp"
