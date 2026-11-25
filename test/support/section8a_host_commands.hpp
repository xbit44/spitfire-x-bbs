#pragma once
#include "sfx/application/local_message_administration.hpp"
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>

// Test coordinators only. Installed callbacks use authenticated private IPC;
// this textual fixture adapter is never linked into a shipping caller API.
inline bool section8a_command(const std::string& line,
    const std::function<sfx::application::LocalMessageConfigurationSnapshot(const sfx::application::LocalMessageConfigurationQuery&)>& query,
    const std::function<sfx::application::LocalMessageConfigurationResult(const sfx::application::MessageConfigurationChange&)>& execute){
    namespace app=sfx::application;
    if(line.rfind("message ",0)!=0)return false;
    std::istringstream in(line.substr(8));std::string action;in>>action;
    if(action=="query"){
        app::LocalMessageConfigurationQuery q;std::int64_t group{};
        in>>group>>q.cursor.configuration_revision>>q.cursor.after_order>>q.cursor.after_id>>q.cursor.page_size;
        if(group)q.group=app::MessageGroupId{group};
        const auto s=in?query(q):app::LocalMessageConfigurationSnapshot{};
        std::cout<<"{\"ok\":"<<(s.code==app::LocalAccountResultCode::ok)<<",\"revision\":"<<s.catalog.revision
            <<",\"sysop\":"<<s.catalog.system_sysop.value_or(0)<<",\"assignment_revision\":"<<s.catalog.assignment_revision
            <<",\"private_pipe_colors\":"<<s.catalog.private_pipe_colors<<",\"private_presentation_revision\":"<<s.catalog.private_presentation_revision
            <<",\"next\":"<<(s.catalog.next.has_value())<<",\"groups\":[";
        bool first=true;for(const auto& g:s.catalog.groups){if(!first)std::cout<<',';first=false;
            std::cout<<"{\"id\":"<<g.id.value<<",\"revision\":"<<g.revision<<",\"name\":"<<std::quoted(g.name)
                <<",\"order\":"<<g.order<<",\"access\":"<<g.access<<",\"enabled\":"<<g.enabled<<'}';}
        std::cout<<"],\"echoes\":[";first=true;
        for(const auto& e:s.catalog.echoes){if(!first)std::cout<<',';first=false;
            std::cout<<"{\"id\":"<<e.id.value<<",\"revision\":"<<e.revision<<",\"group\":"<<e.group.value
                <<",\"number\":"<<e.conference_number<<",\"name\":"<<std::quoted(e.name)<<",\"order\":"<<e.order
                <<",\"access\":"<<e.permissions.access<<",\"reading\":"<<e.permissions.reading<<",\"posting\":"<<e.permissions.posting
                <<",\"enabled\":"<<e.enabled<<",\"pipe_colors\":"<<e.renegade_pipe_colors<<'}';}
        std::cout<<"],\"limits\":[";first=true;
        for(const auto& l:s.catalog.line_limits){if(!first)std::cout<<',';first=false;
            std::cout<<"{\"level\":"<<l.security_level<<",\"revision\":"<<l.revision<<",\"lines\":"<<l.maximum_lines<<'}';}
        std::cout<<"]}"<<std::endl;return true;
    }
    app::MessageConfigurationChange c;std::uint32_t operation{};
    in>>operation>>c.expected_configuration_revision;
    c.operation.bytes[0]=0x8a;
    for(unsigned n=0;n<4;++n)c.operation.bytes[15-n]=static_cast<std::uint8_t>(operation>>(8*n));
    if(action=="limit"){
        c.action=app::MessageConfigurationAction::line_limit;in>>c.security_level>>c.maximum_lines>>c.expected_limit_revision;
    }else if(action=="sysop"){
        c.action=app::MessageConfigurationAction::system_sysop;in>>c.sysop>>c.expected_account_revision>>c.expected_assignment_revision;
    }else if(action=="group"){
        c.action=app::MessageConfigurationAction::group;in>>c.group.id.value>>c.group.revision>>c.group.order>>c.group.access>>c.group.enabled>>std::quoted(c.group.name);
    }else if(action=="delete-group"){
        c.action=app::MessageConfigurationAction::delete_group;in>>c.group.id.value>>c.group.revision;
    }else if(action=="delete" || action=="up" || action=="down"){
        c.action=action=="delete"?app::MessageConfigurationAction::delete_echo:action=="up"?app::MessageConfigurationAction::move_echo_up:app::MessageConfigurationAction::move_echo_down;
        in>>c.echo.id.value>>c.echo.revision;
    }else if(action=="insert"){
        c.action=app::MessageConfigurationAction::insert_echo;
        in>>c.echo.group.value>>c.insert_before.value>>c.insert_before_revision>>c.echo.permissions.access>>c.echo.permissions.reading>>c.echo.permissions.posting>>std::quoted(c.echo.name);c.echo.enabled=true;
    }else if(action=="private-pipe"){
        c.action=app::MessageConfigurationAction::private_presentation;in>>c.expected_private_presentation_revision>>c.private_pipe_colors;
    }else if(action=="echo" || action=="echo-pipe"){
        c.action=app::MessageConfigurationAction::echo;in>>c.echo.id.value>>c.echo.revision>>c.echo.group.value>>c.echo.conference_number>>c.echo.order
            >>c.echo.permissions.access>>c.echo.permissions.reading>>c.echo.permissions.posting>>c.echo.enabled>>std::quoted(c.echo.name);
        if(action=="echo-pipe"){c.presentation_mask=1;in>>c.echo.renegade_pipe_colors;}
    }else if(action=="clone"){
        c.action=app::MessageConfigurationAction::clone_permissions;unsigned mask{},count{};
        in>>c.clone_source.value>>c.source_revision>>mask>>count;c.permission_mask=static_cast<std::uint8_t>(mask);
        if(mask>7 || count>999)throw std::runtime_error("bounded message clone required");
        for(unsigned n=0;n<count;++n){app::MessageEchoRevision target;in>>target.echo.value>>target.revision;c.clone_targets.push_back(target);}
    }else throw std::runtime_error("bounded message configuration command required");
    const auto result=in && operation?execute(c):app::LocalMessageConfigurationResult{app::LocalAccountResultCode::invalid_request};
    std::cout<<"{\"ok\":"<<(result.code==app::LocalAccountResultCode::ok)<<",\"code\":"<<static_cast<unsigned>(result.code)
        <<",\"revision\":"<<result.result.configuration_revision<<",\"id\":"<<result.result.entity_id
        <<",\"entity_revision\":"<<result.result.entity_revision<<",\"replayed\":"<<result.result.replayed<<'}'<<std::endl;return true;
}
