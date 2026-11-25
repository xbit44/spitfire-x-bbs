#include "sfx/application/classic_information_storage.hpp"
#include "sfx/application/phase3a_storage.hpp"
#include <algorithm>
#include <charconv>
#include <limits>

namespace sfx::application {
namespace {
bool printable(std::string_view value,std::size_t maximum) {
    return !value.empty() && value.size()<=maximum && value.front()!=' ' && value.back()!=' ' &&
        std::all_of(value.begin(),value.end(),[](unsigned char c){return c>=32 && c<127;});
}
bool address(std::string_view value,std::string_view scheme) {
    if(value.empty()) return true;
    if(!printable(value,kOtherBoardAddressMaximum)) return false;
    const auto prefix=value.find("://");
    if(prefix!=std::string_view::npos) {
        if(value.substr(0,prefix)!=scheme) return false;
        value.remove_prefix(prefix+3);
    }
    if(value.empty()) return false;
    std::string_view host=value,port;
    if(value.front()=='[') {
        const auto end=value.find(']');
        if(end==std::string_view::npos || end<3) return false;
        host=value.substr(1,end-1);
        if(host.find(':')==std::string_view::npos || !canonicalize_source_ip(host)) return false;
        if(end+1<value.size()) {if(value[end+1]!=':')return false;port=value.substr(end+2);if(port.empty())return false;}
    } else {
        const auto colon=value.find(':');
        if(colon!=std::string_view::npos) {host=value.substr(0,colon);port=value.substr(colon+1);if(port.empty())return false;}
        if(host.empty() || host.size()>253 || host.front()=='.' || host.back()=='.' || host.front()=='-' || host.back()=='-' ||
            !std::all_of(host.begin(),host.end(),[](unsigned char c){return (c>='0'&&c<='9') || (c>='a'&&c<='z') || (c>='A'&&c<='Z') || c=='.' || c=='-';})) return false;
        for(std::size_t begin=0;begin<host.size();) {
            const auto end=host.find('.',begin);
            const auto label=host.substr(begin,end==std::string_view::npos?host.size()-begin:end-begin);
            if(label.empty() || label.size()>63 || label.front()=='-' || label.back()=='-')return false;
            if(end==std::string_view::npos)break;
            begin=end+1;
        }
    }
    if(port.empty()) return true;
    unsigned number{};const auto parsed=std::from_chars(port.data(),port.data()+port.size(),number);
    return parsed.ec==std::errc{} && parsed.ptr==port.data()+port.size() && number>=1 && number<=65535;
}
}
bool valid_other_board_values(const OtherBoardValues& v) noexcept {
    return printable(v.name,kOtherBoardNameMaximum) && (!v.telnet.empty() || !v.ssh.empty()) && address(v.telnet,"telnet") && address(v.ssh,"ssh");
}
bool valid_classic_information_change(const ClassicInformationChange& v) noexcept {
    const bool empty=v.values.name.empty()&&v.values.telnet.empty()&&v.values.ssh.empty();
    if(v.expected_revision<0 || v.expected_revision==std::numeric_limits<std::int64_t>::max())return false;
    switch(v.action) {
        case ClassicInformationAction::add_board:return v.id==0 && v.expected_revision==0 && v.date.empty() && valid_other_board_values(v.values);
        case ClassicInformationAction::edit_board:return v.id>0 && v.id<=99999 && v.expected_revision>0 && v.date.empty() && valid_other_board_values(v.values);
        case ClassicInformationAction::remove_board:return v.id>0 && v.id<=99999 && v.expected_revision>0 && empty && v.date.empty();
        case ClassicInformationAction::opening_date:return v.id==0 && v.expected_revision>0 && empty && valid_iso_civil_date(v.date);
    }
    return false;
}
} // namespace sfx::application
