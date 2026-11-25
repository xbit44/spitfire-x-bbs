#include "../../src/apps/runtime/sysop_log_reader.hpp"
#define main retained_sysop_session_main
#include "../unit/sysop_session_tests.cpp"
#undef main
#include <sstream>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace rt=sfx::apps::runtime;
namespace fs=std::filesystem;
void write(const fs::path& p,const std::string& s){std::ofstream f(p,std::ios::binary);f<<s;check(bool(f),"fixture write");}
int main(int argc,char** argv){try{
    check(argc==2,"new evidence root required");const auto root=fs::absolute(argv[1]);check(!fs::exists(root),"preserve previous evidence");fs::create_directories(root/"Logs/ClamAV");
    const auto keep=[]{return true;};
    check(rt::read_sysop_log(root,app::SysopLog::callers,{},25,80,keep).notice=="Current log is missing.","missing current source is explicit");
    std::string log;for(unsigned n=0;n<1000;++n)log+="Record "+std::to_string(n)+"\r\n";write(root/"Logs/CALLERS.LOG",log);
    auto page=rt::read_sysop_log(root,app::SysopLog::callers,{},25,80,keep);check(page.page==49&&page.previous&&!page.next&&page.lines.front()=="Record 980"&&page.lines.back()=="Record 999","latest full-file page exceeds bounded tail");
    page=rt::read_sysop_log(root,app::SysopLog::callers,0,25,80,keep);check(page.lines.size()==20&&page.lines.front()=="Record 0"&&page.next&&!page.previous,"oldest records reachable");
    page=rt::read_sysop_log(root,app::SysopLog::callers,30,25,80,keep);check(page.lines.front()=="Record 600"&&page.lines.back()=="Record 619","middle page exact");
    page=rt::read_sysop_log(root,app::SysopLog::callers,{},25,80,[]{return false;});check(page.lines.empty()&&page.notice.find("cancelled")!=std::string::npos,"cancel and authority loss discard result");
    write(root/"Logs/HEYSYSOP.LOG","hello\x1b[31m\a\r\npartial");page=rt::read_sysop_log(root,app::SysopLog::heysysop,{},25,80,keep);check(page.lines.size()==1&&page.lines[0].find('\x1b')==std::string::npos&&page.lines[0].find('\a')==std::string::npos&&page.notice.find("Incomplete")!=std::string::npos,"inert controls and partial writer record");
    write(root/"Logs/bink.log",std::string(17000,'x')+"\n");page=rt::read_sysop_log(root,app::SysopLog::bink,{},25,80,keep);check(!page.lines.empty()&&page.lines.back().find("[line truncated]")!=std::string::npos&&page.notice.find("bounded")!=std::string::npos,"overlong line bounded truthfully");
    bool rotated=false;page=rt::read_sysop_log(root,app::SysopLog::callers,{},25,80,[&]{if(!rotated){rotated=true;fs::rename(root/"Logs/CALLERS.LOG",root/"Logs/CALLERS.retained");write(root/"Logs/CALLERS.LOG","new log\n");}return true;});check(page.lines.empty()&&page.notice.find("replaced")!=std::string::npos,"rotation never merges old and new files");
    page=rt::read_sysop_log(root,app::SysopLog::callers,{},25,80,keep);check(page.lines==std::vector<std::string>{"new log"},"explicit refresh sees replacement");
    write(root/"Logs/SFXRuntime.log","unclassifiable legacy record\n");page=rt::read_sysop_log(root,app::SysopLog::botgate,{},25,80,keep);check(page.lines.empty()&&page.notice.find("could not be classified")!=std::string::npos,"BotGate never displays raw untyped records");
    check(fs::file_size(root/"Logs/CALLERS.retained")==log.size(),"reader preserves original file bytes");
    write(root/"Logs/ClamAV/freshclam.log","");page=rt::read_sysop_log(root,app::SysopLog::freshclam,{},25,80,keep);check(page.lines.empty()&&page.notice.find("Current log has no matching complete records.")!=std::string::npos,"empty source differs from missing source");
    app::OperationalContext context;context.epoch.bytes[0]=1;context.origin_sequence=1;context.attempt=1;context.transport=app::OperationalTransport::telnet;context.source_ip="192.0.2.1";context.diagnostic_code="manual_ip_rule";
    const auto bot=app::make_operational_entry(1,1793836800,{app::OperationalKind::botgate_blocked,{},context});
    auto system_context=context;system_context.attempt=0;system_context.transport=app::OperationalTransport::none;system_context.source_ip.clear();system_context.diagnostic_code.clear();
    const auto system=app::make_operational_entry(2,1793836801,{app::OperationalKind::runtime_started,{},system_context});
    const auto bot_record=app::encode_operational_record(bot),system_record=app::encode_operational_record(system);check(!bot_record.empty()&&!system_record.empty(),"real typed event encodings");
    write(root/"Logs/SFXRuntime.log",system_record+"\n"+bot_record+"\n");page=rt::read_sysop_log(root,app::SysopLog::botgate,{},25,80,keep);check(!page.lines.empty()&&page.lines.front().find("SFXLOG")==std::string::npos&&page.lines.front().find("SFXLOG")==std::string::npos,"typed BotGate view excludes System events and raw storage records");
    bool appended=false;page=rt::read_sysop_log(root,app::SysopLog::callers,{},25,80,[&]{if(!appended){appended=true;std::ofstream f(root/"Logs/CALLERS.LOG",std::ios::app|std::ios::binary);f<<"appended record\n";}return true;});check(page.lines==std::vector<std::string>{"new log"},"concurrent append stays outside captured read boundary");
    page=rt::read_sysop_log(root,app::SysopLog::callers,{},25,80,keep);check(page.lines.size()==2&&page.lines.back()=="appended record","explicit refresh includes complete appended record");
    const auto compact=[](const std::string& text){std::string value;for(unsigned char c:text)if(c!=' '&&c!='\r'&&c!='\n')value+=static_cast<char>(c);return value;};
    const auto wrapped=app::wrap_sysop_log_text("Record | File sample.zip | Signature Example-Signature | upload rejected.",40);
    check(wrapped.size()>1&&wrapped[1].substr(0,2)=="  ","continuation lines are indented");
    check(std::any_of(wrapped.begin(),wrapped.end(),[](const auto& row){return row.find("File")!=row.npos;}),"short words are never split at a screen boundary");
    check(compact(std::string(150,'x'))==compact([&]{std::string joined;for(const auto& row:app::wrap_sysop_log_text(std::string(150,'x'),40))joined+=row;return joined;}()),"overlong tokens retain every byte across wraps");
    const std::vector<std::pair<unsigned,unsigned>> sizes{{40,15},{80,25},{132,40}};
    const char* names[]={"HEYSYSOP.LOG","CALLERS.LOG","bink.log","ClamAV/freshclam.log","SFXRuntime.log"};
    for(unsigned source=0;source<5;++source){
        std::string source_bytes,expected;
        for(unsigned n=0;n<41;++n){
            std::string record;
            if(source==4){
                context.origin_sequence=n+1;
                const auto entry=app::make_operational_entry(n+1,1793836800,{app::OperationalKind::botgate_blocked,{},context});
                record=app::encode_operational_record(entry);
                expected+=app::render_operational_line(entry,std::string("2026-11-05 00:00:00 UTC"));
            }else{
                record="Entry "+std::to_string(n)+" | Node 1 | SSH | File sample.zip | Signature Example-Signature | A deliberately long log record that must wrap on words and keep the command prompt visible.";
                expected+=record;
            }
            source_bytes+=record+"\r\n";
        }
        const auto path=root/"Logs"/names[source];write(path,source_bytes);
        for(const auto& [columns,rows]:sizes){
            const auto kind=static_cast<app::SysopLog>(source);
            const auto latest=rt::read_sysop_log(root,kind,{},rows,columns,keep);
            check(latest.page>0&&!latest.next&&latest.previous,"wrapped records span navigable pages");
            std::string reconstructed;
            for(std::uint64_t n=0;n<=latest.page;++n){
                const auto part=rt::read_sysop_log(root,kind,n,rows,columns,keep);
                check(part.page==n&&part.previous==(n>0)&&part.next==(n<latest.page),"screen-row pages have exact previous/next boundaries");
                const auto used=app::wrap_sysop_log_text(part.title,columns,false).size()+1+part.lines.size()+
                    app::wrap_sysop_log_text(part.notice,columns,false).size()+app::wrap_sysop_log_text(app::kSysopLogControls,columns,false).size()+1;
                check(used<=rows,"title log footer and prompt fit actual screen height");
                for(const auto& row:part.lines){check(row.size()<columns,"every emitted log row fits terminal width");reconstructed+=row;}
            }
            check(compact(reconstructed)==compact(expected),"all record text remains ordered with no duplicates or omissions across pages");
            check(fs::file_size(path)==source_bytes.size(),"viewer leaves source log bytes intact");
            for(const bool ansi:{false,true}){
                Fixture view(ansi);view.columns=columns;view.rows=rows;
                unsigned reads{};
                view.ports.log=[&](app::SysopLog requested,std::optional<std::uint64_t> n){++reads;return rt::read_sysop_log(root,requested,n,view.rows,view.columns,keep);};
                const char keys[]={'H','C','B','F','A'};
                const std::string key(1,keys[source]);
                check(view.run({"L",key.c_str(),"P","N","R","Q","Q","Q"}),"actual log controller supports previous next refresh and back");
                check(reads==4&&view.terminal.wire.output.find("Press <ENTER>")==std::string::npos,"screen paging avoids a second generic pause");
                if(!ansi)check(view.terminal.wire.output.find('\x1b')==std::string::npos,"plain log pages contain no ANSI controls");
                view.save(root/"previews"/(std::to_string(columns)+"x"+std::to_string(rows)),"log-"+std::to_string(source));
            }
        }
    }
    Fixture resized(true);unsigned reads{};
    resized.ports.log=[&](app::SysopLog kind,std::optional<std::uint64_t> n){
        ++reads;if(reads==1){resized.columns=40;resized.rows=15;}
        return rt::read_sysop_log(root,kind,n,resized.rows,resized.columns,keep);
    };
    check(resized.run({"L","H","Q","Q","Q"})&&reads==2,"resize during log read discards obsolete layout and redraws for new size");
    std::cout<<"PASS Sysop log reader: "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL Sysop log reader: "<<e.what()<<'\n';return 1;}}
