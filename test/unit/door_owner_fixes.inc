#include "sfx/application/door_storage.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "door_legacy_fixtures.hpp"
namespace {
void owner_policy_and_order_checks(){
    auto d=door();d.execution={app::DoorSecurityMode::minimum,50,false};
    check(app::door_accessible(d,{10,90})&&!app::door_executable(d,{10,90}),"visible Door may refuse execution independently");
    for(const auto mode:{app::DoorSecurityMode::minimum,app::DoorSecurityMode::maximum,app::DoorSecurityMode::exact}){
        d.execution={mode,50,false};for(std::uint16_t level=1;level<=999;++level){const bool expected=mode==app::DoorSecurityMode::minimum?level>=50:mode==app::DoorSecurityMode::maximum?level<=50:level==50;check(app::door_executable(d,{level,90})==expected,"independent execution modes over entire security domain");}
    }
    d.security_level=40;d.sysop_only=true;d.execution={app::DoorSecurityMode::exact,90,false};
    const auto legacy=legacy_door_v1_fixture();
    const auto old=app::decode_door_definition(legacy);check(old&&old->execution.mode==d.security_mode&&old->execution.level==40&&old->execution.sysop_only,"v1 saved Door migrates existing rule to visibility and execution");
    d=door();app::DoorCatalog c;c.doors={d};c.menus.push_back({1,"BBS Transporter"});c.menus.push_back({2,"Games"});
    c.entries={{0,7,app::DoorEntryKind::submenu,1,"BBS Transporter","keep one"},{0,21,app::DoorEntryKind::submenu,2,"Games","keep two"},{0,42,app::DoorEntryKind::door,1,"Play","keep three"},{1,9,app::DoorEntryKind::door,1,"Shared",""}};
    c.menus[1].execution.level=50;check(app::visible_door_entries(c,0,{10,90}).size()==3,"category execution failure does not hide its entry");
    c.menus[1].access.level=50;check(app::visible_door_entries(c,0,{10,90}).size()==2,"category access failure hides only its entry");
    const auto encoded=app::encode_door_catalog(c);const auto decoded=app::decode_door_catalog(encoded);check(decoded&&decoded->menus[1].access.level==50&&decoded->menus[1].execution.level==50,"category requirements survive catalog encoding");
    app::DoorConfigurationChange change;change.operation.bytes[0]=1;change.expected_revision=c.revision;change.action=app::DoorConfigurationAction::save_entry;change.entry=c.entries[0];change.order={21,42,7};
    const auto arranged=app::changed_door_catalog(c,change);check(arranged.has_value(),"explicit reorder accepted");
    const auto rows=app::visible_door_entries(*arranged,0,{90,90});check(rows.size()==3&&rows[0].number==7&&rows[0].target==2&&rows[1].number==21&&rows[1].kind==app::DoorEntryKind::door&&rows[2].number==42&&rows[2].target==1,"root order changes by permuting existing gapped caller numbers");
    check(arranged->doors.size()==c.doors.size()&&arranged->menus.size()==c.menus.size()&&rows[2].description=="keep one","reorder retains identities, requirements and complete descriptions");
    const auto wire=app::encode_door_configuration_change(change);const auto roundtrip=app::decode_door_configuration_change(wire);check(roundtrip&&roundtrip->order==change.order,"full explicit permutation bound by strict IPC");
    for(std::size_t n=0;n<wire.size();++n){auto short_record=wire;short_record.resize(n);check(!app::decode_door_configuration_change(short_record),"truncated ordering request refused");}
    change.order={21,21,7};check(!app::valid_door_change(change),"duplicate ordering identities refused");change.order={21,7};check(!app::changed_door_catalog(c,change),"partial permutation cannot silently drop entries");
    change.order.clear();change.move_entry=true;change.source_menu=0;change.previous_number=7;change.create_only=true;change.entry.menu=2;change.entry.number=1;
    const auto moved=app::changed_door_catalog(c,change);check(moved&&std::none_of(moved->entries.begin(),moved->entries.end(),[](const auto& e){return e.menu==0&&e.number==7;})&&std::any_of(moved->entries.begin(),moved->entries.end(),[](const auto& e){return e.menu==2&&e.number==1&&e.target==1;}),"cross-category move changes both placements as one validated catalog");
    change.entry.menu=1;check(!app::changed_door_catalog(c,change),"moving a category into itself refused without changing source");
    check(app::encode_door_catalog(c)==encoded,"failed changes leave original catalog and policies unchanged");
}
}
