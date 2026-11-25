#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <unistd.h>
int main(int argc,char** argv){
    if(std::string(argv[0]).find("hang")!=std::string::npos){for(;;)std::this_thread::sleep_for(std::chrono::seconds(1));}
    if(argc==2&&std::strcmp(argv[1],"--version")==0){std::puts("DOSEMU2 test fixture (not an emulator)");return 0;}
    const char message[]="SFX SELECTED DOS FIXTURE\r\n";return write(1,message,sizeof(message)-1)==sizeof(message)-1?0:1;
}
