#include "ftn_current_installed_client.inc"
#include "network_setup_fixture.hpp"
int main(int argc,char**)try{
 check(argc==1,"closed ColorFix caller host takes no installation override");
 const auto root=ins::canonical_root(network_setup_qualification_root());check(!ins::elevated(),"ordinary owner only");
 RuntimeSession runtime(root);std::cout<<"CALLER_READY"<<std::endl;
 std::string command;check(bool(std::getline(std::cin,command))&&command=="STOP","explicit end of isolated caller test");runtime.stop();std::cout<<"PASS shipping caller host stopped normally"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}
