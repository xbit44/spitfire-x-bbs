#include "sfx/application/classic_information_storage.hpp"

namespace sfx::application {
namespace {
std::string_view trim(std::string_view text) {
    while(!text.empty() && (text.front()==' ' || text.front()=='\t'))text.remove_prefix(1);
    while(!text.empty() && (text.back()==' ' || text.back()=='\t'))text.remove_suffix(1);
    return text;
}
}
std::optional<std::vector<OtherBoardValues>> parse_other_board_text(std::string_view text) {
    if(text.size()>kOtherBoardTextMaximum)return {};
    if(text.substr(0,3)=="\xef\xbb\xbf")text.remove_prefix(3);
    std::vector<OtherBoardValues> result;OtherBoardValues current;unsigned fields=0;
    const auto finish=[&] {
        if(!fields)return true;
        if(!(fields&1) || !valid_other_board_values(current) || result.size()>=kOtherBoardMaximum)return false;
        result.push_back(std::move(current));current={};fields=0;return true;
    };
    while(!text.empty()) {
        const auto end=text.find('\n');auto line=text.substr(0,end);
        if(end==text.npos)text={};else text.remove_prefix(end+1);
        if(!line.empty() && line.back()=='\r')line.remove_suffix(1);
        line=trim(line);
        if(line.empty()){if(!finish())return {};continue;}
        if(line.front()=='#' || line.front()==';')continue;
        unsigned field=0;std::string* value=nullptr;
        if(line.substr(0,5)=="Name:"){field=1;value=&current.name;line.remove_prefix(5);}
        else if(line.substr(0,7)=="Telnet:"){field=2;value=&current.telnet;line.remove_prefix(7);}
        else if(line.substr(0,4)=="SSH:"){field=4;value=&current.ssh;line.remove_prefix(4);}
        else return {};
        if(fields&field)return {};
        fields|=field;*value=std::string(trim(line));
    }
    if(!finish())return {};
    return result;
}
std::string encode_other_board_text(const std::vector<OtherBoardValues>& entries) {
    if(entries.size()>kOtherBoardMaximum)return {};
    std::string text;
    for(const auto& row:entries) {
        if(!valid_other_board_values(row))return {};
        text+="Name: "+row.name+"\r\nTelnet: "+row.telnet+"\r\nSSH: "+row.ssh+"\r\n\r\n";
    }
    return text;
}
}
