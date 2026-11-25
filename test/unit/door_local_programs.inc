#include "sfx/application/door_execution.hpp"
namespace {
void local_door_model_checks(){
    auto d=door();d.kind=app::DoorExecutionKind::local;d.rlogin.host.clear();d.code="OUTPOST";d.program.command_line="otrader /dd%N";d.program.startup_directory="Doors/Outpost";d.program.drop_file=app::DoorDropFile::door_sys52;
    check(app::door_accessible(d,{10,90}),"local Door visibility uses its local command");
    app::DoorCatalog catalog;catalog.doors={d};catalog.entries={{0,1,app::DoorEntryKind::door,1,"Outpost",""}};
    catalog.doors[0].event_only=true;check(app::visible_door_entries(catalog,0,{10,90}).empty(),"event-only Door hidden from caller menu");catalog.doors[0].event_only=false;
    auto clone=d;clone.id=2;clone.code="outpost";catalog.doors.push_back(clone);check(!app::validate_door_catalog(catalog).valid(),"case-insensitive shared Door code uniqueness");
    clone.code=app::suggest_door_code(catalog,"Outpost",2);check(clone.code=="OUTPOST_1","name-derived unique code suggestion");catalog.doors[1]=clone;check(app::validate_door_catalog(catalog).valid(),"separate definitions use separate codes");
    d.program.command_line=std::string(4096,'X');d.program.cleanup_command=std::string(4096,'Y');d.program.startup_directory=std::string(1024,'Z');d.rlogin.protected_credential.assign(4096,4);
    const auto encoded=app::encode_door_definition(d);check(encoded.size()>10000&&encoded.size()<32768,"v3 accommodates real local settings without old10KB truncation");
    const auto read=app::decode_door_definition(encoded);check(read&&app::encode_door_definition(*read)==encoded,"complete local settings and protected remote data round trip");
    for(std::size_t n=0;n<encoded.size();n+=23){auto bad=encoded;bad.resize(n);check(!app::decode_door_definition(bad),"truncated v3 local record refused");}
    d.program.type=app::DoorProgramType::native;check(!app::valid_door_definition(d),"Native FOSSIL combination refused");d.program.io=app::DoorIo::socket;check(app::valid_door_definition(d),"Native Socket combination accepted");
    app::DoorReservations reservations;auto multi=reservations.acquire(1,true);check(bool(multi),"multiuser ownership captured");check(!reservations.acquire(1,false),"new exclusive snapshot waits for earlier multiuser cleanup");auto other=reservations.acquire(1,true);check(bool(other),"parallel multiuser allowed");multi->release_after_cleanup();other->release_after_cleanup();auto exclusive=reservations.acquire(1,false);check(bool(exclusive)&&!reservations.acquire(1,true),"active exclusive snapshot blocks shared placement launch");exclusive->release_after_cleanup();
    {auto incomplete=reservations.acquire(1,false);check(bool(incomplete),"cleanup failure fixture acquired");}check(!reservations.acquire(1,true),"unconfirmed cleanup quarantines Door reservation");check(bool(reservations.acquire(2,true)),"other definitions unaffected by quarantine");
    app::DoorCommandPaths paths{"C:/SFX/Node/2","C:/SFX/Node/2","C:/Games/Outpost","OUTPOST",2};
    const auto command=app::expand_door_command("game /n%# /d\"%D\" %% %C",paths);check(command&&*command=="game /n2 /d\"C:/SFX/Node/2\" % OUTPOST","normal command substitutions use actual owned paths and node");check(!app::expand_door_command("game %PASSWORD",paths),"unknown substitutions refused, no caller credential interpolation");
}
}
