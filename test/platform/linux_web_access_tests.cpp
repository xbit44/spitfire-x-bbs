#include "linux_web_access.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
namespace {
namespace web=sfx::apps::runtime::web;
unsigned checks{};
void check(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
template<class F>void refuses(F action){bool refused{};try{action();}catch(const std::exception&){refused=true;}check(refused,"unsafe or stale web access refused");}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0&&sodium_init()>=0,"normal-user test context");
    const std::filesystem::path root(argv[1]);check(root.is_absolute()&&!std::filesystem::exists(root),"fresh owned fixture");
    check(mkdir(root.c_str(),0700)==0&&mkdir((root/"config").c_str(),0700)==0&&mkdir((root/"management").c_str(),0700)==0,"private fixture created");
    auto settings=web::read_web_access(root);check(!settings.enabled&&settings.approved.empty()&&settings.revision==1,"fresh LAN access disabled with no approved addresses");
    for(const auto* bad:{"","0.0.0.0","0.1.2.3","224.0.0.1","255.255.255.255","169.254.1.1","192.168.1.0/24","*.example","localhost","192.168.001.012","::","ff02::1","fe80::1","::ffff:192.168.1.12","192.168.1.12\n"})refuses([&]{web::web_ip(bad);});
    check(web::web_ip("FD00:0:0:0:0:0:0:12")=="fd00::12","IPv6 canonical exact address");
    settings.enabled=true;settings.address="192.168.1.250";settings.approved={"192.168.1.12","fd00::12"};settings.revision=2;
    auto request=web::web_access_json(settings);check(web::parse_web_access(request).approved==settings.approved,"typed exact ACL preserved");
    for(const auto& bad:nlohmann::json::array({0,"0","01","-1","18446744073709551615"})){auto invalid=request;invalid["revision"]=bad;refuses([&]{web::parse_web_access(invalid);});}
    for(const auto& bad:nlohmann::json::array({0,-1,65536,true,"8443",1.5})){auto invalid=request;invalid["port"]=bad;refuses([&]{web::parse_web_access(invalid);});}
    auto low_port=request;low_port["port"]=443;check(web::parse_web_access(low_port).port==443,"optional low port remains selectable; native bind authority checked separately");
    auto invalid=request;invalid["approved"]=nlohmann::json::array();refuses([&]{web::parse_web_access(invalid);});invalid=request;invalid["approved"].push_back("192.168.1.12");refuses([&]{web::parse_web_access(invalid);});
    invalid=request;invalid["trusted_proxy"]=true;refuses([&]{web::parse_web_access(invalid);});
    web::write_web_access(root,settings,1);check(web::web_access_json(web::read_web_access(root))==request,"private settings saved and read back");refuses([&]{web::write_web_access(root,settings,1);});
    check(web::web_ip_allowed(settings,"192.168.1.12")&&!web::web_ip_allowed(settings,"192.168.1.13")&&web::web_ip_allowed(settings,"FD00::12"),"allowlist uses exact normalized addresses");
    settings.enabled=false;check(!web::web_ip_allowed(settings,"192.168.1.12"),"disabled LAN denies approved address");settings.enabled=true;
    refuses([&]{web::load_web_tls(root,settings);});web::generate_web_tls(root,settings.address,false);const auto first=web::load_web_tls(root,settings);
    check(first.context&&first.fingerprint.size()==95&&first.certificate.find("BEGIN CERTIFICATE")!=std::string::npos,"generated certificate and SHA256 fingerprint loaded");
    check(SSL_CTX_get_min_proto_version(first.context.get())==TLS1_2_VERSION,"TLS minimum version enforced");
    check(web::load_web_tls(root,settings).fingerprint==first.fingerprint,"reopening preserves generated certificate identity");
    refuses([&]{web::generate_web_tls(root,settings.address,false);});
    auto wrong=settings;wrong.address="192.168.1.12";refuses([&]{web::load_web_tls(root,wrong);});
    settings.custom_certificate=true;refuses([&]{web::load_web_tls(root,settings);});settings.custom_certificate=false;
    const auto directory=sfx::platform::linux_host::open_owned_directory(root/"config");
    auto bytes=sfx::platform::linux_host::read_private_file(directory.get(),"web-https-generated.v1",2097152);auto material=nlohmann::json::parse(bytes.begin(),bytes.end());
    const auto write=[&](const char* name,const std::string& text){sfx::platform::linux_host::publish_private_file(directory.get(),name,{text.begin(),text.end()},false);};
    write("supplied-cert.pem",material["certificate"].get<std::string>());write("supplied-key.pem",material["key"].get<std::string>());
    web::import_web_tls(root,settings.address,root/"config/supplied-cert.pem",root/"config/supplied-key.pem");settings.custom_certificate=true;
    check(web::load_web_tls(root,settings).fingerprint==first.fingerprint,"validated custom certificate imported portably");
    check(chmod((root/"config/supplied-key.pem").c_str(),0644)==0,"fixture public key permissions set");refuses([&]{web::import_web_tls(root,settings.address,root/"config/supplied-cert.pem",root/"config/supplied-key.pem");});
    check(web::load_web_tls(root,settings).fingerprint==first.fingerprint,"failed import preserves previous custom identity");
    web::generate_web_tls(root,"192.168.1.12",true);wrong.custom_certificate=false;const auto replaced=web::load_web_tls(root,wrong);check(replaced.fingerprint!=first.fingerprint,"explicit replacement creates reviewed new identity");
    check(web::load_web_tls(root,settings).fingerprint==first.fingerprint,"generated replacement leaves custom identity intact");
    for(const auto* bad:{"*.example.test","https://bbs.example.test","bbs.example.test:8443","-bbs.example.test","bbs..example.test","bbs.example.test.","bbs example.test"})refuses([&]{web::web_server_name(bad);});
    check(web::web_server_name("BBS.Example.Test")=="bbs.example.test","configured DNS identity normalized separately from numeric bind address");
    web::generate_web_tls(root,"bbs.example.test",true);auto dns=settings;dns.custom_certificate=false;dns.server_name="bbs.example.test";const auto generated_dns=web::load_web_tls(root,dns);
    const auto dns_bytes=sfx::platform::linux_host::read_private_file(directory.get(),"web-https-generated.v1",2097152);const auto dns_material=nlohmann::json::parse(dns_bytes.begin(),dns_bytes.end());
    write("dns-cert.pem",dns_material["certificate"].get<std::string>());write("dns-key.pem",dns_material["key"].get<std::string>());
    web::import_web_tls(root,"bbs.example.test",root/"config/dns-cert.pem",root/"config/dns-key.pem");dns.custom_certificate=true;
    check(web::load_web_tls(root,dns).fingerprint==generated_dns.fingerprint,"custom DNS-SAN certificate accepted with separate numeric listener bind");
    dns.server_name="other.example.test";refuses([&]{web::load_web_tls(root,dns);});dns.server_name.clear();refuses([&]{web::load_web_tls(root,dns);});
    dns.server_name="bbs.example.test";check(web::load_web_tls(root,dns).fingerprint==generated_dns.fingerprint,"DNS mismatch refuses without replacing custom identity");
    struct stat info{};check(stat((root/"config/web-https-generated.v1").c_str(),&info)==0&&(info.st_mode&0777)==0600,"generated private key owner-only");
    sodium_memzero(bytes.data(),bytes.size());auto& key=material["key"].get_ref<std::string&>();sodium_memzero(key.data(),key.size());
    std::cout<<"PASS "<<checks<<" Web Access and TLS identity checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
