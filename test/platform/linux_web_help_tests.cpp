#include "linux_web_help.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace web=sfx::apps::runtime::web;
namespace fs=std::filesystem;
namespace {unsigned checks{};void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"normal user and new fixture required");const fs::path root=argv[1];check(root.is_absolute()&&!fs::exists(root),"new absolute fixture");
    fs::create_directories(root/"docs/sysop/assets");fs::create_directories(root/"docs/sysop/images");
    for(const auto& p:{root,root/"docs",root/"docs/sysop",root/"docs/sysop/assets",root/"docs/sysop/images"})check(chmod(p.c_str(),0700)==0,"private fixture directory");
    const auto put=[&](const fs::path& p,std::string_view text){std::ofstream file(p,std::ios::binary);file<<text;file.close();check(chmod(p.c_str(),0600)==0,"private fixture file");};
    put(root/"docs/sysop/wiki.html","<h1>Owned installed Wiki</h1>");put(root/"docs/sysop/assets/search.js","const installed=true;");
    put(root/"docs/sysop/images/logo.png",std::string("\211PNG\r\n\032\n",8));
    auto value=web::read_help_asset(root,"wiki.html");check(value&&value->content_type=="text/html; charset=utf-8"&&std::string(value->bytes.begin(),value->bytes.end())=="<h1>Owned installed Wiki</h1>","installed HTML exact");
    check(web::read_help_asset(root,"assets/search.js").has_value(),"installed script");check(web::read_help_asset(root,"images/logo.png").has_value(),"PNG signature");
    for(const auto* name:{"../Config/settings.ini","/etc/passwd","assets/../../wiki.html","assets//search.js","assets/.hidden.js","wiki.html/other","wiki.html.txt","wiki%2ehtml","images/logo.svg","data/sfx.db","","assets/search.js\n"})check(!web::read_help_asset(root,name),"unsafe path rejected");
    check(!web::read_help_asset(root,"missing.html"),"missing file explicit");
    fs::create_symlink(root/"docs/sysop/wiki.html",root/"docs/sysop/link.html");check(!web::read_help_asset(root,"link.html"),"file symlink refused");
    fs::create_hard_link(root/"docs/sysop/wiki.html",root/"docs/sysop/hard.html");check(!web::read_help_asset(root,"hard.html"),"hard link refused");
    put(root/"docs/sysop/images/wrong.png","not PNG");check(!web::read_help_asset(root,"images/wrong.png"),"false PNG refused");
    check(mkfifo((root/"docs/sysop/pipe.html").c_str(),0600)==0,"FIFO fixture");check(!web::read_help_asset(root,"pipe.html"),"FIFO rejected without blocking");
    put(root/"docs/sysop/large.html",std::string(1024*1024+1,'x'));check(!web::read_help_asset(root,"large.html"),"bounded file");
    chmod((root/"docs/sysop/assets/search.js").c_str(),0644);check(web::read_help_asset(root,"assets/search.js").has_value(),"normal readable static file accepted");
    for(const auto& p:{root/"docs",root/"docs/sysop",root/"docs/sysop/assets"})check(chmod(p.c_str(),0755)==0,"normal readable resource directory");
    check(web::read_help_asset(root,"assets/search.js").has_value(),"normal copied documentation accepted");
    const auto directory=sfx::platform::linux_host::open_owned_directory(root/"docs/sysop/assets",false);bool private_refused=false;
    try{(void)sfx::platform::linux_host::read_private_file(directory.get(),"search.js",1024);}catch(const std::exception&){private_refused=true;}
    check(private_refused,"private credential reader still refuses public permissions");
    chmod((root/"docs/sysop/assets/search.js").c_str(),0664);check(!web::read_help_asset(root,"assets/search.js"),"group-writable resource refused");
    chmod((root/"docs/sysop/assets/search.js").c_str(),0646);check(!web::read_help_asset(root,"assets/search.js"),"other-writable resource refused");
    chmod((root/"docs/sysop/assets/search.js").c_str(),0644);
    for(const auto& p:{root/"docs",root/"docs/sysop",root/"docs/sysop/assets"}){
        chmod(p.c_str(),0775);check(!web::read_help_asset(root,"assets/search.js"),"each group-writable resource directory refused");chmod(p.c_str(),0755);
    }
    fs::rename(root/"docs/sysop/assets",root/"docs/sysop/kept-assets");fs::create_directory_symlink(root/"docs/sysop/kept-assets",root/"docs/sysop/assets");check(!web::read_help_asset(root,"assets/search.js"),"resource directory symlink refused");
    std::cout<<"PASS "<<checks<<" installed Help asset checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
