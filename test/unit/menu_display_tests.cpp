#include "sfx/application/display.hpp"
#include <array>
#include <iostream>
#include <map>
#include <stdexcept>
#include <fstream>
#include <filesystem>

namespace app = sfx::application;
using Id = app::TrustedDisplayIdentity;
unsigned checks = 0;
void require(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
struct Reader final : app::ITrustedDisplayAssetReader {
    std::map<std::string,std::string> files;
    std::vector<std::string> requests;
    app::TrustedDisplayAssetReadResult read_asset(std::string_view name, std::size_t maximum) override {
        requests.emplace_back(name);
        auto it = files.find(std::string(name));
        if (it == files.end()) return {app::TrustedDisplayAssetReadCode::not_found,{},{},{}};
        if (it->second.size() > maximum) return {app::TrustedDisplayAssetReadCode::invalid,{},{},{}};
        return {app::TrustedDisplayAssetReadCode::available,it->first,it->first,it->second};
    }
};
std::string text(const app::TrustedDisplaySelectionResult& result) {
    require(result.available(),"display available");
    std::string output;
    for (const auto& op : result.program->terminal_program.operations) {
        if (op.kind == app::TerminalDisplayOperationKind::cp437_text) output += op.bytes;
    }
    return output;
}
std::string sauce(std::string body) {
    std::string metadata(128,'\0');
    metadata.replace(0,7,"SAUCE00");
    auto count = body.size();
    for (unsigned n=0;n<4;++n) metadata[90+n] = static_cast<char>((count>>(8*n))&255U);
    metadata[94]=1; metadata[95]=1; metadata[96]=80; metadata[98]=25;
    return body + '\x1a' + metadata;
}
int main(int argc, char** argv) {
    try {
        constexpr std::array<Id,11> menus{{Id::menu_main,Id::menu_messages,Id::menu_files,
            Id::menu_doors,Id::menu_bulletins,
            Id::menu_door_bulletins,Id::menu_chat,Id::menu_settings,Id::menu_file_groups,Id::menu_file_areas,Id::menu_batch_transfer}};
        for (auto id : menus) for (auto graphics : {app::GraphicsMode::ascii,app::GraphicsMode::ansi}) {
            Reader reader;
            app::TrustedDisplaySelector selector(reader);
            app::TrustedDisplaySelectionRequest request;
            request.identity=id; request.graphics=graphics; request.authenticated_security_level=std::uint16_t{10};
            const std::string base(app::trusted_menu_basename(id));
            const std::string extension=graphics==app::GraphicsMode::ansi?".ANS":".ASC";
            reader.files[base+extension]="generic";
            reader.files[base+"999"+extension]="wrong level";
            require(text(selector.select(request))=="generic","unsuffixed fallback not nearest level");
            reader.files[base+"10"+extension]="exact";
            auto selected=selector.select(request);
            if(id==Id::menu_bulletins)
                require(text(selected)=="generic" && !selected.program->exact_security_match,"regular menu stays unsuffixed for all callers");
            else require(text(selected)=="exact" && selected.program->exact_security_match,"exact overrides generic");
            auto forged=*selected.program;
            forged.selected_logical_asset=base+"010"+extension;
            require(!app::valid_rendered_trusted_display_program(forged),"noncanonical suffix rejected");
            reader.files[base+"10"+extension]="@PASSWORD@";
            require(text(selector.select(request))=="generic","invalid exact asset falls back safely");
            request.authenticated_security_level=std::uint16_t{0};
            require(!selector.select(request).available(),"level zero rejected");
            request.authenticated_security_level.reset();
            require(!selector.select(request).available(),"menu cannot be served unauthenticated");
            request.authenticated_security_level=std::uint16_t{10};
            reader.files.clear();
            require(!selector.select(request).available(),"missing art signals generated fallback");
        }
        Reader reader;
        app::TrustedDisplaySelector selector(reader);
        app::TrustedDisplaySelectionRequest request;
        request.identity=Id::menu_main; request.authenticated_security_level=std::uint16_t{10};
        request.context.name="@CLS@";
        request.context.time_on="3661"; request.context.time_left="Unlimited";
        request.context.date="2026-09-27"; request.context.time="13:02:03";
        request.context.file_size="1048576";
        reader.files["MAIN.ASC"]="[@NAME@][@TIMEON:HMS@][@TIMEON:SHORT@][@TIMELEFT:MIN@]"
            "[@DATE:LONG@][@TIME:12@][@FILESIZE:AUTO@][@MSGCOUNT@]";
        require(text(selector.select(request))=="[@CLS@][1:01:01][1h 1m][Unlimited]"
            "[September 27, 2026][1:02:03 PM][1 M][]","typed menu formats and unavailable blank; values inert");
        for (const auto* token : {"SUBDATE","ORGLOG","LOGTIME","QUIET","HOTKEY","UPLOADS","DOWNLOADS",
            "UPK","DOWNK","POSTS","HOSTNAME","MAILTOTAL","MAILUNREAD","ONLINE","UPTIME","MSGCONF",
            "MSGCONFNUM","MSGCOUNT","NEWMSGS","MSGNUM","MSGTO","MSGFROM","MSGSUBJECT","MSGDATE",
            "FILEAREA","FILEAREANUM","FILECOUNT","NEWFILES","FILENAME","FILEDESC","FILEUPLOADER",
            "FILEDATE","FILEDLS","DOORNAME","DOORNUM","DOORMENU","CHATROOM","CHATCOUNT","FTNNET","FTNADDR"}) {
            reader.files["MAIN.ASC"]=std::string("[")+"@"+token+"@]";
            require(text(selector.select(request))=="[]","absent future subsystem value is blank not fabricated");
        }
        for (const auto* invalid : {"@NAME:HMS@","@DATE:MIN@","@FILESIZE:12@","@TIME:LONG@",
            "@PASSWORD@","@BIRTHDATE@","@NAME|L4294967306@","@INCLUDE:../outside.asc@"}) {
            reader.files["MAIN.ASC"]=invalid;
            require(!selector.select(request).available(),"invalid token type/secret/path/overflow rejected");
        }
        reader.files["MAIN.ASC"]="@DATE:LONG@";
        request.context.date="2025-02-29";
        require(!selector.select(request).available(),"invalid calendar date rejected");
        request.context.date="2024-02-29";
        require(text(selector.select(request))=="February 29, 2024","leap day accepted");
        reader.files["MAIN.ASC"]="@NAME@"; request.context.name="evil\x1b[2J";
        require(!selector.select(request).available(),"context cannot inject terminal controls");
        request.graphics=app::GraphicsMode::ansi;
        request.context.name="Caller";
        for(auto mode:{app::GraphicsMode::ansi,app::GraphicsMode::ascii}) {
            request.graphics=mode;
            const auto leaf=mode==app::GraphicsMode::ansi?"MAIN.ANS":"MAIN.ASC";
            reader.files.erase("MAIN.ANS");reader.files.erase("MAIN.ASC");
            reader.files[leaf]=std::string("Legacy text\r\n")+'\x1a';
            require(text(selector.select(request))=="Legacy text","single physical EOF SUB stripped");
            require(reader.files[leaf].back()=='\x1a',"legacy source bytes unchanged");
            reader.files[leaf]=std::string("Embedded")+'\x1a'+"text";
            require(!selector.select(request).available(),"embedded SUB remains invalid");
            reader.files[leaf]=std::string("Repeated")+'\x1a'+'\x1a';
            require(!selector.select(request).available(),"repeated EOF SUB remains invalid");
            reader.files[leaf]=std::string("Not EOF")+'\x1a'+"\r\n";
            require(!selector.select(request).available(),"only physical EOF SUB accepted");
        }
        request.graphics=app::GraphicsMode::ansi;reader.files.erase("MAIN.ASC");
        reader.files["MAIN.ANS"]=sauce("\x1b[31mArt\x1b[0m");
        auto selected=selector.select(request);
        require(text(selected)=="Art","SAUCE excluded from caller output");
        require(selected.program->terminal_program.operations.front().bytes=="\x1b[31m",
            "authored ANSI color preserved");
        reader.files["MAIN.ANS"][reader.files["MAIN.ANS"].size()-38]='\xff';
        require(!selector.select(request).available(),"invalid SAUCE size rejected");
        reader.files["MAIN.ANS"]="@INCLUDE:PART.ANS@";
        reader.files["PART.ANS"]=sauce("Included");
        require(text(selector.select(request))=="Included","bounded SAUCE include");
        reader.files["PART.ANS"]="@INCLUDE:MAIN.ANS@";
        require(!selector.select(request).available(),"menu include cycle rejected");
        request.graphics=app::GraphicsMode::ascii;
        reader.files["MAIN.ASC"]="\x1b[31munsafe";
        require(!selector.select(request).available(),"ASCII never admits raw ANSI");
        request.identity=Id::welcome1; request.authenticated_security_level.reset();
        reader.files["WELCOME1.ASC"]="@ONLINE@";
        require(!selector.select(request).available(),"expanded catalog cannot leak into preauthentication");
        reader.files["WELCOME1.ASC"]=sauce("welcome");
        require(text(selector.select(request))=="welcome","welcome strips validated SAUCE metadata");
        reader.files["WELCOME3.ASC"]="Contact: sysop@example.invalid; @@; @BBSNAME@";
        request.context.bbs_name="Board";request.welcome_number=3;
        require(text(selector.select(request))=="Contact: sysop@example.invalid; @; Board","numbered welcome and literal email with real tokens");
        reader.files["WELCOME3.ASC"]="@PASSWORD@";
        require(!selector.select(request).available(),"numbered welcome still rejects forbidden secret tokens");
        request.welcome_number=100;
        require(!selector.select(request).available(),"welcome sequence bounded at 99");
        request.identity=Id::sfprelog;request.welcome_number=1;
        reader.files["SFPRELOG.ASC"]="Sysop Contact: sysop@example.invalid";
        require(text(selector.select(request))=="Sysop Contact: sysop@example.invalid","owner prelog email remains literal");
        request.identity=Id::menu_main;request.authenticated_security_level=std::uint16_t{999};
        for(auto graphics:{app::GraphicsMode::ascii,app::GraphicsMode::ansi}) {
            request.graphics=graphics;
            const auto name=graphics==app::GraphicsMode::ansi?"MAIN999.ANS":"MAIN999.ASC";
            reader.files[name]="[ @ Sysop Utilities ] @";
            require(text(selector.select(request))=="[ @ Sysop Utilities ] @","bare command at sign is literal in both formats");
            reader.files[name]="@PASSWORD@";
            reader.files.erase(graphics==app::GraphicsMode::ansi?"MAIN.ANS":"MAIN.ASC");
            require(!selector.select(request).available(),"bare-at support cannot enable secret tokens");
            reader.files[name]="@NAME";
            require(!selector.select(request).available(),"unterminated token-like syntax remains invalid");
            reader.files[name]=std::string("Hello")+'\x10'+" world";
            if(graphics==app::GraphicsMode::ansi)
                require(text(selector.select(request))=="Hello world","legacy ANSI DLE is consumed, never emitted");
            else require(!selector.select(request).available(),"ASCII controls remain rejected");
        }
        for(auto graphics:{app::GraphicsMode::ascii,app::GraphicsMode::ansi}) {
            Reader tabs;app::TrustedDisplaySelector tab_selector(tabs);
            app::TrustedDisplaySelectionRequest tab_request;tab_request.identity=Id::sfnewsletter;tab_request.graphics=graphics;
            tabs.files["SFNWSLTR.ASC"]="\tA\tB\r\n12345678\tC\r\n@BBSNAME@\tD";
            tab_request.context.bbs_name="Board";
            require(text(tab_selector.select(tab_request))=="        A       B12345678        CBoard   D","tabs expand to eight-column text stops across lines and tokens");
            tabs.files["SFNWSLTR.ASC"]=std::string(app::kMaximumTrustedDisplaySourceBytes,'\t');
            require(!tab_selector.select(tab_request).available(),"tab expansion retains rendered byte budget");
            tabs.files["SFNWSLTR.ASC"]="Text\x01 still forbidden";
            require(!tab_selector.select(tab_request).available(),"supporting tabs does not permit other control bytes");
        }
        if(argc==3) {
            const auto fixtures=std::filesystem::path(argv[2]);
            for(const auto* filename:{"sfnwsltr.asc","sfnwsltr.asc.ogsf","sfnwsltr.ans.bak"}) {
                std::ifstream owner(fixtures/filename,std::ios::binary);
                const std::string raw((std::istreambuf_iterator<char>(owner)),{});
                require(owner.good() && !raw.empty(),"exact supplied newsletter retained");
                for(auto graphics:{app::GraphicsMode::ascii,app::GraphicsMode::ansi}) {
                    const bool ansi=std::string_view(filename)=="sfnwsltr.ans.bak";
                    Reader exact;app::TrustedDisplaySelector exact_selector(exact);
                    exact.files[ansi?"SFNWSLTR.ANS":"SFNWSLTR.ASC"]=raw;
                    app::TrustedDisplaySelectionRequest exact_request;exact_request.identity=Id::sfnewsletter;exact_request.graphics=graphics;
                    auto exact_selected=exact_selector.select(exact_request);
                    if(ansi && graphics==app::GraphicsMode::ascii) require(!exact_selected.available(),"ASCII never uses owner ANSI-only file");
                    else {
                        require(exact_selected.available(),"exact supplied newsletter selects successfully");
                        const auto owner_text=text(exact_selected);
                        require(app::prepare_acknowledged_display(exact_selected.program->terminal_program,graphics,std::uint8_t{12}),"exact owner file receives bounded newsletter framing");
                        if(std::string_view(filename)=="sfnwsltr.asc.ogsf") {
                            require(owner_text.find("X-Bit BBS:      x-bit.org:23")!=std::string::npos &&
                                owner_text.find("UniX-Bit BBS    x-bit.org:1336")!=std::string::npos,"owner tabbed headings preserve eight-column alignment");
                            require(owner_text.find("xbit.ops@gmail.com")!=std::string::npos,"OGSF literal email preserved");
                        }
                    }
                }
            }
        }
        if(argc>=2) {
            const std::filesystem::path root(argv[1]);
            const auto read=[&](const std::filesystem::path& path) {
                std::ifstream in(path,std::ios::binary);require(in.good(),"exact asset fixture exists");
                return std::string(std::istreambuf_iterator<char>(in),{});
            };
            for(auto graphics:{app::GraphicsMode::ascii,app::GraphicsMode::ansi})
                for(const auto& stock:std::array<std::pair<Id,const char*>,4>{{
                    {Id::menu_main,"main"},{Id::menu_main,"main999"},
                    {Id::menu_messages,"msg"},{Id::menu_files,"file"}}}) {
                    Reader exact_reader;app::TrustedDisplaySelector exact_selector(exact_reader);
                    app::TrustedDisplaySelectionRequest owner;owner.identity=stock.first;owner.graphics=graphics;
                    owner.authenticated_security_level=std::string_view(stock.second)=="main999"?std::uint16_t{999}:std::uint16_t{10};
                    std::string upper=stock.second;
                    for(auto& c:upper)if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');
                    exact_reader.files[upper+(graphics==app::GraphicsMode::ansi?".ANS":".ASC")]=
                        read(root/"distribution/Display"/(std::string(stock.second)+(graphics==app::GraphicsMode::ansi?".ans":".asc")));
                    const auto selected_owner=exact_selector.select(owner);
                    require(selected_owner.available() && !text(selected_owner).empty(),
                        "all eight exact owner revisions select without inventing artwork content");
                    require(text(selected_owner).find(std::string_view(stock.second)=="file"?"Quick Goodbye / Logoff":"Goodbye [/G] Quick")!=std::string::npos,
                        "Main and File owner artwork preserves supplied quick logoff label");
                }
            for(auto graphics:{app::GraphicsMode::ascii,app::GraphicsMode::ansi}) {
                request.graphics=graphics;
                const auto ext=graphics==app::GraphicsMode::ansi?".ans":".asc";
                reader.files[graphics==app::GraphicsMode::ansi?"MAIN999.ANS":"MAIN999.ASC"]=read(root/"distribution/Display"/(std::string("main999")+ext));
                const auto exact=selector.select(request);
                require(exact.available() && exact.program->exact_security_match && text(exact).find("@ Sysop Utilities")!=std::string::npos,
                    "exact approved level999 artwork selected, literal at-command survives");
            }
            request.identity=Id::welcome1;request.authenticated_security_level.reset();request.graphics=app::GraphicsMode::ansi;
            reader.files["WELCOME1.ANS"]=read(root/"test/fixtures/section5-login-menu/welcome1.ans");
            const auto welcome=selector.select(request);
            require(welcome.available() && !text(welcome).empty(),"exact owner welcome ANSI renders");
            for(const auto& op:welcome.program->terminal_program.operations)
                require(op.bytes.find('\x10')==std::string::npos,"legacy control is never sent to terminal");
        }
        const std::array<std::pair<Id,const char*>,8> optional{{{Id::all,"ALL"},{Id::chatdone,"CHATDONE"},
            {Id::eventime,"EVENTIME"},{Id::goodbye,"GOODBYE"},{Id::lockout,"LOCKOUT"},
            {Id::newuser,"NEWUSER"},{Id::sfasleep,"SFASLEEP"},{Id::sfnewsletter,"SFNWSLTR"}}};
        for(const auto& item:optional) for(auto graphics:{app::GraphicsMode::ascii,app::GraphicsMode::ansi}) {
            Reader optional_reader;app::TrustedDisplaySelector optional_selector(optional_reader);
            app::TrustedDisplaySelectionRequest event;event.identity=item.first;event.graphics=graphics;
            require(!optional_selector.select(event).available(),"absent optional display has no fabricated program");
            const std::string filename=std::string(item.second)+(graphics==app::GraphicsMode::ansi?".ANS":".ASC");
            optional_reader.files[filename]="Optional Sysop event";
            auto optional_selected=optional_selector.select(event);
            require(optional_selected.available() && optional_selected.program->selected_logical_asset==filename,"exact optional filename and graphics selected");
            auto framed=optional_selected.program->terminal_program;
            require(app::prepare_acknowledged_display(framed,graphics),"optional screen acknowledgment is bounded");
            require(framed.operations.front().kind==(graphics==app::GraphicsMode::ansi?
                app::TerminalDisplayOperationKind::clear_screen:app::TerminalDisplayOperationKind::logical_newline),"optional screen clears first");
            require(framed.operations[framed.operations.size()-(graphics==app::GraphicsMode::ansi?2:26)].kind==
                app::TerminalDisplayOperationKind::pause,"optional screen pauses before final clear");
            optional_reader.files[filename]="@PASSWORD@";
            require(!optional_selector.select(event).available(),"unsafe optional display cannot expose password");
        }
        std::vector<Id> fallback_ids(menus.begin(),menus.end());
        for(const auto& item:optional) fallback_ids.push_back(item.first);
        fallback_ids.push_back(Id::welcome1);fallback_ids.push_back(Id::newreg);fallback_ids.push_back(Id::nonodes);
        for(const auto id:fallback_ids) {
            Reader fallback_reader;app::TrustedDisplaySelector fallback_selector(fallback_reader);
            app::TrustedDisplaySelectionRequest event;event.identity=id;event.graphics=app::GraphicsMode::ascii;
            if(!app::trusted_menu_basename(id).empty()) event.authenticated_security_level=std::uint16_t{999};
            auto absent=fallback_selector.select(event);require(!absent.available(),"missing pair uses built-in behavior");
            const auto plain_name=fallback_reader.requests.back();
            auto ansi_name=plain_name;ansi_name.replace(ansi_name.size()-4,4,".ANS");
            fallback_reader.files[plain_name]="Plain owner display\r\n";
            require(text(fallback_selector.select(event))=="Plain owner display","ASCII selects ASC");
            event.graphics=app::GraphicsMode::ansi;
            auto plain=fallback_selector.select(event);
            require(plain.available() && plain.program->selected_logical_asset==plain_name,"ANSI uses ASC when ANS missing for every identity");
            fallback_reader.files[ansi_name]=sauce("\x1b[31mAuthored ANSI\x1b[0m");
            require(text(fallback_selector.select(event))=="Authored ANSI","ANSI prefers ANS and consumes optional SAUCE");
            event.graphics=app::GraphicsMode::ascii;fallback_reader.files.erase(plain_name);
            require(!fallback_selector.select(event).available(),"ASCII never selects ANS-only artwork");
        }
        if(argc==2) {
            std::ifstream owner(std::filesystem::path(argv[1])/"research/architecture/evidence/section5-owner-findings-20260930/owner-files/sfnwsltr.asc",std::ios::binary);
            const std::string raw((std::istreambuf_iterator<char>(owner)),{});
            require(owner.good() && raw.size()==43,"exact owner newsletter fixture retained");
            for(auto graphics:{app::GraphicsMode::ascii,app::GraphicsMode::ansi}) for(bool renamed:{false,true}) {
                Reader exact_reader;app::TrustedDisplaySelector exact_selector(exact_reader);
                exact_reader.files[renamed?"SFNWSLTR.ANS":"SFNWSLTR.ASC"]=raw;
                app::TrustedDisplaySelectionRequest event;event.identity=Id::sfnewsletter;event.graphics=graphics;
                auto result=exact_selector.select(event);
                if(graphics==app::GraphicsMode::ascii && renamed) require(!result.available(),"ASCII rejects owner renamed ANSI-only file");
                else require(result.available() && text(result)=="9-30-26:welcome to SFX! More to follow:","exact owner newsletter content renders, including renamed ANS");
            }
        }
        std::cout<<"PASS "<<checks<<" menu display checks\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<"\n"; return 1; }
}
