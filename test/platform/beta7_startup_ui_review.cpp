// Shipping Control Panel window/resources with clearly synthetic local-control
// peer and trust ports. No real board/service/public listener is started.
#define main retained_beta7_console_tests
#include "beta7_startup_tests.cpp"
#undef main
#include <fstream>
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show){try{
    fixture::ui=GetCurrentThreadId();
    std::array<wchar_t,32768> module{};const auto count=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    if(!count||count>=module.size())return 2;
    const auto directory=std::filesystem::path(std::wstring(module.data(),count)).parent_path();
    fixture::fixture_child=directory/L"sfx_beta7_startup_fixture_child.exe";
    fixture::root=directory/L"startup-polish-ui-fixture-r2";
    if(std::filesystem::exists(fixture::root))return 3;
    std::filesystem::create_directories(fixture::root);
    fixture_configuration(4);
    std::filesystem::create_directories(fixture::root/L"Work");
    std::ofstream lifecycle(fixture::root/L"review-lifecycle.txt");lifecycle<<"synthetic trust/peer; actual CP window/resources\nentry=1\n"<<std::flush;
    fixture::protocol_peer=true;fixture::release_verify=false;fixture::prepare_confirm=false;fixture::dialog_answer=100;
    fixture::release_creation=false;
    std::thread delayed_verifier([]{Sleep(45000);fixture::release_verify=true;Sleep(45000);fixture::release_creation=true;});
    wchar_t empty[]{0};const auto result=retained_beta7_production_entry(instance,nullptr,empty,show);
    lifecycle<<"exit="<<result<<"\nnotices="<<fixture::notices<<"\npreparation_calls="<<fixture::preparation_calls<<"\n";
    const auto length=WideCharToMultiByte(CP_UTF8,0,fixture::last_notice.data(),static_cast<int>(fixture::last_notice.size()),nullptr,0,nullptr,nullptr);
    std::string notice(static_cast<std::size_t>(length),'\0');if(length)WideCharToMultiByte(CP_UTF8,0,fixture::last_notice.data(),static_cast<int>(fixture::last_notice.size()),notice.data(),length,nullptr,nullptr);
    lifecycle<<"last_notice="<<notice<<'\n';lifecycle.flush();
    fixture::release_verify=true;delayed_verifier.join();
    if(fixture::child)CloseHandle(fixture::child);return result;
}catch(...){return 9;}}
