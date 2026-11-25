#include "sfx/application/local_control_protocol.hpp"
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;
namespace {
unsigned checks{};
void check(bool b,const char* message){++checks;if(!b)throw std::runtime_error(message);}
template<class T,class Encode,class Decode> void wire(const T& value,Encode encode,Decode decode){
    const auto bytes=encode(value);check(!bytes.empty()&&bytes.size()<=app::kLocalControlMaximumPayloadBytes,"bounded encoded configuration");
    const auto decoded=decode(bytes);check(decoded&&encode(*decoded)==bytes,"canonical configuration roundtrip");
    for(std::size_t n=0;n<bytes.size();++n)check(!decode(std::vector<std::uint8_t>(bytes.begin(),bytes.begin()+n)),"every truncated configuration rejected");
    auto trailing=bytes;trailing.push_back(0);check(!decode(trailing),"trailing data rejected");
}
}
int main(){try{
    app::LocalFileConfigurationQuery query;
    for(unsigned n=0;n<=4;++n){query={};query.part=static_cast<app::FileConfigurationPart>(n);if(n==4)query.account=42;wire(query,app::encode_file_configuration_query,app::decode_file_configuration_query);}
    query={};query.after_id=1;check(app::encode_file_configuration_query(query).empty(),"continuation requires revision");query={};query.group={1};check(app::encode_file_configuration_query(query).empty(),"group filter not smuggled onto policy");
    auto invalid_query=app::encode_file_configuration_query({});invalid_query[0]=255;check(!app::decode_file_configuration_query(invalid_query),"unknown query kind rejected");
    app::FileConfigurationChange c;c.operation.bytes[0]=1;c.expected_catalog_revision=1;
    wire(c,app::encode_file_configuration_change,app::decode_file_configuration_change);
    c.policy.counts.daily_downloads=25;c.policy.counts.ratio=app::FileRatio{10,1};c.policy.scan_uploads=true;
    wire(c,app::encode_file_configuration_change,app::decode_file_configuration_change);
    c.action=app::FileConfigurationAction::group;c.group.name="Main";wire(c,app::encode_file_configuration_change,app::decode_file_configuration_change);
    c.action=app::FileConfigurationAction::area;c.area.area.group={1};c.area.area.name="New library";c.area.storage_path="Files/NewLibrary";c.area.area.maximum_upload_bytes=1048576;
    wire(c,app::encode_file_configuration_change,app::decode_file_configuration_change);
    c.action=app::FileConfigurationAction::security_rule;c.security.level=10;c.security.inherit=false;c.security.counts.ratio=app::FileRatio{2,1};wire(c,app::encode_file_configuration_change,app::decode_file_configuration_change);
    c.action=app::FileConfigurationAction::caller_exemption;c.exemption.account=42;c.exemption.daily_exempt=true;wire(c,app::encode_file_configuration_change,app::decode_file_configuration_change);
    c.anchor={1};c.anchor_revision=1;
    for(auto action:{app::FileConfigurationAction::insert_area_before,app::FileConfigurationAction::insert_area_after}){c.action=action;wire(c,app::encode_file_configuration_change,app::decode_file_configuration_change);}
    c.area.area.id={2};c.area.area.number=2;
    for(auto action:{app::FileConfigurationAction::move_area_up,app::FileConfigurationAction::move_area_down}){c.action=action;wire(c,app::encode_file_configuration_change,app::decode_file_configuration_change);}
    c.group.id={2};c.group.deletion_operation=c.operation;c.delete_disk_files=true;
    for(auto action:{app::FileConfigurationAction::preview_group_delete,app::FileConfigurationAction::begin_group_delete,app::FileConfigurationAction::finish_group_delete}){c.action=action;wire(c,app::encode_file_configuration_change,app::decode_file_configuration_change);}
    c.deletion_items.push_back({});const auto native_only=app::decode_file_configuration_change(app::encode_file_configuration_change(c));check(native_only&&native_only->deletion_items.empty(),"IPC cannot supply native deletion plan");
    app::LocalFileConfigurationResult extended{app::LocalAccountResultCode::ok,{2,42,false},"Reviewed safely",3,4,5};const auto ext=app::encode_file_configuration_result(extended);const auto decoded_ext=app::decode_file_configuration_result(ext);check(decoded_ext&&decoded_ext->area_count==3&&decoded_ext->catalog_files==4&&decoded_ext->disk_files==5&&decoded_ext->detail==extended.detail,"bounded preview counts and detail roundtrip");
    extended.detail=std::string(513,'x');check(app::encode_file_configuration_result(extended).empty(),"oversized native detail rejected");extended.detail="bad\ntext";check(app::encode_file_configuration_result(extended).empty(),"control characters in result refused");
    c.action=app::FileConfigurationAction::policy;c.policy.counts.ratio=app::FileRatio{};check(app::encode_file_configuration_change(c).empty(),"ratio On requires an explicit nonzero pair");
    auto malformed=app::encode_file_configuration_change(app::FileConfigurationChange{c.operation,app::FileConfigurationAction::policy,1});malformed.back()=2;check(!app::decode_file_configuration_change(malformed),"non-Boolean policy flags rejected");
    for(const auto* path:{"../elsewhere","Files/../Data","C:","C:/","C:/safe//bad","Files/name ","Files/NUL","\\\\host\\share","Files/ok:stream","Files/bad\x1bname","Files/x/"})check(!app::valid_file_configuration_path(path),"unsafe path text rejected");
    check(app::valid_file_configuration_path("C:/libraries/Files")&&app::valid_file_configuration_path(u8"Files/École"),"bounded explicit local and UTF8 paths supported");
#if defined(__linux__)
    check(app::valid_file_configuration_path("/home/sysop/shared files")&&app::valid_file_configuration_path(u8"/home/sysop/École"),"Linux absolute external areas admitted lexically");
    for(const auto* path:{"/","//host/share","/home/../etc","/home//files","/home/files/","/C:/Files","/home\\files"})check(!app::valid_file_configuration_path(path),"Linux absolute aliases and ambiguous paths refused");
#else
    check(!app::valid_file_configuration_path("/home/sysop/shared files"),"Windows absolute path policy unchanged");
#endif
    for(unsigned n=0;n<=4;++n){
        app::LocalFileConfigurationSnapshot s;s.code=app::LocalAccountResultCode::ok;s.part=static_cast<app::FileConfigurationPart>(n);
        if(n==1)for(unsigned i=1;i<=12;++i)s.groups.push_back({{i},"Group",static_cast<std::int64_t>(12-i),1,1,true});
        if(n==2)for(unsigned i=1;i<=12;++i){app::FileAreaConfiguration a;a.area.id={i};a.area.group={1};a.area.number=static_cast<std::uint16_t>(i);a.area.name="Library";a.storage_path="C:/";for(unsigned j=0;j<15;++j)a.storage_path+=std::string(100,'a')+"/";a.storage_path+="library";s.areas.push_back(std::move(a));}
        if(n==3)for(unsigned i=1;i<=12;++i){app::FileSecurityRule r;r.level=static_cast<std::uint16_t>(i);r.inherit=false;s.security_rules.push_back(r);}
        if(n==4)s.exemption=app::FileCallerExemption{42,1,true,false};
        if(n>=1&&n<=3)s.next_after=12;
        wire(s,app::encode_file_configuration_snapshot,app::decode_file_configuration_snapshot);
        if(n==1){s.groups[1].id=s.groups[0].id;check(app::encode_file_configuration_snapshot(s).empty(),"duplicate identities rejected");}
    }
    for(unsigned n=0;n<=9;++n){app::LocalFileConfigurationSnapshot s;s.code=static_cast<app::LocalAccountResultCode>(n);if(n)s.exemption.reset();wire(s,app::encode_file_configuration_snapshot,app::decode_file_configuration_snapshot);}
    wire(app::LocalFileConfigurationResult{app::LocalAccountResultCode::ok,{2,42,true}},app::encode_file_configuration_result,app::decode_file_configuration_result);
    wire(app::LocalFileConfigurationResult{app::LocalAccountResultCode::conflict},app::encode_file_configuration_result,app::decode_file_configuration_result);
    check(app::encode_file_configuration_result({app::LocalAccountResultCode::conflict,{2,42,true}}).empty(),"refusal cannot claim a successful mutation");
    app::LocalControlFrame frame;frame.type=app::LocalControlMessageType::file_configuration_change_request;frame.payload=app::encode_file_configuration_change({c.operation,app::FileConfigurationAction::policy,1});
    check(app::encode_local_control_frame(frame).empty(),"unassigned request identity is rejected before framing");frame.request_id=1;
    check(app::decode_local_control_frame(app::encode_local_control_frame(frame)).code==app::LocalControlFrameDecodeCode::complete,"new typed frame accepted by versioned framing");
    frame.version.major=32;check(app::decode_local_control_frame(app::encode_local_control_frame(frame)).code==app::LocalControlFrameDecodeCode::incompatible_version,"accepted predecessor IPC32 rejected before new payload interpretation");
    std::cout<<"PASS file configuration protocol "<<checks<<" checks; portable codecs only\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
