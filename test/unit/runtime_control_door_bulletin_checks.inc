// Exercise the actual shared dispatcher; the platform authenticates its caller.
struct DoorBulletinDispatchFixture : DispatchFixture {
    unsigned queries{},changes{};
    app::LocalDoorBulletinQuery last_query;
    app::DoorBulletinChange last_change;
    app::LocalDoorSnapshot door_bulletin_snapshot(const app::LocalDoorBulletinQuery& query){
        ++calls;++queries;last_query=query;return {};
    }
    app::LocalDoorResult change_door_bulletin(const app::DoorBulletinChange& change){
        ++calls;++changes;last_change=change;return {app::LocalAccountResultCode::conflict};
    }
};
void door_bulletin_dispatch_checks(){
    DoorBulletinDispatchFixture host;app::AlphaStatusSnapshotCache cache;
    app::LocalControlFrame request;request.request_id=771;
    for(const auto kind:{app::LocalDoorBulletinQueryKind::catalog,app::LocalDoorBulletinQueryKind::validate,app::LocalDoorBulletinQueryKind::preview}){
        app::LocalDoorBulletinQuery query;query.kind=kind;query.revision=7;query.ansi=false;
        if(kind!=app::LocalDoorBulletinQueryKind::catalog)query.id={999};
        request.type=app::LocalControlMessageType::door_bulletin_query_request;request.payload=app::encode_door_bulletin_query(query);
        check(!request.payload.empty(),"valid typed Door Bulletin query encoded");
        const auto response=rt::dispatch_runtime_control(host,request,cache);
        check(response&&response->type==app::LocalControlMessageType::door_bulletin_query_response&&response->request_id==771&&request.payload.empty(),"Door Bulletin query dispatch correlates and erases request");
        check(host.last_query.kind==query.kind&&host.last_query.id==query.id&&host.last_query.revision==7&&!host.last_query.ansi,"query kind, full ID, revision and mode reach actual host port");
        const auto decoded=app::decode_door_configuration_snapshot(response->payload);
        check(decoded&&decoded->code==app::LocalAccountResultCode::unavailable,"unavailable host result never promoted to success");
    }
    check(host.calls==3&&host.queries==3,"each query invokes exactly one host operation");
    for(const auto type:{app::LocalControlMessageType::door_bulletin_query_request,app::LocalControlMessageType::door_bulletin_change_request}){
        request.type=type;request.payload={255};const auto before=host.calls;
        const auto response=rt::dispatch_runtime_control(host,request,cache);
        check(host.calls==before&&request.payload.empty(),"malformed Door Bulletin request invokes no host and is erased");
        check(response&&response->request_id==771,"malformed response retains correlation");
        if(type==app::LocalControlMessageType::door_bulletin_query_request){
            const auto decoded=app::decode_door_configuration_snapshot(response->payload);
            check(response->type==app::LocalControlMessageType::door_bulletin_query_response&&decoded&&decoded->code==app::LocalAccountResultCode::invalid_request,"bad query returns typed refusal");
        }else{
            const auto decoded=app::decode_door_configuration_result(response->payload);
            check(response->type==app::LocalControlMessageType::door_bulletin_change_response&&decoded&&decoded->code==app::LocalAccountResultCode::invalid_request,"bad change returns typed refusal");
        }
    }
    app::DoorBulletinChange change;change.operation.bytes[0]=1;change.expected_revision=7;change.create_only=true;change.definition.id={999};change.definition.title="Scores";
    request.type=app::LocalControlMessageType::door_bulletin_change_request;request.payload=app::encode_door_bulletin_change(change);
    check(!request.payload.empty(),"valid Door Bulletin change encoded");
    const auto response=rt::dispatch_runtime_control(host,request,cache);
    check(response&&response->type==app::LocalControlMessageType::door_bulletin_change_response&&response->request_id==771&&request.payload.empty(),"change dispatch correlates and erases request");
    check(host.calls==4&&host.changes==1&&host.last_change.definition.id.value==999&&host.last_change.expected_revision==7&&host.last_change.create_only,"full permanent ID and optimistic revision reach one host change");
    const auto decoded=app::decode_door_configuration_result(response->payload);
    check(decoded&&decoded->code==app::LocalAccountResultCode::conflict,"host conflict remains a refusal without retry or invented success");
}
