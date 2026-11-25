#include "sfx/application/password_change_prompt.hpp"
#include "sfx/application/terminal_stream.hpp"
#include <deque>
#include <iostream>
#include <stdexcept>

namespace app=sfx::application;
namespace {
unsigned checks{};
void check(bool okay,const char* text){++checks;if(!okay)throw std::runtime_error(text);}
struct Wire final:app::IInteractiveSessionByteStream {
    std::deque<unsigned char> input;std::string output;
    explicit Wire(std::string_view text){for(unsigned char c:text)input.push_back(c);}
    app::InteractiveStreamReadResult read_some(std::size_t)override{
        if(input.empty())return {app::InteractiveStreamOutcome::disconnected,{}};
        const auto byte=input.front();input.pop_front();return {app::InteractiveStreamOutcome::completed,{byte}};
    }
    app::InteractiveStreamWriteResult write_some(std::string_view text)override{output+=text;return {app::InteractiveStreamOutcome::completed,text.size()};}
};
std::size_t occurrences(std::string_view text,std::string_view value){std::size_t count{},at{};while((at=text.find(value,at))!=text.npos){++count;at+=value.size();}return count;}
}
int main()try{
    for(const auto input:{"\r\n\r\n","\n\n"}){
        Wire wire(input);app::StreamCallerTerminal terminal(wire);
        const auto result=app::read_password_change_current(terminal,64);
        check(result.outcome==app::TerminalOutcome::cancelled&&result.value.empty(),"two logical empty entries cancel without a password value");
        check(occurrences(wire.output,"Current password: ")==2,"two logical empty entries produce exactly two current-password prompts");
        check(wire.output.find("New password")==wire.output.npos,"cancel does not enter replacement-password flow");
    }
    {
        Wire wire("\r\nfixture-current\r\n");app::StreamCallerTerminal terminal(wire);
        auto result=app::read_password_change_current(terminal,64);
        check(result.has_value()&&result.value=="fixture-current","one empty entry followed by nonempty current password retains verification input");
        check(occurrences(wire.output,"Current password: ")==2,"nonempty second attempt needs no extra prompt");
        check(wire.output.find("fixture-current")==wire.output.npos,"secret bytes are never echoed");
        app::explicitly_erase_terminal_secret(result.value);
        check(result.value.empty(),"returned secret retains explicit erase support");
    }
    {
        Wire wire("\r\n");app::StreamCallerTerminal terminal(wire);
        const auto result=app::read_password_change_current(terminal,64);
        check(result.outcome==app::TerminalOutcome::disconnected,"one empty and disconnect remains a connection failure");
    }
    {
        Wire wire(std::string("\r\n")+char(0x11));app::StreamCallerTerminal terminal(wire);
        check(app::read_password_change_current(terminal,64).outcome==app::TerminalOutcome::cancelled,"existing Ctrl-Q cancellation remains cancellation");
    }
    std::cout<<"PASS beta6 current-password prompt "<<checks<<" real stream checks; no authentication or credentials changed\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}
