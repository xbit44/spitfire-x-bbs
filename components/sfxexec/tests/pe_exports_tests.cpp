#include "pe_exports.hpp"
#include <iostream>
#include <fstream>
using namespace sfxdos;
static unsigned checks;static void test(bool b){++checks;if(!b)throw std::runtime_error("PE fixture assertion");}
int main(int argc,char** argv){try{
    require(argc==2,"private fixture PE path required");std::ifstream f(argv[1],std::ios::binary);std::string real((std::istreambuf_iterator<char>(f)),{});
    auto names=pe32_exports(real);test(names.count("getAX")&&names.count("MGetVdmPointer")&&names.count("TestRegisters"));test(!names.count("NotAnExport"));
    for(auto input:{std::string{},real.substr(0,63),real.substr(0,127)}){bool rejected=false;try{pe32_exports(input);}catch(...){rejected=true;}test(rejected);}
    auto bad=real;bad[0]=0;bool rejected=false;try{pe32_exports(bad);}catch(...){rejected=true;}test(rejected);
    bad=real;for(unsigned i=60;i<64;++i)bad[i]=char(255);rejected=false;try{pe32_exports(bad);}catch(...){rejected=true;}test(rejected);
    std::cout<<"PASS "<<checks<<" read-only PE checks against real linked fixture; no module load/native claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
