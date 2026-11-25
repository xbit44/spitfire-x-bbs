// Synthetic native Door; never an installed/default Door or shipped game.
#include <winsock2.h>
#include <windows.h>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
int main(int argc,char** argv){
    const std::string mode=argc>1?argv[1]:"--stdio";
    if(mode=="--cleanup"){if(argc!=3)return 8;std::ofstream file(argv[2],std::ios::binary);file<<"owned cleanup completed\n";return file.good()?0:9;}
    if(mode=="--fail")return 7;
    if(mode=="--wait-fail"){
        if(argc!=3)return 8;const auto deadline=GetTickCount64()+10000;
        while(!std::filesystem::exists(argv[2])&&GetTickCount64()<deadline)Sleep(5);
        return std::filesystem::exists(argv[2])?7:9;
    }
    if(mode=="--wait"){Sleep(30000);return 0;}
    if(mode=="--stderr"){DWORD sent{};const char text[]="DIAGNOSTIC MUST NOT REACH CALLER";WriteFile(GetStdHandle(STD_ERROR_HANDLE),text,sizeof(text)-1,&sent,nullptr);}
    if(mode=="--bbsdev"){
        char path[32768]{};if(!GetEnvironmentVariableA("BBSDEV_DRP",path,sizeof(path)))return 20;
        std::ifstream file(path,std::ios::binary);std::vector<std::string> lines;std::string line;while(std::getline(file,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();lines.push_back(line);}
        if(lines.size()!=19||lines[0]!="1.0"||lines[1]!="stdio"||!lines[2].empty()||lines[3]!="Fixture Caller"||lines[16]!="10"||lines[17]!="2")return 21;
    }
    if(mode=="--remove-drop"){if(argc!=3||!DeleteFileA(argv[2]))return 22;return 0;}
    SOCKET socket=INVALID_SOCKET;WSADATA data{};
    if(mode=="--doordrop-stdio"||mode=="--doordrop-socket"){
        wchar_t path[32768]{};
        const auto count=GetEnvironmentVariableW(L"DOORDROP_INI",path,32768);
        if(!count||count>=32768||argc!=3)return 30;
        const std::filesystem::path ini(path);
        if(!ini.is_absolute()||ini.filename()!=L"DOORDROP.INI"||
           ini.lexically_normal()!=std::filesystem::u8path(argv[2]).lexically_normal())return 31;
        wchar_t value[256]{};
        GetPrivateProfileStringW(L"user",L"USER_ALIAS",L"",value,256,path);
        if(std::wstring(value)!=L"Fixture Caller")return 32;
        GetPrivateProfileStringW(L"user",L"USER_KEY",L"",value,256,path);
        if(std::wstring(value)!=L"sfx-fixture-key")return 33;
        GetPrivateProfileStringW(L"comm",L"COMM_TYPE",L"",value,256,path);
        const bool socket_mode=mode=="--doordrop-socket";
        if(std::wstring(value)!=(socket_mode?L"socket":L"stdio"))return 34;
        if(socket_mode){
            if(WSAStartup(MAKEWORD(2,2),&data))return 35;
            if(!GetPrivateProfileStringW(L"comm",L"COMM_HANDLE",L"",value,256,path))return 36;
            socket=static_cast<SOCKET>(std::stoull(value));
            if(socket==INVALID_SOCKET)return 37;
        }
    }
    if(mode=="--socket"||mode=="--socket-close"||mode=="--socket-contract"){
        if(argc!=3||WSAStartup(MAKEWORD(2,2),&data))return 10;
        if(mode=="--socket-contract"){
            DWORD input_mode{},output_mode{};wchar_t title[256]{};
            if(!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&input_mode)||!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),&output_mode)||!IsWindowVisible(GetConsoleWindow()))return 23;
            if(!GetConsoleTitleW(title,256)||std::wstring(title)!=L"Native Fixture - SFX Node 2")return 24;
            // Windows Terminal owns the visible root; conhost uses the console
            // window itself. Both must start minimized and remain restorable.
            const auto top=GetAncestor(GetConsoleWindow(),GA_ROOTOWNER);
            const auto until=GetTickCount64()+2000;
            while(!IsIconic(top)&&GetTickCount64()<until)Sleep(10);
            if(!IsWindowVisible(top)||!IsIconic(top)||(GetWindowLongW(top,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)||GetWindow(top,GW_OWNER))return 26;
            HANDLE drop=CreateFileA(argv[2],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(drop==INVALID_HANDLE_VALUE)return 25;
            CloseHandle(drop);
        }
        std::ifstream file(argv[2]);std::vector<std::string> lines;std::string line;
        while(std::getline(file,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();lines.push_back(line);}
        if(lines.size()!=11||lines[0]!="2"||lines[4]!="42"||lines[5]!="Fixture Caller"||lines[6]!="Fixture Caller"||lines[7]!="10"||lines[10]!="2")return 11;
        socket=static_cast<SOCKET>(std::stoull(lines[1]));
        if(mode=="--socket-close"){closesocket(socket);Sleep(10000);WSACleanup();return 0;}
    }
    const auto write=[&](const std::string& text){
        if(socket!=INVALID_SOCKET)return send(socket,text.data(),static_cast<int>(text.size()),0)==static_cast<int>(text.size());
        DWORD count{};return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data(),static_cast<DWORD>(text.size()),&count,nullptr)&&count==text.size();
    };
    if(!write("SFX LOCAL READY\r\n"))return 12;
    for(;;){char byte{};DWORD count{};
        if(socket!=INVALID_SOCKET){if(recv(socket,&byte,1,0)<=0)break;}
        else if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),&byte,1,&count,nullptr)||!count)break;
        if(byte=='q'){write("SFX LOCAL RETURN\r\n");break;}
        if(!write(std::string(1,byte)))return 13;
    }
    if(socket!=INVALID_SOCKET){closesocket(socket);WSACleanup();}return 0;
}
