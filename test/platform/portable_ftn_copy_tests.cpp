// Reuse the actual native TIC/spool/recovery producer. Then move its original
// tree out of reach and exercise the complete copied journals and payloads.
#define SFX_FTN_PUBLICATION_ENTRY original_native_publication_main
#include "../integration/ftn_native_publication_tests.cpp"
#undef SFX_FTN_PUBLICATION_ENTRY
#include "sfx/platform/win32/portable_credentials.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/ftn_stored_scan.hpp"
#include <map>

int main(int argc,char** argv)try{
    if(argc==4)return original_native_publication_main(argc,argv);
    check(argc==2,"new copied FTN fixture required");
    const auto base=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(base),"preserve previous copied FTN fixtures");
    const auto original=base/L"original",copy=base/L"copied";
    auto original_argument=original.u8string();char* producer[]{argv[0],original_argument.data()};
    check(original_native_publication_main(2,producer)==0,"actual native producer and pending/ACK/TIC recovery qualification passed");
    const auto who=ins::operator_sid();auto descriptor=ins::mutable_descriptor(who);
    check(std::filesystem::exists(original/L"Config"/L"board.key"),"native producer supplied its portable board key prerequisite");
    sfx::test::FakeStoragePathPolicy paths;const app::MessageConfigurationActor owner{true,{}};
    {
        storage::SqliteStorage db(settings(original/L"Data"),paths);check(db.start().succeeded(),"source journal reopened for explicit legacy conversion");
        const auto ftn=db.ftn_configuration(owner);const auto doors=db.door_configuration(owner);
        check(ftn&&ftn.value&&doors&&doors.value,"actual source credential catalogs available");
        const auto converted=native::prepare_portable_credentials(original,who,*ftn.value,*doors.value);
        check(converted.issues.empty(),"all current native FTN credential aliases prepared");
        const auto mail=(original/L"FTN"/L"NetMail").make_preferred();ins::create_directory(mail,descriptor.get());
        ins::create_file(mail/L"attach.bin",{'S','T','O','R','E','D'},descriptor.get());
        std::string message(190,'\0');message.replace(0,4,"Game");message.replace(36,4,"Game");message.replace(72,10,"attach.bin");
        const auto word=[&](std::size_t at,unsigned value){message[at]=static_cast<char>(value);message[at+1]=static_cast<char>(value>>8);};
        word(166,2);word(168,1);word(172,100);word(174,100);word(186,0x191);
        message+="\1INTL 2:100/2 2:100/1\r\1FLAGS KFS\r";message+='\0';
        ins::create_file(mail/L"1.MSG",{message.begin(),message.end()},descriptor.get());
        auto configured=*ftn.value;configured.networks.front().msg_netmail_enabled=true;configured.networks.front().msg_netmail_directory=mail.u8string();
        check(bool(db.change_ftn_configuration(owner,id(990010).bytes,configured)),"configure real contained stored MSG source before copy");
        configured=*db.ftn_configuration(owner).value;ins::Receipt receipt;receipt.root=original;receipt.owner=who;std::string cursor;
        const auto scan=native::scan_ftn_stored_messages(db,configured,receipt,cursor,[]{return true;});
        if(scan.failed)std::cerr<<"stored source: "<<scan.reason<<'\n';
        check(scan.queued==1&&!scan.failed&&std::filesystem::exists(mail/L"1.MSG"),"actual stored attachment queued and retained pending acknowledgement");
        check(db.checkpoint().succeeded()&&db.shutdown().succeeded(),"source journal stopped before consistent copy");
    }
    const auto captured=native::refresh_portable_file_snapshot(original,who);
    check(captured.published&&!captured.deferred,"current native FTN and file namespace completely captured");
    std::map<std::filesystem::path,app::FilePayloadIdentity> proof;
    for(const auto* role:{L"FTN",L"Files",L"Data/FileIntake",L"Data/FileRemoved"})
        for(const auto& entry:std::filesystem::recursive_directory_iterator(original/role))if(entry.is_regular_file()){
            auto directory=native::FilePayloadDirectory::private_store(entry.path().parent_path(),who);
            auto file=directory.read(entry.path().filename().u8string());
            proof.emplace(entry.path().lexically_relative(original),file.identity());
        }
    check(proof.size()>20,"populated native packet/TIC/payload and retained-original fixture");
    const auto database_before=ins::file_sha256(original/L"Data"/L"SFX.DB");
    std::filesystem::copy(original,copy,std::filesystem::copy_options::recursive);
    std::filesystem::rename(original,base/L"original-unavailable");
    native::WindowsStorageDaclPolicy access(who);
    check(access.apply(copy,native::StorageDaclObjectKind::directory).succeeded(),"native copied-host root access");
    for(const auto& entry:std::filesystem::recursive_directory_iterator(copy))check(access.apply(entry.path(),entry.is_directory()?native::StorageDaclObjectKind::directory:native::StorageDaclObjectKind::file).succeeded(),"native copied-host contained access");
    const auto prepared=native::prepare_copied_file_bindings(copy,who);
    check(prepared.published&&!prepared.deferred,"all copied native journal payload proofs rebound");
    check(ins::file_sha256(copy/L"Data"/L"SFX.DB")==database_before,"copy preparation changes no pending/GOT/TIC generation or hold records");
    for(const auto& entry:proof){
        const auto resolved=native::resolve_portable_owned_path(copy,original/entry.first);
        check(resolved==copy/entry.first,"old configured owned pathname resolves with original root absent");
        auto directory=native::FilePayloadDirectory::private_store(resolved.parent_path(),who);
        auto file=directory.read(resolved.filename().u8string());const auto logical=file.identity();const auto raw=file.native_object_identity();
        check(logical.volume==entry.second.volume&&logical.file_id==entry.second.file_id&&logical.bytes==entry.second.bytes&&logical.sha256==entry.second.sha256,"copied pending or retained native bytes preserve exact journal identity");
        check(raw.file_id!=logical.file_id,"actual copied object has a distinct native identity");
    }
    {
        storage::SqliteStorage db(settings(copy/L"Data"),paths);check(db.start().succeeded(),"copied actual FTN journal opens with original unavailable");
        const auto config=db.ftn_configuration(owner);check(config&&config.value,"copied actual FTN configuration");unsigned credentials{};
        for(const auto& link:config.value->links)for(const auto entry:{std::pair{native::FtnCredentialKind::session,&link.credentials.session},std::pair{native::FtnCredentialKind::packet,&link.credentials.packet},std::pair{native::FtnCredentialKind::areafix,&link.credentials.areafix},std::pair{native::FtnCredentialKind::tic,&link.credentials.tic}})if(!entry.second->empty()){
            check(native::unprotect_ftn_secret(copy.u8string(),link.network,link.code,entry.first,*entry.second).has_value(),"actual copied FTN credential decodes portably");
            check(!native::unprotect_legacy_ftn_secret(copy.u8string(),link.network,link.code,entry.first,*entry.second),"copied purpose cannot decode original Windows-bound credential");++credentials;
        }
        check(credentials>=4,"multiple real FTN peers and TIC credentials retained");
        const auto jobs_before=scalar(copy/L"Data"/L"SFX.DB","SELECT count(*) FROM sfx_ftn_outbound");
        ins::Receipt receipt;receipt.root=copy;receipt.owner=who;std::string cursor;
        const auto scan=native::scan_ftn_stored_messages(db,*config.value,receipt,cursor,[]{return true;});
        if(scan.failed)std::cerr<<"stored copy: "<<scan.reason<<'\n';
        check(!scan.failed&&!scan.queued&&!scan.completed&&scalar(copy/L"Data"/L"SFX.DB","SELECT count(*) FROM sfx_ftn_outbound")==jobs_before,"copied stored MSG reuses original pending operation without duplicate queue or premature cleanup");
        check(std::filesystem::exists(copy/L"FTN"/L"NetMail"/L"1.MSG")&&std::filesystem::exists(copy/L"FTN"/L"NetMail"/L"attach.bin"),"copied unacknowledged stored message and attachment remain");
        check(scalar(copy/L"Data"/L"SFX.DB","SELECT count(*) FROM pragma_foreign_key_check")==0,"copied actual pending/ACK/held journals remain relationally consistent");
        check(db.checkpoint().succeeded()&&db.shutdown().succeeded(),"copied journal closes normally");
    }
    check(ins::file_sha256(base/L"original-unavailable"/L"Data"/L"SFX.DB")==database_before,"all copy checks preserve unavailable original database");
    std::cout<<"PASS "<<checks<<" actual native FTN/TIC/ACK/held-work whole-copy identity and credential checks; original pathname absent, synthetic host transport only\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL copied FTN "<<checks<<": "<<error.what()<<'\n';return 1;}
