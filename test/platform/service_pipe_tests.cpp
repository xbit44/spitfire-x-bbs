#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <atomic>
#include <thread>
#include <exception>
#include <iostream>
namespace p=sfx::platform::win32;
namespace app=sfx::application;
namespace {unsigned checks{};void check(bool v,const char* why){++checks;p::installation::require(v,why);}}
int main(){try{
    for(const bool cancel:{false,true}) {
        const auto name=L"\\\\.\\pipe\\SFX-Section15-Pipe-QA-"+p::installation::unique_id();
        const auto owner=p::current_process_user_sid_string();
        std::atomic<bool> writing{false},done{false};bool delivered=false;std::exception_ptr error;
        std::thread server([&]{try{
            auto connection=p::accept_local_control_client(name,owner,GetCurrentProcessId(),3000);
            p::installation::require(connection.valid(),"owned native pipe accepted");
            app::LocalControlFrame frame;frame.type=app::LocalControlMessageType::hello;frame.request_id=1;
            frame.payload=app::encode_hello_payload({});writing.store(true);
            delivered=connection.write_frame(frame);
            // The peer deliberately remains alive without reading in the
            // cancelled case. Destruction must not re-enter a blocking drain.
            connection.disconnect();done.store(true);
        }catch(...){error=std::current_exception();done.store(true);}});
        auto client=p::connect_local_control_server(name,GetCurrentProcessId(),3000);
        bool accepted=client.valid();bool received=false;bool bounded=false;
        if(accepted) {
            if(cancel) {
                const auto deadline=GetTickCount64()+3000;
                while(!writing.load()&&!done.load()&&GetTickCount64()<deadline)Sleep(1);
                Sleep(25);const auto stop_deadline=GetTickCount64()+1500;
                while(!done.load()&&GetTickCount64()<stop_deadline) {
                    CancelSynchronousIo(server.native_handle());Sleep(10);
                }
                bounded=done.load();
            }else {
                app::LocalControlFrame response;
                received=client.read_frame(response)&&response.type==app::LocalControlMessageType::hello&&response.request_id==1;
            }
        }
        // Closing only our own test peer also releases a failed legacy drain,
        // allowing a failing regression test to join without an orphan process.
        client.disconnect();server.join();
        if(error)std::rethrow_exception(error);
        check(accepted,"same-owner native peer connected to isolated endpoint");
        if(cancel) {
            check(bounded,"cancelled server teardown stays bounded while client is alive and not reading");
            check(!delivered,"cancelled drain never claims successful frame delivery");
        }else check(received&&delivered,"normal completed frame is delivered before disconnect");
    }
    std::cout<<"PASS "<<checks<<" native service pipe checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
