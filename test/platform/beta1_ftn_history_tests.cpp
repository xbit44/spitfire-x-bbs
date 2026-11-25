// Real stopped Windows installation, SQLite transactions and file capabilities.
// All fault/state edits apply only to this new synthetic test board.
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/portable_key_store.hpp"
#include "sfx/platform/win32/ftn_native_session.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include <sodium.h>
#include <iostream>
namespace app=sfx::application;
namespace win=sfx::platform::win32;
namespace ins=win::installation;
namespace store=sfx::adapters::storage;
namespace sql=store::internal;
namespace {
unsigned checks{};
void check(bool yes,const char* reason){++checks;if(!yes)throw std::runtime_error(reason);}
template<class F>bool refused(F function){try{function();return false;}catch(const std::exception&){return true;}}
app::Identifier128 id(unsigned number){app::Identifier128 value;value.bytes[0]=0xb1;value.bytes[14]=static_cast<std::uint8_t>(number>>8);value.bytes[15]=static_cast<std::uint8_t>(number);return value;}
std::int64_t scalar(const ins::Path& path,const char* query){sql::SqliteConnection db(path,SQLITE_OPEN_READONLY,std::chrono::milliseconds(500),false);return db.scalar_int64(query,"inspect owned synthetic recovery state");}
void edit(const ins::Path& path,const char* query){sql::SqliteConnection db(path,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(500),false);db.exec(query,"explicit fault in exact disposable recovery board");}
}
int wmain(int argc,wchar_t** argv)try{
    check(argc==2&&!ins::elevated(),"normal Windows owner and exact new test board required");
    const auto root=ins::canonical_root(argv[1]);check(root==ins::Path(L"C:\\Program Files\\SFX-Beta1-Test-20261112-r1"),"only beta.1 disposable installation");
    const auto owner=ins::operator_sid();ins::verify_runtime_installation(root,owner);
    const auto receipt=ins::read_receipt(root,owner);ins::verify_ftn_directory(receipt);
    const auto file=root/L"Data"/L"SFX.DB";check(!std::filesystem::exists(file),"seed only fresh unconfigured test database");
    win::WindowsLocalStoragePathPolicy paths(owner);store::SqliteStorageSettings settings;settings.data_directory=root/L"Data";
    const std::string content="exact synthetic pending FTN bytes\r\n";
    app::FtnOutboundJob pending;pending.id=id(1);pending.operation=id(2);pending.network="TEST";pending.link="PEER";pending.file={"pending.pkt",content.size(),1700000000};
    crypto_hash_sha256(pending.digest.data(),reinterpret_cast<const unsigned char*>(content.data()),content.size());
    const auto name=win::ftn_native_spool_name(pending);
    {
        win::OfflineInstallationGuard guard;check(bool(guard.acquire(root,owner)),"fresh board held stopped exclusively");
        check(bool(win::load_portable_board_key(root))||bool(win::create_portable_board_key(root,owner)),"fresh explicit key initialization");
        store::SqliteStorage db(settings,paths);check(bool(db.start())&&bool(db.shutdown()),"normal native store initialized and closed");
        check(win::refresh_portable_file_snapshot(root,owner,true).published,"verified fresh file namespace initialized");
        check(bool(db.start()),"prepared native store opens");
        auto configuration=*db.ftn_configuration({true,{}}).value;
        app::FtnNetwork network;network.code="TEST";network.name="Synthetic recovery";network.address="2:100/1@test";network.enabled=true;configuration.networks.push_back(network);
        app::FtnLink link;link.network="TEST";link.code="PEER";link.name="Offline synthetic peer";link.address="2:100/2@test";link.host="127.0.0.1";link.enabled=true;configuration.links.push_back(link);
        check(bool(db.change_ftn_configuration({true,{}},id(3).bytes,configuration)),"normal FTN configuration saved");
        configuration=*db.ftn_configuration({true,{}}).value;
        auto held=pending;held.id=id(4);held.operation=id(5);held.file.name="held.pkt";
        check(bool(db.queue_ftn_jobs({true,{}},configuration.revision,{pending,held})),"pending and separate held fixture jobs queued");
        check(bool(db.preserve_ftn_serial_history({true,{}},app::FtnSerialState{100,true,id(6)})),"verified higher prior floor retained");
        check(bool(db.shutdown()),"native writer settled before recovery");
        edit(file,"UPDATE sfx_ftn_outbound SET state=4,reason='expected_held' WHERE wire_name='held.pkt';UPDATE sfx_ftn_serial_allocator SET recovery_required=1");
        auto descriptor=ins::mutable_descriptor(owner);
        for(const auto& path:{root/L"FTN"/L"Outbound",root/L"FTN"/L"Outbound"/L"TEST"})if(!std::filesystem::exists(path))ins::create_directory(path,descriptor.get());
    }
    check(refused([&]{(void)win::preview_ftn_history_recovery(root,owner);}),"missing required queued file refuses guided recovery");
    check(scalar(file,"SELECT recovery_required FROM sfx_ftn_serial_allocator")==1&&scalar(file,"SELECT count(*) FROM sfx_ftn_serial_recoveries")==0,"refused preview preserves hold and creates no receipt");
    {
        win::OfflineInstallationGuard guard;check(bool(guard.acquire(root,owner)),"normal stopped ownership available after refused preview");
        (void)win::configure_portable_file_bindings(root);
        auto directory=win::FilePayloadDirectory::private_store(root/L"FTN"/L"Outbound"/L"TEST",owner);
        {auto payload=directory.create(name,content.size());payload.append(reinterpret_cast<const std::uint8_t*>(content.data()),content.size());check(payload.identity().sha256==pending.digest,"matching required native payload created");}
        check(win::refresh_portable_file_snapshot(root,owner).published,"new synthetic payload namespace durably captured");
    }
    {
        auto preview=win::preview_ftn_history_recovery(root,owner);
        check(preview->preview().state.recovery_required&&preview->preview().pending_outbound==1&&preview->preview().held_outbound==1,"native preview verifies pending bytes and retains separate held item");
        win::OfflineInstallationGuard competitor;check(!competitor.acquire(root,owner),"preview excludes competing Runtime/maintenance ownership");
        check(!preview->execute("unconfirmed").receipt,"wrong preview confirmation never commits");
    }
    check(scalar(file,"SELECT recovery_required FROM sfx_ftn_serial_allocator")==1,"cancelled preview does not clear history hold");
    const auto payload_hash=ins::file_sha256(root/L"FTN"/L"Outbound"/L"TEST"/std::filesystem::u8path(name));
    const auto external=root/L"FTN"/L"External",busy=external/L"fixture.bsy";
    auto descriptor=ins::mutable_descriptor(owner);if(!std::filesystem::exists(external))ins::create_directory(external,descriptor.get());
    ins::create_file(busy,{'b','u','s','y'},descriptor.get());
    check(refused([&]{(void)win::preview_ftn_history_recovery(root,owner);}),"known independently launched stock-writer busy fence refuses mutation");
    check(DeleteFileW(busy.c_str()),"remove only this test's exact synthetic busy fence");
    ins::Path report;
    {
        auto recovery=win::preview_ftn_history_recovery(root,owner);
        report=root/L"Logs"/std::filesystem::u8path("FTN-Recovery-"+recovery->confirmation_token()+".log");
        ins::create_file(report,{'O','W','N','E','R'},descriptor.get());
        const auto result=recovery->execute(recovery->confirmation_token());
        check(result.status&&result.receipt&&!result.report_written,"database recovery commits while conflicting report is retained/pending");
        check(result.receipt->old_floor==100&&result.receipt->new_floor==100&&result.receipt->held_outbound==1,"native receipt preserves retained floor and separate held count");
        check(ins::read_file(report,100)==std::vector<std::uint8_t>({'O','W','N','E','R'}),"report conflict never overwrites owner content");
    }
    check(scalar(file,"SELECT recovery_required FROM sfx_ftn_serial_allocator")==0&&scalar(file,"SELECT count(*) FROM sfx_ftn_serial_recoveries")==1,"durable native receipt and history clear published once");
    check(DeleteFileW(report.c_str()),"remove only owned test conflict after its bytes are verified");
    ins::create_file(busy,{'b','u','s','y'},descriptor.get());
    {
        auto retry=win::preview_ftn_history_recovery(root,owner);check(!retry->preview().state.recovery_required,"retry reads already committed history");
        const auto result=retry->execute(retry->confirmation_token());
        check(result.status&&result.receipt&&result.report_written&&result.receipt->generation==1,"report-only retry works despite later stock busy fence and does not recover again");
    }
    check(DeleteFileW(busy.c_str()),"retire only this test's later synthetic busy fence");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_serial_recoveries")==1&&scalar(file,"SELECT count(*) FROM sfx_ftn_outbound")==2&&scalar(file,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=4 AND reason='expected_held'")==1,"report retry preserves every queued state and one recovery generation");
    check(ins::file_sha256(root/L"FTN"/L"Outbound"/L"TEST"/std::filesystem::u8path(name))==payload_hash,"required queued payload byte exact after all recovery/report operations");
    std::cout<<"PASS "<<checks<<" stopped installed Windows FTN recovery checks; board remains stopped\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}
