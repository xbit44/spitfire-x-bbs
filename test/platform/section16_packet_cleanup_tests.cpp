// Exact separate shipping helpers and real native jobs/files; decoded caller
// channels and final host completion are synthetic. Writer integration has its
// own sealed-spool test. This is not installed/physical/reader acceptance.
#define SFX_QWK_HOST_SUPPORT_ONLY
#include "fixture_paths.hpp"
#include "qwk_transfer_host_tests.cpp"
#include <map>

namespace fs=std::filesystem;
using Snapshot=std::map<fs::path,std::vector<std::uint8_t>>;
Snapshot inventory(const fs::path& work){Snapshot out;for(const auto& entry:fs::recursive_directory_iterator(work))if(entry.is_regular_file())out.emplace(entry.path().lexically_relative(work),inst::read_file(entry.path(),20U*1024U*1024U));return out;}
bool preserves(const Snapshot& before,const Snapshot& after){for(const auto& item:before){const auto found=after.find(item.first);if(found==after.end()||found->second!=item.second)return false;}return true;}
int wmain(int argc,wchar_t** argv)try{
    check(argc==4,"new contained root and exact packet/message helpers required");const auto root=inst::canonical_root(argv[1]);
    check(root.parent_path()==(sfx::test::fixture_checkout() / L"build")&&root.filename().native().rfind(L"section16-packet-cleanup-r",0)==0&&!fs::exists(root),"fresh bounded packet cleanup fixture only");
    const auto owner=inst::operator_sid();const auto mutable_sd=inst::mutable_descriptor(owner),structural=inst::structural_descriptor(owner);
    for(const auto& directory:{root,root/L"Bin",root/L"Bin"/L"Tools",root/L"Work"})inst::create_directory(directory,mutable_sd.get());
    const auto packet=inst::read_file(argv[2],4U*1024U*1024U),message=inst::read_file(argv[3],4U*1024U*1024U);
    check(inst::sha256(packet)=="073DF149ED5D973CF30C89F900EDBC81DB8BEF28B81F56B5B0742E238C35334D"&&inst::sha256(message)=="E884051DC99FA4D56DF3770CD2680EC40A22C32048440354CCE8D4E9E0D36FE8","actual qualified helper bytes only");
    inst::create_file(root/L"Bin"/L"Tools"/L"SFXQWKTransfer.exe",packet,structural.get());inst::create_file(root/L"Bin"/L"Tools"/L"sexyz.exe",message,structural.get());
    inst::create_file(root/L"Work"/L"foreign.tmp",{'K','E','E','P'},mutable_sd.get());
    protect(root/L"Bin"/L"Tools",structural.get());protect(root/L"Bin",structural.get());protect(root,structural.get());
    WSADATA ws{};check(WSAStartup(MAKEWORD(2,2),&ws)==0,"native Winsock");struct End{~End(){WSACleanup();}} end;
    auto notices=std::make_shared<std::vector<std::pair<std::string,std::string>>>();
    const sfx::apps::runtime::WorkProblemObserver notice=[notices](std::string resource,std::string code){notices->emplace_back(std::move(resource),std::move(code));};
    auto retained=inventory(root/L"Work");const auto zip=rep_zip();
    for(const auto chunk:{std::size_t(1),std::size_t(4096)}){
        const auto prior_notices=notices->size();Channel c;c.chunk=chunk;c.tail_coalesced=true;upload(c,zip,"TEST.REP","\r\n");
        {
            sfx::apps::runtime::AlphaQwkPacketTransfer host(root,owner,packet_ports(c),observe(),notice);
            auto result=host.receive(packet_request());check(result.code==app::QwkTransferCode::completed&&result.source&&c.synchronized,"actual reply relay and sealed source complete");
            check(result.source->rewind().succeeded()&&result.source->next().succeeded(),"native sealed source remains usable before host completion");
            check(std::distance(fs::directory_iterator(root/L"Work"),fs::directory_iterator{})>1,"private job stays live before host completion");
            result.source->confirm_completed();
        }
        check(inventory(root/L"Work")==retained&&notices->size()==prior_notices,"confirmed host completion disposes exact job after all source owners release");
    }
    {
        const auto prior_notices=notices->size();Channel c;upload(c,zip);
        {sfx::apps::runtime::AlphaQwkPacketTransfer host(root,owner,packet_ports(c),observe(),notice);auto result=host.receive(packet_request());check(result.source&&result.code==app::QwkTransferCode::completed,"unconfirmed import fixture has valid sealed input");}
        const auto after=inventory(root/L"Work");bool spool{};for(const auto& item:after)if(item.first.filename()==L"reply.spool")spool=true;
        check(after.size()>retained.size()&&preserves(retained,after)&&spool&&notices->size()==prior_notices+1,"abandoned/unconfirmed import retains sealed and envelope evidence with one safe notice");retained=after;
    }
    for(const auto reason:{1,2,3,4}){
        const auto prior_notices=notices->size();Channel c;c.tail_coalesced=true;
        if(reason==1)policy_offer(c,"wrong.exe","4","\r");else if(reason==2)policy_offer(c,"large.REP","16777217","\r\n");else if(reason==3)empty_offer(c,"\n");else upload(c,"not a zip","TEST.ZIP","\r");
        {sfx::apps::runtime::AlphaQwkPacketTransfer host(root,owner,packet_ports(c),observe(),notice);const auto result=host.receive(packet_request());check(result.code==(reason==3?app::QwkTransferCode::cancelled:app::QwkTransferCode::refused)&&!result.source&&c.synchronized,"policy/cancel result stays truthful and synchronized");}
        const auto after=inventory(root/L"Work");
        if(reason==3)check(after==retained&&notices->size()==prior_notices,"verified completed empty caller cancellation disposes only its owned scratch");
        else check(after.size()>retained.size()&&preserves(retained,after)&&notices->size()==prior_notices+1,"failed/refused packet retains its evidence and leaves earlier failures exact");
        retained=after;
    }
    {
        const auto prior_notices=notices->size();Channel c;c.wire=transfer("Uploaded message text\r\n");
        sfx::apps::runtime::AlphaMessageSourceReceiver host(root,owner,c.ports(),observe(),notice);const auto result=host.receive(request());
        check(result.code==app::MessageReceiveCode::completed&&result.source&&inventory(root/L"Work")==retained&&notices->size()==prior_notices,"actual successful message receive disposes only its exact control/payload scratch");
    }
    {
        const auto prior_notices=notices->size();Channel c;c.wire=transfer(std::string("bad\0body",8));
        sfx::apps::runtime::AlphaMessageSourceReceiver host(root,owner,c.ports(),observe(),notice);const auto result=host.receive(request());const auto after=inventory(root/L"Work");
        check(result.code==app::MessageReceiveCode::refused&&after.size()>retained.size()&&preserves(retained,after)&&notices->size()==prior_notices+1,"failed message validation preserves exact job evidence and emits one notice");retained=after;
    }
    for(const auto& entry:*notices)check(!entry.first.empty()&&entry.first.find_first_of("\\/:\r\n")==std::string::npos&&(entry.second=="job_work_unconfirmed_preserved"||entry.second=="job_owned_cleanup_incomplete_preserved"),"notices expose only safe job identifier and fixed code");
    check(inventory(root/L"Work")==retained&&retained.at(fs::path(L"foreign.tmp"))==std::vector<std::uint8_t>({'K','E','E','P'}),"every retained failure and foreign Work file stays byte-exact through later success/cancellation");
    std::cout<<"PASS "<<checks<<" real packet/message cleanup and preserved-evidence checks; synthetic decoded channel, no installed/owner acceptance\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}
