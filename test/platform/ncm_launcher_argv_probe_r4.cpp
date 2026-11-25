// TEST ONLY: closed r4 launcher argv/cwd/NCMAIL proof, never NCM or UI input.
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 64;
    const wchar_t* env=_wgetenv(L"NCMAIL");if(!env)return 65;
    const std::filesystem::path profile(env),packet(argv[1]);
    const std::filesystem::path base(L"C:\\SFX-Reader-Test-r4\\Profiles");
    if(profile!=base/L"Wrap-Off"&&profile!=base/L"Wrap-On")return 66;
    if(std::filesystem::current_path()!=profile||packet!=profile/L"down"/L"SFX863DF.QWK"||!std::filesystem::is_regular_file(packet))return 67;
    if(std::filesystem::exists(profile/L"LAUNCHER-ARGV-TEST-ONLY.txt"))return 68;
    std::ofstream out(profile/L"LAUNCHER-ARGV-TEST-ONLY.txt");out<<"TEST ONLY; NCM NOT EXECUTED\nargc=2\nNCMAIL="<<profile.string()<<"\ncwd="<<std::filesystem::current_path().string()<<"\npacket="<<packet.string()<<"\n";
    std::cout<<"PASS TEST-ONLY r4 launcher argv/cwd/process NCMAIL; NOT NCM startup"<<std::endl;return 0;
}
