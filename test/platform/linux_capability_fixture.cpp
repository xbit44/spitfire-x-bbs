#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cerrno>

int main(int argc,char** argv){
    const bool helper=argc==3&&std::string(argv[1])=="--ptr";
    if(geteuid()==0)return 1;
    std::ifstream input("/proc/self/status");std::string line,report;
    while(std::getline(input,line))if(line.rfind("Uid:",0)==0||line.rfind("Gid:",0)==0||line.rfind("Cap",0)==0||line.rfind("NoNewPrivs:",0)==0)report+=line+'\n';
    if(report.find("CapEff:")==report.npos)return 2;
    unsigned port{};{std::ifstream file("capability-probe-port");file>>port;}
    if(port<900||port>=1024)return 5;
    const auto socket_fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(socket_fd<0)return 6;
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(static_cast<std::uint16_t>(port));
    const auto bound=bind(socket_fd,reinterpret_cast<sockaddr*>(&address),sizeof(address));const auto error=errno;close(socket_fd);
    report+="ProbePort: "+std::to_string(port)+"\nBindResult: "+std::to_string(bound)+"\nBindErrno: "+std::to_string(error)+'\n';
    {std::ofstream file(helper?"capability-helper.txt":"capability-door.txt");file<<report;file.close();if(!file)return 3;}
    if(helper){std::cout<<"fixture.example\n";return 0;}
    std::cout<<"CAPABILITY DOOR READY\r\n"<<std::flush;
    char byte{};while(std::cin.get(byte))if(byte=='q'){std::cout<<"CAPABILITY DOOR RETURN\r\n"<<std::flush;return 0;}
    return 4;
}
