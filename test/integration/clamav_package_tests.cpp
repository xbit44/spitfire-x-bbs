#include "sfx/platform/win32/clamav_installation.hpp"
#include "sfx/platform/win32/clamav_update.hpp"
#include "sfx/archive/archive_engine.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <fstream>
#include <iostream>
namespace ins=sfx::platform::win32::installation;
namespace native=sfx::platform::win32;
namespace {
unsigned checks{};
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}

void check_prepared_component(const ins::Path& zip, const ins::Path& root) {
    check(ins::elevated() && !std::filesystem::exists(root) &&
        root.filename().wstring().rfind(L"beta15-clamav-component-test-", 0) == 0,
        "consented elevated worker and new disposable component root required");
    const auto owner = ins::operator_sid();
    const auto structural = ins::structural_descriptor(owner);
    const auto content = ins::mutable_descriptor(owner);
    for (const auto& directory : {root, root / L"Bin", root / L"Bin" / L"Tools"})
        ins::create_directory(directory, structural.get());
    for (const auto& directory : {root / L"Config", root / L"Config" / L"ClamAV"})
        ins::create_directory(directory, content.get());
    const auto predecessor = ins::unique_id(), current = ins::unique_id();
    native::provision_clamav_engine(root, owner, predecessor, zip);
    const auto engine = root / L"Bin" / L"Tools" / L"ClamAV";
    const auto marker = engine / L"SFX-component.receipt";
    const auto old_marker = ins::read_file(marker, 1024);
    const auto config = root / L"Config" / L"ClamAV" / L"freshclam.conf";
    const ins::Path old_root = L"Z:\\Previous SFX Test Board";
    const auto old_config = native::clamav_update_configuration(old_root);
    const auto current_config = native::clamav_update_configuration(root);
    auto write_config = [&](const std::string& value) {
        ins::create_file(config, {value.begin(), value.end()}, content.get());
        native::WindowsStorageDaclPolicy policy(owner);
        check(policy.apply(config, native::StorageDaclObjectKind::file).succeeded(), "private generated configuration fixture");
    };
    write_config(old_config);

    auto rejects_rebind = [&](const std::wstring& id) {
        try { native::rebind_clamav_receipt(root, owner, current, id, old_root); }
        catch (const std::exception&) { return true; }
        return false;
    };
    check(rejects_rebind(ins::unique_id()) && ins::read_file(marker, 1024) == old_marker &&
        ins::read_file(config, 65536) == std::vector<std::uint8_t>(old_config.begin(), old_config.end()),
        "an unproved predecessor leaves both files byte-exact");
    const auto result = native::rebind_clamav_receipt(root, owner, current, predecessor, old_root);
    check(result.receipt_changed && result.configuration_changed && !result.configuration_preserved,
        "known predecessor rebinds the official engine and exact copied configuration");
    native::verify_clamav_installation(root, owner, current);
    const auto new_marker = ins::read_file(marker, 1024);
    const auto new_config = std::vector<std::uint8_t>(current_config.begin(), current_config.end());
    check(new_marker != old_marker && ins::read_file(config, 65536) == new_config, "live identity and local paths verified");

    // Model retry after the receipt already advanced, with the original path
    // present only in the generated text and the old drive unavailable.
    check(DeleteFileW(config.c_str()) != FALSE, "remove exact disposable configuration fixture");
    write_config(old_config);
    const auto retry = native::rebind_clamav_receipt(root, owner, current, predecessor);
    check(!retry.receipt_changed && retry.configuration_changed && !retry.configuration_preserved &&
        ins::read_file(marker, 1024) == new_marker && ins::read_file(config, 65536) == new_config,
        "confirmed historical repair can finish this exact generated configuration on retry");

    const auto custom = old_config + "# sysop customization\n";
    check(DeleteFileW(config.c_str()) != FALSE, "remove exact disposable configuration fixture");
    write_config(custom);
    const auto preserved = native::rebind_clamav_receipt(root, owner, current, predecessor);
    check(!preserved.receipt_changed && !preserved.configuration_changed && preserved.configuration_preserved &&
        ins::read_file(config, 65536) == std::vector<std::uint8_t>(custom.begin(), custom.end()),
        "custom configuration is preserved byte-for-byte");

    check(CreateHardLinkW((engine / L"fixture-marker-alias").c_str(), marker.c_str(), nullptr) != FALSE,
        "exact linked receipt refusal fixture");
    check(rejects_rebind(predecessor), "linked component receipt cannot be reissued");
    std::ifstream untouched(marker, std::ios::binary);
    const std::vector<std::uint8_t> linked_bytes{std::istreambuf_iterator<char>(untouched), {}};
    check(linked_bytes == new_marker, "refused receipt remains byte-exact");
}

void check_engine_upgrade(const ins::Path& old_zip,const ins::Path& new_zip,const ins::Path& root) {
    check(ins::elevated()&&!std::filesystem::exists(root)&&
        root.filename().wstring().rfind(L"rc2-clamav-upgrade-",0)==0,"new elevated disposable migration fixture");
    const auto owner=ins::operator_sid();
    const auto structural=ins::structural_descriptor(owner), writable=ins::mutable_descriptor(owner);
    const auto bin=root/L"Bin",tools=bin/L"Tools",active=tools/L"ClamAV";
    const auto retained=tools/L"ClamAV-1.4.6-retained";
    for(const auto& path:{root,bin,tools,active})ins::create_directory(path,structural.get());
    for(const auto& path:{root/L"Config",root/L"Data"})ins::create_directory(path,writable.get());
    const auto current=ins::unique_id(),predecessor=ins::unique_id();
    const std::string old_sha="AE0FF986504180B0966170FD058E1C337849174A015B2F7958479A04CDEE5F00";
    check(ins::file_sha256(old_zip)==old_sha,"exact prior package, fixture only");
    for(const auto& old:native::legacy_clamav_program_members()){
        const auto member=sfx::archive::read_zip_member(old_zip,"clamav-1.4.6.win.win32/"+std::filesystem::path(old.name).u8string());
        check(member.result.ok()&&member.bytes.size()==old.bytes&&ins::sha256(member.bytes)==old.sha256,"proved previous image fixture");
        ins::create_file(active/old.name,member.bytes,structural.get());
    }
    std::string predecessor_ascii;
    for(const auto character:predecessor)predecessor_ascii.push_back(static_cast<char>(character));
    const std::string old_receipt="SFX-CLAMAV-1\n"+predecessor_ascii+"\n1.4.6-win32\n"+old_sha+"\n";
    const std::vector<std::uint8_t> old_bytes(old_receipt.begin(),old_receipt.end()), sentinel{'T','E','S','T'};
    ins::create_file(active/L"SFX-component.receipt",old_bytes,structural.get());
    const auto data_sentinel=root/L"Data"/L"fixture-preserve.txt";
    const auto config_sentinel=root/L"Config"/L"fixture-preserve.txt";
    for(const auto& path:{data_sentinel,config_sentinel})ins::create_file(path,sentinel,writable.get());
    check(native::clamav_upgrade_required(root,owner,current,predecessor)&&!native::clamav_upgrade_required(root,owner,current),"classification requires the exact live or proved predecessor ID");
    bool rejected{};
    try{native::provision_clamav_engine(root,owner,current,new_zip);}catch(const std::exception&){rejected=true;}
    check(rejected&&ins::read_file(active/L"SFX-component.receipt",1024)==old_bytes&&!std::filesystem::exists(retained),
        "unsupported lineage refuses migration with the old engine still in place");
    // Simulate an interruption between the two validated publications.
    check(MoveFileExW(active.c_str(),retained.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"closed engine rotation fixture");
    check(native::clamav_upgrade_required(root,owner,current,predecessor),"interrupted migration keeps an explicit upgrade state");
    native::provision_clamav_engine(root,owner,current,new_zip,{},predecessor);
    native::verify_clamav_installation(root,owner,current);
    check(!native::clamav_upgrade_required(root,owner,current)&&ins::read_file(retained/L"SFX-component.receipt",1024)==old_bytes,
        "resumed publication activates only 1.4.7 and preserves the exact old engine receipt");
    native::provision_clamav_engine(root,owner,current,new_zip);
    for(const auto& path:{data_sentinel,config_sentinel})check(ins::read_file(path,16)==sentinel,"retry leaves Config and Data bytes alone");
}
}
int main(int argc,char** argv){try{
    if(argc==5&&std::string_view(argv[1])=="--engine-upgrade"){
        check_engine_upgrade(ins::canonical_root(std::filesystem::absolute(argv[2])),
            ins::canonical_root(std::filesystem::absolute(argv[3])),ins::canonical_root(std::filesystem::absolute(argv[4])));
        std::cout<<"PASS ClamAV engine migration "<<checks<<" checks\n";
        return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--prepared-component") {
        check_prepared_component(ins::canonical_root(std::filesystem::absolute(argv[2])),
            ins::canonical_root(std::filesystem::absolute(argv[3])));
        std::cout << "PASS prepared ClamAV component " << checks << " checks\n";
        return 0;
    }
    check(argc==2||argc==3,"exact official portable package and optional NEW log fixture arguments required");
    const std::string history="SFX-CLAMAV-STATUS-1\n1792316700\n0\nmanual\nclamav_update_vendor_cooldown\n-\n-\n-\n";
    check(native::valid_clamav_update_status({history.begin(),history.end()}),"strict history permits truthful failed manual attempt without invented success");
    const std::string history2="SFX-CLAMAV-STATUS-2\n1792316700\n0\nmanual\nclamav_update_vendor_cooldown\n-\n-\n-\n0\n0\n";
    check(native::valid_clamav_update_status({history2.begin(),history2.end()}),"version2 history preserves truthful zero-success migration");
    std::string header="ClamAV-VDB:20 Oct 2026 00-00 +0000:123:456:90:abcdef:signature:builder:1792454400";header.resize(512,' ');
    const auto metadata=native::clamav_database_header({header.begin(),header.end()});
    check(metadata&&metadata->version==123&&metadata->signatures==456&&metadata->published==1792454400,"bounded definition version/publication parser, no authentication implied");
    for(auto invalid: {std::string("garbage"),std::string(512,'x'),header.substr(0,511)})check(!native::clamav_database_header({invalid.begin(),invalid.end()}),"invalid/truncated header cannot invent version/date");
    header[11]='\x1b';check(!native::clamav_database_header({header.begin(),header.end()}),"header control bytes never reach UI");
    check(native::clamav_change_text(native::ClamAvDefinitionChange::current).find("Already current")!=std::string::npos,"current and changed outcomes are distinct");
    for(const auto& invalid:{std::string{},history+"extra\n",std::string("SFX-CLAMAV-STATUS-1\n1\n2\nmanual\nx\n-\n-\n-\n"),std::string("SFX-CLAMAV-STATUS-1\n1\n0\nmanual\n/path leak\n-\n-\n-\n")})check(!native::valid_clamav_update_status({invalid.begin(),invalid.end()}),"truncated/noncanonical/path/future-success history cannot claim readiness");
    const auto zip=ins::canonical_root(std::filesystem::absolute(argv[1]));native::verify_clamav_package(zip);
    check(std::filesystem::file_size(zip)==native::kClamAvPortableBytes,"official source identity and bounded streaming hash verified");
    const auto inspection=sfx::archive::inspect_zip(zip);check(inspection.result.ok(),"CRC/member inspection");
    for(const auto& m:native::clamav_program_members()){
        const auto member=sfx::archive::read_zip_member(zip,std::string(native::kClamAvPortableArchivePrefix)+std::filesystem::path(m.name).u8string());
        check(member.result.ok()&&member.bytes.size()==m.bytes&&ins::sha256(member.bytes)==m.sha256,"exact bounded native executable member");
    }
    const auto license=sfx::archive::read_zip_member(zip,std::string(native::kClamAvPortableArchivePrefix)+"COPYING.txt");
    check(license.result.ok()&&!license.bytes.empty(),"external license available");
    check(!sfx::archive::read_zip_member(zip,std::string(native::kClamAvPortableArchivePrefix)+"libclamav.dll",1024).result.ok(),"member allocation limit enforced");
    check(!sfx::archive::read_zip_member(zip,"absent.txt").result.ok(),"missing member cannot be success");
    check(!sfx::archive::read_zip_member(zip,"../COPYING.txt").result.ok(),"exact member cannot resolve parent alias");
    const auto configuration=native::clamav_update_configuration(L"C:\\SFX Test");
    check(configuration.find("DatabaseDirectory \"C:\\SFX Test\\Data\\ClamAV\"")!=std::string::npos&&configuration.find("UpdateLogFile \"C:\\SFX Test\\Logs\\ClamAV\\freshclam.log\"")!=std::string::npos&&configuration.find("DatabaseMirror database.clamav.net")!=std::string::npos&&configuration.find("TestDatabases yes")!=std::string::npos,"native quoted Windows paths and official verified definitions policy");
    bool rejected{};try{native::clamav_update_configuration(L"C:\\bad#root");}catch(...){rejected=true;}check(rejected,"configuration path cannot inject comment");
    native::verify_clamav_package(zip);check(true,"official source unchanged, whole SHA256 reverified");
    if(argc==3){
        const auto root=std::filesystem::absolute(argv[2]);check(!std::filesystem::exists(root),"fresh disposable log fixture required");const auto owner=ins::operator_sid();native::WindowsStorageDaclPolicy policy(owner);
        for(const auto& path:{root,root/L"Logs",root/L"Logs"/L"ClamAV"}){std::filesystem::create_directory(path);check(policy.apply(path,native::StorageDaclObjectKind::directory).succeeded(),"private log fixture directory");}
        check(native::read_clamav_update_log(root,owner).missing,"missing log is not invented history");
        const auto path=root/L"Logs"/L"ClamAV"/L"freshclam.log";
        ins::Handle writer(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));check(writer.get()!=INVALID_HANDLE_VALUE,"real concurrent vendor-style writer");
        std::string log(70000,'x');log+="\nrecent row\x1b[31m\r\n";DWORD written{};check(WriteFile(writer.get(),log.data(),static_cast<DWORD>(log.size()),&written,nullptr)&&written==log.size()&&FlushFileBuffers(writer.get()),"write log longer than viewer tail");
        auto view=native::read_clamav_update_log(root,owner);check(view.available&&view.truncated&&view.text.find("recent row?")!=std::string::npos&&view.text.find('\x1b')==std::string::npos&&view.text.size()<=131072,"bounded sanitized shared-read tail while writer remains open");
        const std::string more="still writing\r\n";check(WriteFile(writer.get(),more.data(),static_cast<DWORD>(more.size()),&written,nullptr)&&written==more.size(),"viewer leaves writer usable");
        writer.reset();const auto alias=root/L"Logs"/L"ClamAV"/L"alias.log";check(CreateHardLinkW(alias.c_str(),path.c_str(),nullptr)!=FALSE,"create scoped hardlink negative fixture");check(!native::read_clamav_update_log(root,owner).available,"linked log cannot be exposed");
    }
    std::cout<<"PASS ClamAV package "<<checks<<" checks; native package reader only, not Setup or updater execution\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL ClamAV package after "<<checks<<": "<<e.what()<<'\n';return 1;}}
