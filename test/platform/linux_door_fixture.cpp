#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <dirent.h>
int main(int argc,char** argv){
    const std::string mode=argc>1?argv[1]:"";int input=0,output=1;
    if(mode=="cleanup"){if(argc!=3)return 31;std::ofstream(argv[2])<<"cleaned";return 0;}
    if(mode=="fail")return 23;
    if(mode=="wait"){for(;;)std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    if(mode=="socket"||mode=="close"){input=output=3;}
    if(mode=="close"){close(3);std::this_thread::sleep_for(std::chrono::seconds(10));return 0;}
    if(mode=="bbsdev"){const char* path=getenv("BBSDEV_DRP");if(!path)return 32;std::ifstream drop(path);std::string line;std::getline(drop,line);if(line!="1.0\r")return 33;}
    if(mode=="ini"){
        const char* path=getenv("DOORDROP_INI");if(!path||path[0]!='/')return 42;
        std::ifstream file(path,std::ios::binary);const std::string text((std::istreambuf_iterator<char>(file)),{});
        if(text.empty()||text.size()>32768||text.find("FILE_TIME=")==text.npos||text.find("USER_KEY=")==text.npos)return 43;
        const auto capture="INI BEGIN\r\nPATH="+std::string(path)+"\r\n"+text+"INI END\r\n";
        if(write(output,capture.data(),capture.size())!=static_cast<ssize_t>(capture.size()))return 44;
    }
    DIR* descriptors=opendir("/proc/self/fd");if(!descriptors)return 34;while(auto* e=readdir(descriptors)){const int fd=std::atoi(e->d_name);if(fd>2&&fd!=dirfd(descriptors)&&!(fd==3&&input==3)){closedir(descriptors);return 35;}}closedir(descriptors);
    const std::string ready="LINUX READY\r\n";if(write(output,ready.data(),ready.size())!=static_cast<ssize_t>(ready.size()))return 36;
    const std::string diagnostic="PRIVATE DIAGNOSTIC\n";if(write(2,diagnostic.data(),diagnostic.size())!=static_cast<ssize_t>(diagnostic.size()))return 41;
    if(mode=="bulk"){const std::string bytes(131072,'X');std::size_t at{};while(at<bytes.size()){const auto n=write(output,bytes.data()+at,bytes.size()-at);if(n<=0)return 37;at+=static_cast<std::size_t>(n);}}
    for(;;){char c{};if(read(input,&c,1)!=1)return 38;if(c=='q')break;if(write(output,&c,1)!=1)return 39;}
    const std::string end="LINUX RETURN\r\n";return write(output,end.data(),end.size())==static_cast<ssize_t>(end.size())?0:40;
}
