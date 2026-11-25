#include "sfx/platform/win32/file_administration_client.hpp"
#include "sfx/platform/win32/offline_file_administration.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include <algorithm>
#include <array>
namespace sfx::platform::win32 {
namespace {
namespace app=application;namespace ins=installation;
struct PanelSearch {const ins::Receipt& receipt;DWORD pid{};};
BOOL CALLBACK find_panel(HWND window,LPARAM parameter){
    auto& search=*reinterpret_cast<PanelSearch*>(parameter);std::array<wchar_t,128> name{};
    if(!GetClassNameW(window,name.data(),static_cast<int>(name.size()))||wcscmp(name.data(),L"SFXControlPanelWindow"))return TRUE;
    DWORD pid{};GetWindowThreadProcessId(window,&pid);
    try{ins::verify_installed_program_process(search.receipt,pid,ins::control_panel_program_index);search.pid=pid;return FALSE;}catch(...){return TRUE;}
}
DWORD panel(const ins::Receipt& receipt){PanelSearch search{receipt};EnumWindows(find_panel,reinterpret_cast<LPARAM>(&search));return search.pid;}
class Online final : public IFileAdministrationClient {
    ins::Receipt receipt_;
    std::wstring pipe_;
    std::uint32_t next_{1};
    bool exchange(app::LocalControlMessageType type,std::vector<std::uint8_t> payload,app::LocalControlMessageType expected,app::LocalControlFrame& response){
        try{
            if(!app::file_manager_control_request(type)||payload.empty())return false;
            const auto pid=panel(receipt_);if(!pid)return false;auto connection=connect_local_control_server(pipe_,pid,1500);
            app::LocalControlFrame request;request.type=type;request.request_id=next_++;if(!next_)next_=1;request.payload=std::move(payload);
            return connection.valid()&&connection.write_frame(request)&&connection.read_frame(response)&&response.type==expected&&response.request_id==request.request_id;
        }catch(...){return false;}
    }
    app::LocalFileConfigurationSnapshot part(const app::LocalFileConfigurationQuery& query){
        app::LocalControlFrame response;if(!exchange(app::LocalControlMessageType::file_configuration_query_request,app::encode_file_configuration_query(query),app::LocalControlMessageType::file_configuration_query_response,response))return {};
        return app::decode_file_configuration_snapshot(response.payload).value_or(app::LocalFileConfigurationSnapshot{});
    }
    app::FileManagementJobStatus job(app::LocalControlMessageType type,std::vector<std::uint8_t> payload,app::LocalControlMessageType expected){
        if(payload.empty())return {app::LocalAccountResultCode::invalid_request};app::LocalControlFrame response;if(!exchange(type,std::move(payload),expected,response))return {};return app::decode_file_management_status(response.payload).value_or(app::FileManagementJobStatus{});
    }
public:
    explicit Online(ins::Receipt receipt):receipt_(std::move(receipt)),pipe_(installation_object_names(receipt_.root).configuration_pipe_name){}
    bool online()const noexcept override{return true;}
    std::optional<app::FtnHatchCatalog> ftn_hatch_catalog()override{
        app::FtnConfigurationRequest query;query.token=WindowsSecureIdentifierGenerator{}.generate().bytes;std::vector<std::uint8_t> bytes;std::int64_t revision{};std::uint32_t total{};
        const auto invoke=[&](const app::FtnConfigurationRequest& q)->std::optional<app::FtnConfigurationResponse>{app::LocalControlFrame frame;if(!exchange(app::LocalControlMessageType::ftn_hatch_catalog_request,app::encode_ftn_configuration_request(q),app::LocalControlMessageType::ftn_hatch_catalog_response,frame))return {};return app::decode_ftn_configuration_response(frame.payload);};
        const auto cancel=[&]{auto stop=query;stop.operation=app::FtnConfigurationOperation::cancel;stop.offset=stop.total=0;(void)invoke(stop);};
        for(;;){const auto page=invoke(query);if(!page||page->code!=app::FtnConfigurationCode::ok||page->revision<1||page->bytes.empty()||page->offset!=bytes.size()||(revision&&(page->revision!=revision||page->total!=total))){cancel();return {};}if(!revision){revision=page->revision;total=page->total;bytes.reserve(total);}bytes.insert(bytes.end(),page->bytes.begin(),page->bytes.end());if(bytes.size()==total)break;query.offset=static_cast<std::uint32_t>(bytes.size());query.revision=revision;}cancel();const auto result=app::decode_ftn_hatch_catalog(bytes);return result&&result->revision==revision?result:std::nullopt;
    }
    std::optional<app::FileConfiguration> configuration()override{
        app::FileConfiguration value;std::int64_t revision{};
        for(const auto kind:{app::FileConfigurationPart::groups,app::FileConfigurationPart::areas}){
            app::LocalFileConfigurationQuery query;query.part=kind;query.catalog_revision=revision;bool complete{};
            for(unsigned page=0;page<84;++page){
                const auto read=part(query);if(read.code!=app::LocalAccountResultCode::ok||read.part!=kind||(revision&&revision!=read.policy.catalog_revision))return {};
                revision=read.policy.catalog_revision;value.policy=read.policy;value.groups.insert(value.groups.end(),read.groups.begin(),read.groups.end());value.areas.insert(value.areas.end(),read.areas.begin(),read.areas.end());
                if(value.groups.size()>999||value.areas.size()>999)return {};if(!read.next_after){complete=true;break;}if(read.next_after<=query.after_id)return {};query.catalog_revision=revision;query.after_id=read.next_after;
            }
            if(!complete)return {};
        }
        return value;
    }
    app::FileManagementReadSnapshot read(const app::FileManagementReadRequest& query)override{
        const auto payload=app::encode_file_management_read_request(query);if(payload.empty())return {app::LocalAccountResultCode::invalid_request};app::LocalControlFrame response;if(!exchange(app::LocalControlMessageType::file_management_query_request,payload,app::LocalControlMessageType::file_management_query_response,response))return {};return app::decode_file_management_read_snapshot(response.payload).value_or(app::FileManagementReadSnapshot{});
    }
    app::FileManagementJobStatus start(const app::FileManagementTask& task)override{return job(app::LocalControlMessageType::file_management_start_request,app::encode_file_management_task(task),app::LocalControlMessageType::file_management_start_response);}
    app::FileManagementJobStatus status(app::Identifier128 id)override{return job(app::LocalControlMessageType::file_management_status_request,app::encode_file_management_job(id),app::LocalControlMessageType::file_management_status_response);}
    app::FileManagementJobStatus cancel(app::Identifier128 id)override{return job(app::LocalControlMessageType::file_management_cancel_request,app::encode_file_management_job(id),app::LocalControlMessageType::file_management_cancel_response);}
};
class Offline final : public IFileAdministrationClient {
    std::unique_ptr<OfflineFileAdministration> session_;
public:
    explicit Offline(std::unique_ptr<OfflineFileAdministration> session):session_(std::move(session)){}
    bool online()const noexcept override{return false;}
    std::string finish()override{return session_->finish();}
    std::optional<app::FtnHatchCatalog> ftn_hatch_catalog()override{return session_->ftn_hatch_catalog();}
    std::optional<app::FileConfiguration> configuration()override{const auto value=session_->configuration_catalog();return value&&value.value?value.value:std::nullopt;}
    app::FileManagementReadSnapshot read(const app::FileManagementReadRequest& query)override{return session_->query(query);}
    app::FileManagementJobStatus start(const app::FileManagementTask& task)override{return session_->start(task);}
    app::FileManagementJobStatus status(app::Identifier128 id)override{return session_->status(id);}
    app::FileManagementJobStatus cancel(app::Identifier128 id)override{return session_->cancel(id);}
};
}
std::unique_ptr<IFileAdministrationClient> open_file_administration_client(const std::filesystem::path& root,const std::wstring& owner,std::string& diagnostic){
    try{
        ins::require(owner==ins::operator_sid()&&!ins::elevated(),"file_manager_requires_ordinary_installation_owner");
        auto receipt=ins::read_receipt(root,owner);ins::verify_structure(receipt);ins::require(receipt.program_count>ins::file_manager_program_index,"file_manager_requires_matched_setup");
        if(panel(receipt)){
            auto client=std::make_unique<Online>(std::move(receipt));if(client->read({}).code!=app::LocalAccountResultCode::ok){diagnostic="Running owner relay unavailable or not ready; no offline fallback was attempted.";return {};}
            diagnostic.clear();return client;
        }
        auto session=OfflineFileAdministration::begin(root,owner,diagnostic);if(!session)return {};diagnostic.clear();return std::make_unique<Offline>(std::move(session));
    }catch(const std::exception& e){diagnostic=app::sanitize_file_text(e.what(),256,false);return {};}catch(...){diagnostic="file_manager_connection_unavailable";return {};}
}
} // namespace sfx::platform::win32
