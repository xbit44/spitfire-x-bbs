#include <fstream>
#include <string_view>
#include <unistd.h>
int main(int argc,char** argv){
    if(geteuid()==0||argc!=2||std::string_view(argv[1])!="127.0.0.1")return 1;
    {std::ofstream marker("admission-entered");marker<<getpid();marker.close();if(!marker)return 2;}
    for(;;)pause();
}
