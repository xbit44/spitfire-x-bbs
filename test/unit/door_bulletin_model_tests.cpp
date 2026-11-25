#include "sfx/application/door_bulletin.hpp"
#include "sfx/application/door_bulletin_preview_context.hpp"
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;
struct SampleAssets final:app::ITrustedDisplayAssetReader {app::TrustedDisplayAssetReadResult read_asset(std::string_view name,std::size_t)override{if(name!="DB001.ASC"&&name!="DB001.ANS")return {app::TrustedDisplayAssetReadCode::not_found,{},{},{},{}};return {app::TrustedDisplayAssetReadCode::available,std::string(name),"sample-file","@NAME@ | @VERSION@ | @DATE@\r\n",{}};}};
unsigned checks{};void check(bool v,const char* text){++checks;if(!v)throw std::runtime_error(text);}
int main(){try{
    for(unsigned n:{1,255,256,999})check(app::valid_door_bulletin_id({static_cast<std::uint16_t>(n)}),"permanent full-width ID valid");
    for(unsigned n:{0,1000,65535})check(!app::valid_door_bulletin_id({static_cast<std::uint16_t>(n)}),"out of range ID rejected");
    for(const auto path:{"SCORE.ANS","scores/top.asc","scores\\top.asc"})check(app::valid_door_bulletin_relative_path(path),"portable relative source allowed");
    check(app::valid_door_bulletin_relative_path(std::string(128,'a'))&&!app::valid_door_bulletin_relative_path(std::string(129,'a')),"portable existing filename component limit enforced before save");
    for(const auto path:{"scores~1/x.txt","CLOCK$.TXT","COM1 .TXT","CONIN$.TXT"," file.txt"})check(!app::valid_door_bulletin_relative_path(path),"native alias/device admission agrees across platforms");
    for(const auto path:{"","/x","\\x","C:/x","../x","a/../x","a//b","a/./b","a/","a/CON.txt","nul","AUX","COM1.asc","LPT9.txt","a.","a ","a:b","a?b","a\n.txt"})check(!app::valid_door_bulletin_relative_path(path),"unsafe source path rejected");
    app::DoorBulletinDefinition d;d.id={999};d.title="Scores";d.revision=1;check(app::valid_door_bulletin_definition(d),"static definition");d.relative_path="x";check(!app::valid_door_bulletin_definition(d),"static cannot carry generated fields");d.source=app::DoorBulletinSourceKind::door_file;d.door=256;d.relative_path="SCORE.ANS";d.format=app::GraphicsMode::ansi;check(app::valid_door_bulletin_definition(d),"explicit generated ANSI definition");
    app::DoorBulletinCatalog c;c.revision=4;c.entries.push_back({d,app::DoorBulletinHealth::available,{}});
    for(auto utc:{app::kMinimumUtcSeconds,std::int64_t{-1},std::int64_t{0},std::int64_t{4294967296},app::kMaximumUtcSeconds}){c.entries[0].published_utc=utc;auto bytes=app::encode_door_bulletin_catalog(c);const auto back=app::decode_door_bulletin_catalog(bytes);check(back&&back->entries[0].published_utc==utc&&back->entries[0].definition.id.value==999&&back->entries[0].definition.door==256,"canonical signed UTC and full-width IDs round trip");}
    for(auto bad:{app::kMinimumUtcSeconds-1,app::kMaximumUtcSeconds+1}){c.entries[0].published_utc=bad;check(app::encode_door_bulletin_catalog(c).empty(),"invalid publication clock rejected");}
    c.entries[0].published_utc=app::kMinimumUtcSeconds;const auto complete=app::encode_door_bulletin_catalog(c);for(std::size_t i=0;i<complete.size();++i)check(!app::decode_door_bulletin_catalog({complete.begin(),complete.begin()+i}),"truncated snapshot rejected");auto extra=complete;extra.push_back(0);check(!app::decode_door_bulletin_catalog(extra),"trailing payload rejected");c.entries.push_back(c.entries.front());check(app::encode_door_bulletin_catalog(c).empty(),"duplicate permanent IDs rejected");
    app::DoorBulletinChange change;change.operation.bytes[0]=1;change.expected_revision=1;change.create_only=true;change.definition=d;auto encoded=app::encode_door_bulletin_change(change);auto decoded=app::decode_door_bulletin_change(encoded);check(decoded&&decoded->definition.id.value==999&&decoded->definition.door==256&&decoded->create_only,"typed guarded change round trip");
    app::DoorBulletinPublication p;p.id={999};p.definition_revision=1;p.available=true;p.fingerprint_input="source";
    for(const auto variant:{"DB999.ANS","DB999999.ANS","DB99910.ASC","GENERATED-ASC","GENERATED-ANS"}){p.variant=variant;check(app::valid_door_bulletin_publication(p),"bounded exact source variant");}
    for(const auto variant:{"DB255.ANS","DB9990.ANS","DB9990001.ANS","DB9991000.ANS","../../file","UNAVAILABLE","ARBITRARY"}){p.variant=variant;check(!app::valid_door_bulletin_publication(p),"unbounded/foreign variant rejected");}
    p.available=false;p.fingerprint_input.clear();p.variant="UNAVAILABLE";check(app::valid_door_bulletin_publication(p),"unavailability carries no stale snapshot");p.captured_content.push_back('x');check(!app::valid_door_bulletin_publication(p),"failed publication cannot retain old bytes");
    for(const auto graphics:{app::GraphicsMode::ascii,app::GraphicsMode::ansi}){const auto request=app::door_bulletin_preview_request({1},graphics,"Test BBS","Test Version",4);SampleAssets reader;app::TrustedDisplaySelector selector(reader);const auto selected=selector.select(request);if(!selected.available())std::cerr<<"sample select code "<<static_cast<unsigned>(selected.code)<<'\n';check(selected.available(),"complete sample context selects static preview source");}
    std::cout<<"PASS "<<checks<<" Door Bulletin model/codec checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
