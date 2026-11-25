#pragma once
#include "sfx/application/local_classic_information.hpp"
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>

// Test-only command adapter. Neither fixture authority nor SQL is a caller API.
inline bool section7_command(const std::string& line,
    const std::function<sfx::application::LocalClassicInformationSnapshot(std::int64_t)>& query,
    const std::function<sfx::application::LocalAccountResult(const sfx::application::ClassicInformationChange&)>& execute) {
    namespace app=sfx::application;
    if(line.rfind("classic ",0)!=0)return false;
    std::istringstream input(line.substr(8));std::string kind;input>>kind;
    if(kind=="query") {
        std::int64_t after{};input>>after;const auto value=query(after);
        std::cout<<"{\"ok\":"<<(value.code==app::LocalAccountResultCode::ok)<<",\"opening_revision\":"<<value.opening.revision
            <<",\"initialization_utc\":"<<value.opening.initialization_utc<<",\"date\":"<<std::quoted(value.opening.configured_date.value_or(""))
            <<",\"more\":"<<value.boards.has_more<<",\"entries\":[";
        bool first=true;for(const auto& row:value.boards.entries) {
            if(!first)std::cout<<',';first=false;
            std::cout<<"{\"id\":"<<row.id<<",\"revision\":"<<row.revision<<",\"name\":"<<std::quoted(row.values.name)
                <<",\"telnet\":"<<std::quoted(row.values.telnet)<<",\"ssh\":"<<std::quoted(row.values.ssh)<<'}';
        }
        std::cout<<"]}"<<std::endl;return true;
    }
    if(kind=="change") {
        unsigned action=99;app::ClassicInformationChange change;
        input>>action>>change.id>>change.expected_revision>>std::quoted(change.values.name)>>std::quoted(change.values.telnet)>>std::quoted(change.values.ssh)>>std::quoted(change.date);
        change.action=static_cast<app::ClassicInformationAction>(action);
        const auto result=input?execute(change):app::LocalAccountResult{app::LocalAccountResultCode::invalid_request};
        std::cout<<"{\"ok\":"<<(result.code==app::LocalAccountResultCode::ok)<<",\"committed\":"<<result.committed
            <<",\"conflict\":"<<(result.code==app::LocalAccountResultCode::conflict)<<",\"revision\":"<<result.revision<<'}'<<std::endl;return true;
    }
    throw std::runtime_error("bounded Section7 fixture command required");
}
