#include "linux_web_dos_setup.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "LinuxDosSetupResources.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <charconv>
#include <fstream>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
namespace sfx::apps::runtime::web {
namespace {
namespace host=platform::linux_host;using Json=nlohmann::json;namespace fs=std::filesystem;
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void keys(const Json& j,std::initializer_list<const char*> names){need(j.is_object()&&j.size()==names.size(),"dos_setup_request_invalid");for(const auto* n:names)need(j.contains(n),"dos_setup_request_invalid");}
std::string text(const Json& j,std::size_t max=2048){need(j.is_string(),"dos_setup_text_invalid");const auto s=j.get<std::string>();need(s.size()<=max&&std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32||c==127;}),"dos_setup_text_invalid");return s;}
std::uint64_t revision(const Json& j){const auto s=text(j,20);std::uint64_t n{};const auto p=std::from_chars(s.data(),s.data()+s.size(),n);need(p.ec==std::errc{}&&p.ptr==s.data()+s.size()&&std::to_string(n)==s,"dos_setup_revision_invalid");return n;}
bool can_prepare(){
    std::ifstream input("/etc/os-release");std::string line,id,version;while(std::getline(input,line)){if(line=="ID=pop"||line=="ID=\"pop\"")id="pop";if(line=="VERSION_ID=22.04"||line=="VERSION_ID=\"22.04\"")version="22.04";}
    utsname machine{};
    return id=="pop"&&version=="22.04"&&uname(&machine)==0&&std::string(machine.machine)=="x86_64";
}
Json runtime_json(const host::DoorOptions& o){return {{"root",o.dosemu_root.string()},{"binary",o.dosemu_binary.string()}};}
host::DoorOptions options(const Json& j){keys(j,{"root","binary"});host::DoorOptions result;result.dosemu_root=text(j.at("root"));result.dosemu_binary=text(j.at("binary"));return result;}
std::string safe_detail(std::string_view bytes){std::string s;for(unsigned char c:bytes){if(c=='\n'||c=='\r'||c=='\t'||(c>=32&&c!=127))s+=static_cast<char>(c);}if(s.size()>2048)s.erase(0,s.size()-2048);return s;}
std::string setup_message(std::string_view code){
    if(code=="dos_setup_cancelled")return "DOS setup was cancelled. Saved settings are kept. Incomplete preparation files, if any, remain under bin for inspection.";
    if(code=="door_dosemu2_missing_prepare_Bin_Dosemu_or_select_runtime")return "DOSEMU2 was not found. Choose its actual binary, or prepare the separate SFX copy when available.";
    if(code=="door_directory_unavailable")return "A required runtime folder is missing or is a link. Check the selected runtime prefix and its DOS resource folders.";
    if(code=="dos_setup_amd64_elf_required"||code=="door_native_elf_required")return "Choose the actual AMD64 Linux DOSEMU2 executable. A wrapper script, DOS program or writable/privileged executable is not accepted.";
    if(code=="dos_setup_dosemu2_version_not_recognized")return "The program did not identify itself as DOSEMU2. Check the executable and its installed version.";
    if(code=="dos_setup_command_timed_out")return "The DOS support command did not finish in time and was stopped. Check its installation before trying again.";
    if(code.find("dos_setup_command_failed: ")==0)return "The DOS support command could not finish. "+safe_detail(code.substr(26));
    return "DOS support check could not finish: "+safe_detail(code);
}
}
LinuxDosSetup::LinuxDosSetup(fs::path root):root_(std::move(root)){}
LinuxDosSetup::~LinuxDosSetup(){cancel_=true;if(worker_.joinable())worker_.join();}
bool LinuxDosSetup::active()const{std::lock_guard<std::mutex> lock(mutex_);return active_;}
void LinuxDosSetup::progress(std::string_view s){std::lock_guard<std::mutex> lock(mutex_);detail_=safe_detail(s);}
Json LinuxDosSetup::status()const{
    std::lock_guard<std::mutex> lock(mutex_);const auto saved=host::read_dos_runtime_profile(root_);Json candidates=Json::array();
    const auto add=[&](const fs::path& binary,const fs::path& prefix){std::error_code ec;if(!fs::is_regular_file(binary,ec))return;const auto real=fs::canonical(binary,ec);if(ec)return;host::DoorOptions selected;selected.dosemu_root=prefix;selected.dosemu_binary=real;const auto candidate=runtime_json(selected);if(std::find(candidates.begin(),candidates.end(),candidate)==candidates.end())candidates.push_back(candidate);};
    add(root_/"bin/dosemu/usr/libexec/dosemu2/dosemu2.bin",root_/"bin/dosemu");
    for(const auto* p:{"/usr/libexec/dosemu2/dosemu2.bin","/usr/bin/dosemu.bin","/usr/local/libexec/dosemu2/dosemu2.bin"})add(p,{});
    Json result={{"active",active_},{"job",job_},{"ok",ok_},{"checked",bool(checked_)},{"detail",detail_},{"revision",std::to_string(saved.revision)},{"check_revision",std::to_string(expected_revision_)},{"saved",runtime_json(saved.options)},{"saved_identity",saved.options.dosemu_sha256},{"candidates",candidates},{"can_prepare",can_prepare()}};
    if(checked_){result["runtime"]=runtime_json(checked_->options);result["checked_identity"]=checked_->options.dosemu_sha256;result["version"]=safe_detail(checked_->version);}return result;
}
fs::path LinuxDosSetup::prepare_tools(){
    const auto path=root_/"management"/(std::string("dos-setup-")+std::string(dos_resources::identity).substr(0,16));auto parent=host::open_owned_directory(root_/"management");
    if(mkdirat(parent.get(),path.filename().c_str(),0700)!=0)need(errno==EEXIST,"dos_setup_tools_directory_failed");
    auto dir=host::open_owned_directory(path);
    for(const auto& item:{std::pair<std::string,std::string_view>{"prepare_linux_dos.py",dos_resources::helper},{"linux_dos_pop22_amd64.json",dos_resources::profile}}){
        struct stat exists{};const std::vector<std::uint8_t> expected(item.second.begin(),item.second.end());
        if(fstatat(dir.get(),item.first.c_str(),&exists,AT_SYMLINK_NOFOLLOW)==0)need(host::read_private_file(dir.get(),item.first,65536)==expected,"dos_setup_tools_changed");
        else{need(errno==ENOENT,"dos_setup_tools_unavailable");host::publish_private_file(dir.get(),item.first,expected,false);}
    }return path;
}
Json LinuxDosSetup::start(const Json& request){
    std::lock_guard<std::mutex> action_lock(action_mutex_);
    keys(request,{"action","runtime"});const auto action=text(request.at("action"),16);need(action=="check"||action=="prepare","dos_setup_action_invalid");const auto proposed=options(request.at("runtime"));
    {std::lock_guard<std::mutex> lock(mutex_);need(!active_,"dos_setup_already_active");}
    if(worker_.joinable())worker_.join();
    need(action!="prepare"||can_prepare(),"dos_setup_private_profile_not_available_use_installed_dosemu2");
    {std::lock_guard<std::mutex> lock(mutex_);expected_revision_=host::read_dos_runtime_profile(root_).revision;need(sodium_init()>=0,"dos_setup_random_unavailable");std::array<unsigned char,16> random{};randombytes_buf(random.data(),random.size());std::array<char,33> hex{};sodium_bin2hex(hex.data(),hex.size(),random.data(),random.size());job_=hex.data();active_=true;ok_=false;checked_.reset();cancel_=false;detail_=action=="prepare"?"Preparing a separate SFX DOS runtime...":"Checking DOSEMU2 and DOS resources...";}
    try{worker_=std::thread([this,proposed,action]{
        try{
            auto selected=proposed;
            if(action=="prepare"){
                const auto tools=prepare_tools();const fs::path python=fs::canonical("/usr/bin/python3");
                host::run_dos_setup_command(python,{"-I","-B",(tools/"prepare_linux_dos.py").string(),root_.string()},root_,[this]{return cancel_.load();},[this](auto s){progress(s);},900);
                selected={};selected.dosemu_root=root_/"bin/dosemu";
            }
            auto checked=host::check_dos_runtime(root_,selected,[this]{return cancel_.load();});
            std::lock_guard<std::mutex> lock(mutex_);checked_=std::move(checked);ok_=true;detail_="DOSEMU2 and required resources checked. Save this runtime choice, then add a game. A caller session still needs testing.";active_=false;
        }catch(const std::exception& error){std::lock_guard<std::mutex> lock(mutex_);detail_=setup_message(error.what());ok_=false;active_=false;}
    });}catch(...){std::lock_guard<std::mutex> lock(mutex_);active_=false;detail_="Could not start DOS setup work.";throw;}
    return status();
}
Json LinuxDosSetup::cancel(){cancel_=true;return status();}
Json LinuxDosSetup::save(const Json& request){
    std::lock_guard<std::mutex> action_lock(action_mutex_);
    keys(request,{"job","revision"});const auto job=text(request.at("job"),32);const auto expected=revision(request.at("revision"));
    {std::lock_guard<std::mutex> lock(mutex_);need(!active_&&ok_&&checked_&&job==job_&&expected==expected_revision_,"dos_setup_check_required");host::save_dos_runtime_profile(root_,checked_->options,expected);expected_revision_=expected+1;}
    auto result=status();result["detail"]="DOS runtime choice saved. New DOS Door sessions use this checked binary.";return result;
}
Json LinuxDosSetup::inspect_game(const Json& request)const{
    const bool has_interval=request.is_object()&&request.contains("input_interval_ms");
    if(has_interval)keys(request,{"directory","input_interval_ms"});else keys(request,{"directory"});
    std::optional<unsigned> interval;if(has_interval){const auto& value=request.at("input_interval_ms");need(value.is_number_integer()&&value>=0&&value<=100,"dos_setup_input_interval_out_of_range");interval=value.get<unsigned>();}
    const auto checked=host::inspect_dos_game(root_,text(request.at("directory")),interval);
    return {{"directory",checked.directory.string()},{"programs",checked.programs},{"driver_identity",checked.driver_identity},{"driver_change",checked.driver_change},{"input_interval_ms",checked.input_interval_ms},{"driver_preview",checked.driver_change?"Prepare CanDisconnect=false and the chosen DOS input spacing in this game's sfxexec.ini. Other driver settings are preserved.":"Existing driver settings already support the Linux DOS adapter and chosen input spacing."}};
}
Json LinuxDosSetup::prepare_game(const Json& request){
    const bool has_interval=request.is_object()&&request.contains("input_interval_ms");
    if(has_interval)keys(request,{"directory","driver_identity","runtime_revision","input_interval_ms"});else keys(request,{"directory","driver_identity","runtime_revision"});
    std::optional<unsigned> interval;if(has_interval){const auto& value=request.at("input_interval_ms");need(value.is_number_integer()&&value>=0&&value<=100,"dos_setup_input_interval_out_of_range");interval=value.get<unsigned>();}
    const auto expected=revision(request.at("runtime_revision"));
    const auto saved=host::read_dos_runtime_profile(root_);need(saved.revision&&saved.revision==expected,"dos_setup_runtime_settings_changed");
    host::prepare_dos_game_driver(root_,text(request.at("directory")),text(request.at("driver_identity"),64),interval);
    const auto checked=host::inspect_dos_game(root_,text(request.at("directory")));
    return {{"ok",true},{"input_interval_ms",checked.input_interval_ms},{"driver_identity",checked.driver_identity},{"detail","Game DOS driver settings prepared. Game programs were kept."}};
}
void LinuxDosSetup::reset(){std::lock_guard<std::mutex> lock(mutex_);need(!active_,"dos_setup_already_active");checked_.reset();job_.clear();ok_=false;detail_="Check DOS support for the restored board before saving a runtime choice.";}
}
