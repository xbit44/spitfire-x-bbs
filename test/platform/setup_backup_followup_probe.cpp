// Engineering timing only: generated compressed-definition-shaped payloads,
// never a valid ClamAV installation or an owner installation.
#include "sfx/archive/archive_engine.hpp"
#include <sodium.h>
#include <windows.h>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
namespace fs=std::filesystem;
namespace arc=sfx::archive;
static std::map<std::string,double> phases;
static auto beginning=std::chrono::steady_clock::now();
void sfx_followup_stage(const char* stage){phases[stage]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-beginning).count();}
static void require(bool ok,const char* name){if(!ok)throw std::runtime_error(name);}
static void put(const fs::path& file,std::uint64_t bytes,bool compressed){
    fs::create_directories(file.parent_path());std::ofstream out(file,std::ios::binary);
    std::array<char,65536> block{};std::uint32_t x=0x12345678;
    for(auto& c:block){x^=x<<13;x^=x>>17;x^=x<<5;c=compressed?static_cast<char>(x):'a';}
    while(bytes){const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(bytes,block.size()));out.write(block.data(),n);bytes-=n;
        // Keep the input incompressible across block boundaries as a compressed
        // CVD would be. Synthetic data only; no network/vendor payload.
        if(compressed)for(auto& c:block){x^=x<<13;x^=x>>17;x^=x<<5;c=static_cast<char>(x);}}
    require(out.good(),"fixture_write");
}
static void full_hash(const fs::path& path){
    std::ifstream in(path,std::ios::binary);crypto_hash_sha256_state state{};crypto_hash_sha256_init(&state);
    std::array<char,65536> bytes{};while(in){in.read(bytes.data(),bytes.size());const auto n=in.gcount();if(n>0)crypto_hash_sha256_update(&state,reinterpret_cast<unsigned char*>(bytes.data()),n);}
    require(!in.bad(),"archive_hash_read");std::array<unsigned char,32> digest{};crypto_hash_sha256_final(&state,digest.data());
}
int wmain(int argc,wchar_t** argv){try{
    require(argc==3,"output_and_definition_MiB_required");const fs::path base=fs::absolute(argv[1]);
    require(base.parent_path().filename()==L"section11-owner-followup-20261023"&&!fs::exists(base),"fresh_scoped_output_required");
    const auto mib=std::stoull(argv[2]);require(mib<=512,"fixture_bound");
    fs::create_directories(base);const auto source=base/L"source";
    put(source/L"Config"/L"settings.txt",2*1024*1024,false);
    if(mib)put(source/L"Data"/L"ClamAV"/L"db"/L"daily.cvd",mib*1024*1024,true);
    put(base/L"selected-file-area.dat",4*1024*1024,true);
    arc::CreateBackupRequest request;request.source_root=source;request.partial_path=base/L"backup.partial";request.destination_path=base/L"backup.zip";
    request.source_overrides.push_back({"SFX-FILE-CONTENTS/1/test.dat",base/L"selected-file-area.dat"});
    request.metadata={"setup-upgrade","0123456789ABCDEF0123456789ABCDEF","1.0.0-alpha.2","1",{"20261023-120000","2026-10-23T12:00:00","+00:00","2026-10-23T12:00:00Z"}};
    request.cancelled=[](){return false;};
#ifdef SFX_FOLLOWUP_UPDATED
    request.compression_level=1;request.store_clamav_definitions=true;
    bool writing{},verifying{},publishing{};
    request.backup_progress=[&](const arc::BackupProgressEvent& event){
        if(event.stage==arc::BackupStage::writing&&!writing){sfx_followup_stage("inventory_hashed");sfx_followup_stage("writing_begin");writing=true;}
        if(event.stage==arc::BackupStage::verification&&!verifying){sfx_followup_stage("writing_flushed");verifying=true;}
        if(event.stage==arc::BackupStage::publication&&!publishing){sfx_followup_stage("partial_verified");publishing=true;}
    };
#endif
    phases.clear();beginning=std::chrono::steady_clock::now();
    const auto result=arc::create_backup(request);require(result.ok(),result.detail.c_str());sfx_followup_stage("created");
    const auto inspected=arc::inspect_backup(request.destination_path);require(inspected.result.ok(),"published_full_verification");sfx_followup_stage("published_verified");
    require(inspected.files.size()==(mib?3U:2U),"complete_contents");
    // Baseline retention's repeated digest/inspection sequence, without its ACL,
    // ownership markers, dependency policy or deletion. Those need native tests.
    for(unsigned i=0;i<3;++i){const auto prior=base/(L"retained-"+std::to_wstring(i)+L".zip");fs::copy_file(request.destination_path,prior);
        for(unsigned pass=0;pass<2;++pass){full_hash(prior);require(arc::inspect_backup(prior).result.ok(),"retained_full_verification");}}
    sfx_followup_stage("retained_read_sequence");
    std::ofstream report(base/L"timings.json");report<<"{\"definition_mib\":"<<mib<<",\"archive_bytes\":"<<fs::file_size(request.destination_path)<<",\"phases_ms\":{";
    bool first=true;for(const auto& p:phases){if(!first)report<<',';first=false;report<<'\"'<<p.first<<"\":"<<p.second;}report<<"}}\n";
    std::cout<<"PASS generated backup timing and contents; definition MiB="<<mib<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
