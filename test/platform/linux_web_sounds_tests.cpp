#include "linux_web_sounds.hpp"
#include "sfx/application/operational_configuration.hpp"
#include "sfx/application/operational_log.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace web=sfx::apps::runtime::web;
namespace app=sfx::application;
namespace fs=std::filesystem;
namespace {unsigned checks{};void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void put16(std::vector<std::uint8_t>& b,std::size_t at,unsigned n){b[at]=static_cast<std::uint8_t>(n);b[at+1]=static_cast<std::uint8_t>(n>>8);}
void put32(std::vector<std::uint8_t>& b,std::size_t at,unsigned n){put16(b,at,n);put16(b,at+2,n>>16);}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"normal user and fresh fixture required");const fs::path root=argv[1];check(root.is_absolute()&&!fs::exists(root),"fresh absolute fixture");fs::create_directories(root);check(chmod(root.c_str(),0755)==0,"normal sound folder");
    std::vector<std::uint8_t> wave(8044,128);std::copy_n("RIFF",4,wave.begin());put32(wave,4,8036);std::copy_n("WAVEfmt ",8,wave.begin()+8);put32(wave,16,16);put16(wave,20,1);put16(wave,22,1);put32(wave,24,8000);put32(wave,28,8000);put16(wave,32,1);put16(wave,34,8);std::copy_n("data",4,wave.begin()+36);put32(wave,40,8000);
    std::uint64_t duration{};check(web::pcm_sound(wave,duration)&&duration==1000,"bounded PCM duration");
    for(const auto at:{0U,4U,8U,20U,22U,24U,28U,32U,34U}){auto bad=wave;bad[at]^=127;check(!web::pcm_sound(bad,duration),"malformed PCM refused");}
    auto oversized=wave;put32(oversized,40,8001);check(!web::pcm_sound(oversized,duration),"data chunk exceeds physical bytes");
    auto short_wave=wave;short_wave.resize(43);check(!web::pcm_sound(short_wave,duration),"short wave refused");
    auto long_wave=wave;long_wave.resize(44+8000*31);put32(long_wave,4,static_cast<unsigned>(long_wave.size()-8));put32(long_wave,40,8000*31);check(!web::pcm_sound(long_wave,duration),"duration bound");
    auto metadata=wave;metadata.insert(metadata.begin()+36,{'L','I','S','T',1,0,0,0,'x',0});put32(metadata,4,static_cast<unsigned>(metadata.size()-8));check(web::pcm_sound(metadata,duration),"bounded metadata accepted");
    for(const auto* path:{"relative.wav","//a.wav","/../a.wav","/a/./b.wav","/a//b.wav","/a.wav/","/a.mp3","/a\nb.wav","/a\\b.wav","C:\\sounds\\a.wav"})check(!web::linux_sound_path(path),"Linux path constraints");
    check(web::linux_sound_path("/home/sysop/My Sound.WAV"),"normal Linux WAV path");check(app::valid_event_sound_path("C:\\sounds\\a.wav"),"Windows configuration still decodes on Linux");
    const auto put=[&](const char* name,const std::vector<std::uint8_t>& data){std::ofstream file(root/name,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));file.close();check(chmod((root/name).c_str(),0644)==0,"normal readable WAV");};
    const auto refused=[&](const fs::path& p){try{(void)web::read_web_sound(p.string());return false;}catch(const std::exception&){return true;}};
    put("event.wav",wave);check(web::read_web_sound((root/"event.wav").string()).bytes==wave,"actual owned WAV bytes");
    put("not-a-wave.wav",{1,2,3});check(refused(root/"not-a-wave.wav"),"not arbitrary file disclosure");
    fs::create_symlink(root/"event.wav",root/"link.wav");check(refused(root/"link.wav"),"symlink refused");
    fs::create_hard_link(root/"event.wav",root/"hard.wav");check(refused(root/"hard.wav"),"hardlink refused");fs::remove(root/"hard.wav");
    check(mkfifo((root/"fifo.wav").c_str(),0600)==0,"FIFO fixture");check(refused(root/"fifo.wav"),"FIFO cannot block");
    chmod((root/"event.wav").c_str(),0664);check(refused(root/"event.wav"),"shared writable WAV refused");chmod((root/"event.wav").c_str(),0644);
    chmod(root.c_str(),0775);check(refused(root/"event.wav"),"shared writable folder refused");chmod(root.c_str(),0755);
    check(refused(root/"missing.wav"),"missing WAV refused");
    const auto a=web::web_sound_owner("session-a",std::string(32,'a')),b=web::web_sound_owner("session-b",std::string(32,'a')),c=web::web_sound_owner("session-a",std::string(32,'b'));
    check(a.valid()&&a!=b&&a!=c,"browser and tab owner isolation");check(a==web::web_sound_owner("session-a",std::string(32,'a')),"stable leased owner");bool invalid=false;try{(void)web::web_sound_owner("session-a","bad");}catch(...){invalid=true;}check(invalid,"invalid owner refused");
    app::EventSoundConfiguration config;config.paths[0]=(root/"event.wav").string();app::EventSoundStream stream(c);app::SoundPollRequest request;request.owner=a;
    stream.publish(app::SoundEvent::telnet_login,100);auto poll=stream.poll(request,config,101);check(poll.owner&&poll.events.empty(),"attach does not replay history");request.epoch=poll.epoch;request.after=poll.through;
    app::OperationalEvent event;event.kind=app::OperationalKind::authenticated;event.context.account_id=1;event.context.transport=app::OperationalTransport::telnet;stream.observe(event,102);poll=stream.poll(request,config,103);check(poll.events.size()==1&&poll.events[0].event==app::SoundEvent::telnet_login&&poll.configuration.paths==config.paths,"typed event delivered once with actual configuration");
    check(stream.poll(request,config,104).events.empty(),"old cursor cannot duplicate sound");request.owner=b;check(!stream.poll(request,config,105).owner,"second consumer denied during lease");
    stream.publish(app::SoundEvent::telnet_logout,106);poll=stream.poll(request,config,6000);check(poll.owner&&poll.events.empty(),"expired lease changes owner without history");request.epoch=poll.epoch;request.after=poll.through;
    stream.publish(app::SoundEvent::security_alert,6001);stream.publish(app::SoundEvent::security_alert,6002);poll=stream.poll(request,config,6003);check(poll.events.size()==1,"alerts throttle repeated facts");
    std::cout<<"PASS "<<checks<<" Linux Event Sounds checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
