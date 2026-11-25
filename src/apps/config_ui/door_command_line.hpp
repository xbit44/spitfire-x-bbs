#pragma once
#include "sfx/application/door.hpp"
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::config_ui {
// UI representation only. Never a process command, shell or evaluator. The
// runtime continues to receive exactly its existing structured RloginTarget.
enum class DoorCommandPassword { unchanged, plain_prompt, caller_hash_prompt };
struct DoorCommandResult {
    application::RloginTarget target;
    DoorCommandPassword password{DoorCommandPassword::unchanged};
    std::string error;
    explicit operator bool()const noexcept{return error.empty();}
};
inline bool door_command_uint(std::string_view value,unsigned maximum,unsigned& n){
    n=0;if(value.empty())return false;
    for(const auto ch:value){if(ch<'0'||ch>'9'||n>(maximum-static_cast<unsigned>(ch-'0'))/10)return false;n=n*10+static_cast<unsigned>(ch-'0');}
    return n>0&&n<=maximum;
}
inline std::string door_command_quote(std::string_view value){
    if(!value.empty()&&value.front()!='-'&&value.find_first_of(" \t\"\\'")==std::string_view::npos)return std::string(value);
    std::string out="\"";for(const auto ch:value){if(ch=='\\'||ch=='\"')out+='\\';out+=ch;}return out+'\"';
}
inline std::string format_door_command(const application::RloginTarget& t){
    if(t.host.empty())return {};
    const auto address=t.host.find(':')==std::string::npos?t.host:"["+t.host+"]";
    std::string out="rlogin "+door_command_quote(address+":"+std::to_string(t.port));
    if(t.quiet)out+=" -q";
    if(!t.pause)out+=" -P";
    if(t.prompt_credential&&t.server_identity==application::RloginIdentity::caller_alias&&t.client_name.empty())
        out+=t.authentication==application::RloginAuthentication::synchronet_caller_hash?" -h":" -p";
    if(!t.client_name.empty())out+=" -c "+door_command_quote(t.client_name);
    if(t.server_identity==application::RloginIdentity::configured)out+=" -s "+door_command_quote(t.server_name);
    out+=" -t "+door_command_quote(t.terminal_type);
    if(t.connection_timeout_seconds!=10)out+=" -T "+std::to_string(t.connection_timeout_seconds);
    // Salt, banner and protected secrets are retained in their existing settings.
    // Passwords/pepper are never formatted into this field.
    return out;
}
inline DoorCommandResult parse_door_command(std::string_view line,const application::RloginTarget& existing,bool telnet=false){
    using namespace application;DoorCommandResult r;r.target=existing;
    const auto fail=[&](const char* message){r.error=message;return r;};
    if(line.size()>4096)return fail("Command Line is too long (maximum 4096 bytes).");
    std::vector<std::string> words;std::vector<bool> quoted;
    for(std::size_t i=0;i<line.size();){
        while(i<line.size()&&(line[i]==' '||line[i]=='\t'))++i;
        if(i==line.size())break;
        if(words.size()>=32)return fail("Too many Command Line options.");
        std::string word;char quote{};bool was_quoted{};
        for(;i<line.size();++i){
            const auto ch=line[i];if(static_cast<unsigned char>(ch)<32&&ch!='\t')return fail("Command Line must be one line of text.");
            if(ch==127)return fail("Command Line contains an unsupported control character.");
            if(!quote&&(ch==' '||ch=='\t'))break;
            if(ch=='\"'||ch=='\''){if(!quote){quote=ch;was_quoted=true;continue;}if(quote==ch){quote=0;continue;}}
            if(ch=='\\'&&quote&&i+1<line.size()&&(line[i+1]==quote||line[i+1]=='\\')){word+=line[++i];continue;}
            word+=ch;
        }
        if(quote)return fail("Close the quotation marks in Command Line.");
        words.push_back(std::move(word));quoted.push_back(was_quoted);
    }
    if(words.empty()||words[0]!=(telnet?"telnet":"rlogin"))return fail(telnet?"Use telnet host[:port], with optional -t terminal and -T timeout.":"Use the built-in rlogin command without ?. Local programs and scripts are not available here yet.");
    if(words.size()<2||words[1].empty()||(!quoted[1]&&words[1].front()=='-'))return fail(telnet?"Enter the remote BBS address after telnet, with optional :port.":"Enter the remote BBS address after rlogin, with an optional :port.");
    auto& t=r.target;t.port=telnet?23:513;t.quiet=false;t.pause=true;t.client_name.clear();t.server_identity=RloginIdentity::caller_alias;t.terminal_type="ansi";t.connection_timeout_seconds=10;
    const auto& address=words[1];std::string port;
    if(address.front()=='['){const auto end=address.find(']');if(end==std::string::npos||end<2)return fail("Use [IPv6-address]:port for an IPv6 destination.");t.host=address.substr(1,end-1);if(end+1<address.size()){if(address[end+1]!=':')return fail("Place :port after the closing IPv6 bracket.");port=address.substr(end+2);if(port.empty())return fail("Enter a port after the colon.");}}
    else{const auto colon=address.find(':');if(colon!=std::string::npos&&address.find(':',colon+1)==std::string::npos){t.host=address.substr(0,colon);port=address.substr(colon+1);if(port.empty())return fail("Enter a port after the colon.");}else t.host=address;}
    unsigned value{};if(!port.empty()){if(!door_command_uint(port,65535,value))return fail("Port must be from 1 to 65535.");t.port=static_cast<std::uint16_t>(value);}
    if(!valid_door_text(t.host,253))return fail("Enter a BBS hostname or IP address (maximum 253 bytes).");
    for(const auto ch:t.host)if(static_cast<unsigned char>(ch)<=32||static_cast<unsigned char>(ch)>=127||ch=='/'||ch=='\\'||ch=='@'||ch=='['||ch==']')return fail("Use a BBS hostname or IP address without a URL, user name or spaces.");
    std::array<bool,128> seen{};
    for(std::size_t i=2;i<words.size();++i){
        const auto& word=words[i];if(word.size()<2||word[0]!='-')return fail(telnet?"Use only telnet host[:port], -t terminal and -T timeout.":"Unexpected Command Line argument. Use the supported rlogin options shown below.");
        const auto option=static_cast<unsigned char>(word[1]);
        if(telnet&&option!='t'&&option!='T')return fail("Telnet supports only -t terminal and -T timeout; no login or password options.");
        if(option=='H')return fail("Use Advanced RLogin Settings and its masked predefined-password field instead of -H password.");
        if(option=='h'&&word.size()!=2)return fail("Use the masked hash pepper field in Advanced RLogin Settings.");
        if(option=='m')return fail("Gateway modes and expressions (-m) are not supported by SFX's native rlogin command.");
        if(option=='v'||option=='C')return fail("The -v and -C options are not supported. SFX keeps its existing connection and return-screen behavior.");
        if(option>=seen.size()||std::string_view("qPphcstT").find(static_cast<char>(option))==std::string_view::npos)return fail("Unsupported rlogin option. Use -q, -P, -p, -h, -c, -s, -t or -T.");
        if(seen[option])return fail(telnet?"A Telnet option is repeated. Use -t and -T at most once.":"An rlogin option is repeated. Use each option once; combine a name into one quoted value.");
        seen[option]=true;
        if(option=='q'||option=='P'||option=='p'||option=='h'){
            if(word.size()!=2)return fail("Use separate rlogin options, such as -q -P; do not combine flags.");
            if(option=='q')t.quiet=true;else if(option=='P')t.pause=false;
            else r.password=option=='p'?DoorCommandPassword::plain_prompt:DoorCommandPassword::caller_hash_prompt;
            continue;
        }
        std::string argument=word.substr(2);
        if(argument.empty()){if(i+1>=words.size()||(!quoted[i+1]&&words[i+1].size()>1&&words[i+1][0]=='-'))return fail(telnet?"A Telnet option needs a value: -t terminal or -T seconds.":"An rlogin option needs a value. Use -t terminal, -T seconds, -s account or -c client-name.");argument=words[++i];}
        if(option=='T'){if(!door_command_uint(argument,120,value))return fail("Connection timeout (-T) must be from 1 to 120 seconds.");t.connection_timeout_seconds=static_cast<std::uint16_t>(value);}
        else if(option=='t'){if(!valid_door_text(argument,127))return fail("Terminal value (-t) must contain 1 to 127 bytes.");t.terminal_type=argument;}
        else if(option=='s'){if(!valid_door_text(argument,127))return fail("Remote account (-s) must contain 1 to 127 bytes.");t.server_identity=RloginIdentity::configured;t.server_name=argument;}
        else{if(!valid_door_text(argument,127,true))return fail("Client name (-c) must contain at most 127 bytes.");t.client_name=argument;}
    }
    if((seen['p']&&seen['h'])||((seen['p']||seen['h'])&&(seen['c']||seen['s'])))return fail("Use -p or -h alone for caller alias/password login; do not combine it with -c, -s or another password option.");
    return r;
}
struct TelnetCommandResult {
    application::TelnetTarget target;std::string error;
    explicit operator bool()const noexcept{return error.empty();}
};
inline TelnetCommandResult parse_telnet_command(std::string_view line){
    const auto r=parse_door_command(line,{},true);
    return {{r.target.host,r.target.port,r.target.terminal_type,r.target.connection_timeout_seconds},r.error};
}
inline std::string format_door_command(const application::TelnetTarget& t){
    if(t.host.empty())return {};
    const auto address=t.host.find(':')==std::string::npos?t.host:"["+t.host+"]";
    auto out="telnet "+door_command_quote(address+":"+std::to_string(t.port));
    if(t.terminal_type!="ansi")out+=" -t "+door_command_quote(t.terminal_type);
    if(t.connection_timeout_seconds!=10)out+=" -T "+std::to_string(t.connection_timeout_seconds);
    return out;
}
} // namespace sfx::config_ui
