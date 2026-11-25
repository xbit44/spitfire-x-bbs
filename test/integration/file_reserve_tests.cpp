#include "sfx/platform/win32/file_payload_store.hpp"
#include <iostream>
#include <stdexcept>
namespace native=sfx::platform::win32;
namespace ins=native::installation;
int main(int argc,char** argv){unsigned checks{};try{
    const auto check=[&](bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);};
    check(argc==2,"fresh reserve fixture required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"retain previous reserve fixtures");
    const auto owner=ins::operator_sid();const auto security=ins::mutable_descriptor(owner);ins::create_directory(root,security.get());
    constexpr std::uint64_t room=1024*1024,maximum=700*1024;
    auto directory=native::FilePayloadDirectory::private_store(root,owner,[]{return native::kFilePayloadFreeReserve+room;});
    check(directory.available_bytes()==native::kFilePayloadFreeReserve+room,"fault ceiling only reduces real free bytes; no disk filling");
    auto first=directory.create("first.tmp",maximum);auto inherited=first.duplicate_transfer_handle(true);check(static_cast<bool>(inherited),"real reduced inherited-write capability acquired");
    bool refused{};
    try{auto second=directory.create("second.tmp",maximum);std::cout<<"OBSERVED: two simultaneous receive maxima="<<2*maximum<<" bytes, allowed room="<<room<<" bytes; both files still empty, no helper/process/write run\n";}
    catch(const std::exception& e){const std::string code=e.what();refused=code=="file_free_space_reserve"||code=="file_budget_insufficient_space";if(!refused)throw;}
    check(refused,"aggregate receive reservation missing: concurrent inherited writers can overcommit the reserve");
    inherited.reset();first.identity();
    std::cout<<"PASS native receive reserve "<<checks<<" checks; small real empty files/reduced handle and fault availability ceiling, not actual disk-full or Runtime/client acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native receive reserve after "<<checks<<": "<<e.what()<<'\n';return 1;}}
