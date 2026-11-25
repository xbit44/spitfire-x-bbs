#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <shellapi.h>
#include <string>
#include <array>
// Synthetic local-control peer only. Never a Runtime, public listener or board.
int wmain(int argc,wchar_t** argv){try{
    namespace native=sfx::platform::win32;namespace app=sfx::application;
    HANDLE owner=INVALID_HANDLE_VALUE;std::filesystem::path root;
    for(int n=1;n<argc;++n){const std::wstring argument=argv[n];
        if(argument.rfind(L"--owner-handle=",0)==0)owner=reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(std::stoull(argument.substr(15))));
        if(argument.rfind(L"--fixture-root=",0)==0)root=argument.substr(15);}
    if(root.empty()){Sleep(750);return 0;}
    DWORD owner_pid{},count{};std::array<std::uint8_t,app::kLocalControlNonceBytes> nonce{};
    if(owner==INVALID_HANDLE_VALUE||!ReadFile(owner,&owner_pid,sizeof(owner_pid),&count,nullptr)||count!=sizeof(owner_pid)||
       !ReadFile(owner,nonce.data(),static_cast<DWORD>(nonce.size()),&count,nullptr)||count!=nonce.size())return 2;
    CloseHandle(owner);
    const auto names=native::installation_object_names(root);
    auto connection=native::accept_local_control_client(names.pipe_name,native::current_process_user_sid_string(),owner_pid,15000);
    if(!connection.valid())return 3;
    app::Identifier128 epoch;epoch.bytes[0]=0x77;
    app::AlphaStatusSnapshotCache cache;std::uint64_t sequence{};bool authenticated{};
    app::LocalControlFrame request;
    while(connection.read_frame(request)){
        app::LocalControlFrame response;response.request_id=request.request_id;
        response.type=static_cast<app::LocalControlMessageType>(static_cast<unsigned>(request.type)+1);
        bool stop{};
        if(!authenticated){std::array<std::uint8_t,app::kLocalControlNonceBytes> supplied{};
            if(request.type!=app::LocalControlMessageType::hello||!app::decode_hello_payload(request.payload,supplied)||supplied!=nonce)return 4;
            app::AlphaControlResponse accepted{app::LocalControlResponseCode::ok,"synthetic_ready"};accepted.runtime_epoch=epoch;
            response.payload=app::encode_control_response(accepted);authenticated=true;SecureZeroMemory(nonce.data(),nonce.size());
        }else if(request.type==app::LocalControlMessageType::status_request){
            const auto query=app::decode_status_request(request.payload);if(!query)return 5;
            if(!query->snapshot_sequence){
                app::AlphaRuntimeStatus status;status.runtime_epoch=epoch;status.status_sequence=++sequence;
                status.state=app::AlphaRuntimePresentationState::ready;status.lifecycle.state=app::RuntimeState::online;
                status.lifecycle.owner_mode=app::RuntimeOwnershipMode::desktop;status.lifecycle.configured_nodes=4;
                status.configuration_loaded=true;status.bootstrap_complete=true;status.bbs_name="Synthetic startup handoff";
                status.bbs_time_zone="UTC";status.security_policy_revision=1;status.security_policy_hash=std::string(64,'A');
                status.nodes.resize(4);for(std::size_t n=0;n<status.nodes.size();++n){auto& node=status.nodes[n];node.number=static_cast<std::uint16_t>(n+1);node.available=true;node.enabled=true;}
                status.nodes.front().state=sfx::core::NodeState::ready;status.storage.state=app::StorageRuntimeState::ready;
                status.storage.schema_version=1;status.storage.engine_version="3.53.4";status.log_health=0;
                status.diagnostic_code="synthetic_ready_no_public_listeners";
                if(!cache.capture(std::move(status),query->screen_node))return 6;
            }
            response.payload=cache.chunk(*query);
        }else if(request.type==app::LocalControlMessageType::shutdown_request){
            app::AlphaControlResponse stopped{app::LocalControlResponseCode::ok,"synthetic_stopped"};stopped.runtime_epoch=epoch;
            response.payload=app::encode_control_response(stopped);stop=true;
        }
        if(!connection.write_frame(response))break;if(stop)break;
    }
    return 0;
}catch(...){return 9;}}
