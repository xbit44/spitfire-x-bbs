// Only the fresh owner-fixes fixture; no owner or held Section14 installation.
#include "ftn_current_installed_client.inc"
#include "sfx/platform/win32/offline_account_administration.hpp"
int main()try{
    const auto root=ins::canonical_root(L"C:\\Program Files\\SFX-OwnerFixes-Test-20261031-r3");
    check(!ins::elevated(),"normal-user installed test");
    const auto receipt=ins::read_receipt(root,ins::operator_sid());check(receipt.generation==1,"fresh generation1 fixture");
    RuntimeSession runtime(root,true);
    app::AlphaBootstrapRequest q;q.configuration.bbs_name="Owner fixes isolated fixture";q.configuration.bbs_time_zone="Pacific Standard Time";q.configuration.logical_nodes=4;
    const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& query)->std::optional<std::vector<std::uint8_t>>{return runtime.exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(query));});
    check(status&&status->security_policy_revision>0&&status->security_policy_hash.size()==64,"authoritative policy from normal wizard status");q.configuration.security_policy_revision=status->security_policy_revision;q.configuration.security_policy_hash=status->security_policy_hash;
    q.configuration.telnet.bind_address="127.0.0.1";q.configuration.telnet.port=26481;q.configuration.telnet.bot_gate_enabled=false;q.configuration.botgate.enabled=false;q.configuration.ssh.enabled=false;
    q.sysop_name="Fixture Sysop";q.sysop_password="Synthetic-only-password-123";q.system_password="Synthetic-only-system-456";
    auto encoded=app::encode_bootstrap_request(q);check(!encoded.empty()&&app::decode_bootstrap_request(encoded).has_value(),"complete valid bootstrap frame before send");
    const auto response=app::decode_control_response(runtime.exchange(app::LocalControlMessageType::bootstrap_request,app::LocalControlMessageType::bootstrap_response,std::move(encoded)));
    app::erase_bootstrap_secrets(q);if(response)std::cout<<"BOOTSTRAP "<<static_cast<unsigned>(response->code)<<" "<<response->diagnostic_code<<std::endl;check(response&&response->code==app::LocalControlResponseCode::ok,"real bootstrap commits");runtime.stop();
    auto c=win::offline_ftn_configuration(root,ins::operator_sid());check(c&&c.value,"real offline FTN snapshot");c.value->settings.listener_enabled=false;c.value->settings.tls_listener_enabled=false;
    const auto saved=win::offline_change_ftn_configuration(root,ins::operator_sid(),token(),*c.value);check(saved&&saved.value,"disable isolated FTN listeners");
    std::cout<<"PASS "<<checks<<" fresh installed bootstrap checks; loopback only, no hub\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
