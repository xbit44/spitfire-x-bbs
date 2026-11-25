// Synthetic normal-menu qualification child. Never installed as a default Door.
#include <winsock2.h>
#include <windows.h>
#include <fstream>
#include <string>
#include <vector>
int main(int argc,char** argv){
    const std::string mode=argc>1?argv[1]:"--stdio";
    if(mode=="--cleanup"){if(argc!=3)return 2;std::ofstream f(argv[2],std::ios::app);f<<"cleanup\n";return f.good()?0:3;}
    if(mode=="--event"){if(argc!=3)return 4;const std::string s="SFX EVENT "+std::string(argv[2])+"\r\n";DWORD count{};return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data(),static_cast<DWORD>(s.size()),&count,nullptr)&&count==s.size()?0:5;}
    if(mode=="--wait"){Sleep(30000);return 0;}
    if(mode=="--exit7")return 7;
    SOCKET peer=INVALID_SOCKET;WSADATA data{};
    if(mode=="--socket"||mode=="--socket-close"){
        if(argc!=4||WSAStartup(MAKEWORD(2,2),&data))return 10;
        std::ifstream f(argv[2]);std::vector<std::string> rows;std::string line;
        while(std::getline(f,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();rows.push_back(line);}
        if(rows.size()!=11||rows[0]!="2"||std::stoull(rows[4])==0||rows[5]!=argv[3]||rows[6]!=argv[3]||rows[7]!="10"||std::stoul(rows[10])<1||std::stoul(rows[10])>4)return 11;
        peer=static_cast<SOCKET>(std::stoull(rows[1]));
        if(mode=="--socket-close"){closesocket(peer);Sleep(10000);WSACleanup();return 0;}
    }
    const auto write=[&](const std::string& s){if(peer!=INVALID_SOCKET)return send(peer,s.data(),static_cast<int>(s.size()),0)==static_cast<int>(s.size());DWORD n{};return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data(),static_cast<DWORD>(s.size()),&n,nullptr)&&n==s.size();};
    if(!write("SFX LOCAL READY\r\n"))return 12;
    for(;;){char c{};DWORD n{};if(peer!=INVALID_SOCKET){if(recv(peer,&c,1,0)<=0)break;}else if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),&c,1,&n,nullptr)||!n)break;
        if(c=='q'){write("SFX LOCAL RETURN\r\n");break;}if(!write(std::string(1,c)))return 13;}
    if(peer!=INVALID_SOCKET){closesocket(peer);WSACleanup();}return 0;
}
