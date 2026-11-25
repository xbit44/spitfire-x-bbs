#include "fixture_paths.hpp"
#include "sfx/platform/win32/portable_preparation.hpp"
#include "sfx/platform/win32/portable_installation.hpp"
#include "sfx/platform/win32/portable_key_store.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/clamav_installation.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <fstream>
#include <iostream>
#include <sddl.h>
namespace win=sfx::platform::win32;namespace ins=win::installation;namespace fs=std::filesystem;

void seed_disposable_clamav(const ins::Path& root, const ins::Path& zip) {
    const auto work = (sfx::test::fixture_checkout() / L"build/beta15-cr-windows-r1");
    ins::require(root.parent_path() == work && root.filename().wstring().rfind(L"clamav-donor-test-", 0) == 0,
        "only an explicitly provisioned beta15 CR donor fixture is accepted");
    const auto owner = ins::operator_sid();
    win::OfflineInstallationGuard guard;
    ins::require(guard.acquire(root, owner).succeeded(), "stop the disposable donor fixture first");
    const auto receipt = ins::read_receipt(root, owner);
    ins::verify_structure(receipt);
    ins::require(!win::clamav_installed(root), "donor fixture must have no engine");
    win::provision_clamav_engine(root, owner, receipt.id, zip);
    const auto content = ins::mutable_descriptor(owner);
    const auto directory = root / L"Config" / L"ClamAV";
    ins::create_directory(directory, content.get());
    const auto configuration = win::clamav_update_configuration(root);
    const auto config = directory / L"freshclam.conf";
    ins::create_file(config, {configuration.begin(), configuration.end()}, content.get());
    win::WindowsStorageDaclPolicy policy(owner);
    ins::require(policy.apply(config, win::StorageDaclObjectKind::file).succeeded(), "donor fixture configuration protection");
}

int wmain(int argc,wchar_t** argv){
    std::ofstream log;try{
        if (argc == 4 && std::wstring_view(argv[1]) == L"--seed-disposable-clamav") {
            ins::require(ins::elevated(), "consented test fixture worker only");
            seed_disposable_clamav(ins::canonical_root(argv[2]), ins::canonical_root(argv[3]));
            std::cout << "PASS isolated installed donor contains the pinned ClamAV engine and generated configuration\n";
            return 0;
        }
        ins::require(argc==3&&ins::elevated(),"elevated bounded native fixture only");const auto root=ins::canonical_root(argv[2]);
        ins::require(root.parent_path()==(sfx::test::fixture_checkout() / L"build")&&root.filename().native().rfind(L"section16-preparation-native-r",0)==0,"new owned preparation fixture only");
        const std::wstring mode=argv[1];ins::require(mode==L"interrupt"||mode==L"interrupt-registered"||mode==L"retry"||mode==L"verify","bounded native action only");const auto owner=ins::operator_sid();
        if(mode!=L"retry"&&mode!=L"verify"){
            ins::require(!fs::exists(root),"preserve previous fixtures");auto sd=ins::mutable_descriptor(owner);ins::create_directory(root,sd.get());
            for(const auto* leaf:{L"Bin",L"Bin/Tools",L"Config",L"Data",L"Data/FileIntake",L"Data/FileRemoved",L"Files",L"FTN",L"Logs",L"Work",L"Backup",L"Backup/Recovery",L"Backup/Recovery/Setup-native"})ins::create_directory((root/leaf).make_preferred(),sd.get());
            ins::create_file(root/L"Backup"/L"Recovery"/L"Setup-native"/L"retained.bin",{'K','E','E','P'},sd.get());
            const auto id=ins::unique_id();std::vector<std::uint8_t> identity;for(auto c:id)identity.push_back(static_cast<std::uint8_t>(c));ins::create_file(root/L"Bin"/L"installation.id",identity,sd.get());
            for(const auto* program:ins::programs)ins::create_file(root/L"Bin"/program,{'N','A','T','I','V','E'},sd.get());
            ins::create_file(root/L"Data"/L"preserve.bin",{'K','E','E','P'},sd.get());
            auto r=ins::inspect_generation(root,owner,id,1,true,ins::current_program_count,true);ins::write_portable_installation_input(r);
            ins::require(bool(win::create_portable_board_key(root,owner)),"synthetic portable key");ins::require(win::refresh_portable_file_snapshot(root,owner).published,"initial portable namespace");
        }
        log.open(root/(mode+L".log"),std::ios::binary|std::ios::app);const auto key=ins::read_file(root/L"Config"/L"board.key",128);const auto data=ins::read_file(root/L"Data"/L"preserve.bin",16);
        ins::require(SetCurrentDirectoryW((root/L"Bin").c_str()),"native ordinary launch working directory");
        if(mode!=L"retry"&&mode!=L"verify"){
            bool interrupted=false;try{win::prepare_portable_installation(root,owner,[&]{
                const auto bytes=ins::read_file(root/L"Bin"/L"preparation.pending",8192);std::size_t offset{};for(unsigned n=0;n<3;++n){while(offset<bytes.size()&&bytes[offset]!='\n')++offset;++offset;}ins::require(offset<bytes.size(),"native pending receipt boundary");
                const auto pending=ins::decode_receipt({bytes.begin()+offset,bytes.end()});
                if(mode==L"interrupt-registered"){
                    const auto source=ins::read_portable_installation_input(root);
                    const auto identity=root/L"Bin"/L"installation.id";
                    ins::require(MoveFileExW(identity.c_str(),(root/L"Bin"/(L"native-source-"+pending.id+L".id")).c_str(),MOVEFILE_WRITE_THROUGH),"native retain source identity");
                    std::vector<std::uint8_t> idbytes;for(auto c:pending.id)idbytes.push_back(static_cast<std::uint8_t>(c));auto descriptor=ins::structural_descriptor(owner);ins::create_file(identity,idbytes,descriptor.get());
                    ins::create_receipt(pending,true);
                    ins::write_portable_installation_input(source);
                    throw std::runtime_error("native_preparation_interruption");
                }
                const auto sddl=L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;"+owner+L")";PSECURITY_DESCRIPTOR raw{};ins::require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw,nullptr),"native pending registry security");ins::Descriptor descriptor(raw);SECURITY_ATTRIBUTES sa{sizeof(sa),raw,FALSE};HKEY key{};DWORD disposition{};
                const auto name=L"SOFTWARE\\Spitfire-X BBS.Installation."+pending.id;
                ins::require(RegCreateKeyExW(HKEY_LOCAL_MACHINE,name.c_str(),0,nullptr,REG_OPTION_NON_VOLATILE,KEY_ALL_ACCESS|KEY_WOW64_64KEY,&sa,&key,&disposition)==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY,"native operation-owned empty registry key");RegCloseKey(key);
                throw std::runtime_error("native_preparation_interruption");
            });}catch(const std::exception& e){if(std::string(e.what())!="native_preparation_interruption")throw;interrupted=true;}
            ins::require(interrupted&&fs::exists(root/L"Bin"/L"preparation.pending"),"durable pending operation survives interrupted preparation");
        }else{
            const auto r=mode==L"verify"?ins::read_receipt(root,owner):win::prepare_portable_installation(root,owner);ins::require(!fs::exists(root/L"Bin"/L"preparation.pending")&&win::installation::read_prepared_first_start(root,owner).has_value(),"confirmed preparation retains first explicit start fence");
            ins::require(ins::encode_receipt(ins::read_portable_installation_input(root))==ins::encode_receipt(r),"next-copy portable input matches confirmed host generation");
            const auto m=ins::read_service_registration(root);ins::require(m&&m->id==r.id&&!m->enabled&&ins::query_owned_service(*m).state==SERVICE_STOPPED,"new owned service remains Desktop Disabled and stopped");
            auto structural=ins::structural_descriptor(owner);
            for(const auto* leaf:{L"Backup/Recovery",L"Backup/Recovery/Setup-native",L"Backup/Recovery/Setup-native/retained.bin"}){
                const auto path=(root/leaf).make_preferred();auto object=ins::open_object(path,fs::is_directory(path),READ_CONTROL);
                ins::require(ins::exact_security(object.get(),structural.get()),"copied Setup recovery boundary remains structurally protected");
            }
            ins::require(ins::read_file(root/L"Backup"/L"Recovery"/L"Setup-native"/L"retained.bin",4)==std::vector<std::uint8_t>({'K','E','E','P'}),"protected recovery content unchanged");
            ins::remove_owned_service(root,owner);ins::remove_receipt(r); // only this synthetic fixture's new authority; preserve every file
        }
        ins::require(ins::read_file(root/L"Config"/L"board.key",128)==key&&ins::read_file(root/L"Data"/L"preserve.bin",16)==data,"board key and payload unchanged");log<<"PASS native preparation "<<(mode!=L"retry"?"interruption":"retry and retired test authority")<<"; synthetic programs, no installed UI qualification\n";return 0;
    }catch(const std::exception& e){if(log)log<<"FAIL "<<e.what()<<'\n';std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
