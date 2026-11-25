#include "sfx/application/ftn.hpp"
#include <limits>
namespace sfx::application {
namespace {
struct Put {
    std::vector<std::uint8_t> bytes; bool ok{true};
    void n(std::uint64_t v,unsigned count){if(count>kFtnConfigurationMaximumBytes-bytes.size()){ok=false;return;}for(unsigned i=0;i<count;++i)bytes.push_back(static_cast<std::uint8_t>(v>>(i*8)));}
    void data(const std::uint8_t* p,std::size_t size){if(size>65535||size+2>kFtnConfigurationMaximumBytes-bytes.size()){ok=false;return;}n(size,2);if(size)bytes.insert(bytes.end(),p,p+size);}
    void s(std::string_view v){data(reinterpret_cast<const std::uint8_t*>(v.data()),v.size());}
    void b(const std::vector<std::uint8_t>& v){data(v.data(),v.size());}
    void strings(const std::vector<std::string>& v){n(v.size(),2);for(const auto& s:v)this->s(s);}
};
struct Get {
    const std::vector<std::uint8_t>& bytes; std::size_t pos{}; bool ok{true};
    std::uint64_t n(unsigned count){if(count>bytes.size()-pos){ok=false;return 0;}std::uint64_t v{};for(unsigned i=0;i<count;++i)v|=static_cast<std::uint64_t>(bytes[pos++])<<(i*8);return v;}
    bool flag(){const auto v=n(1);if(v>1)ok=false;return v==1;}
    std::vector<std::uint8_t> data(std::size_t max){const auto length=n(2);if(length>max||length>bytes.size()-pos){ok=false;return {};}std::vector<std::uint8_t> out(bytes.begin()+pos,bytes.begin()+pos+static_cast<std::size_t>(length));pos+=static_cast<std::size_t>(length);return out;}
    std::string s(std::size_t max){const auto v=data(max);return {v.begin(),v.end()};}
    std::vector<std::string> strings(){std::vector<std::string> out;const auto count=n(2);if(count>32){ok=false;return out;}for(std::uint64_t i=0;i<count&&ok;++i)out.push_back(s(96));return out;}
    std::int64_t id(){const auto v=n(8);if(v>static_cast<std::uint64_t>(INT64_MAX))ok=false;return static_cast<std::int64_t>(v);}
};
Put setup_extension(const FtnConfiguration& c){
    Put w;
    for(const auto& n:c.networks){w.n(static_cast<unsigned>(n.echo_list_source),1);w.s(n.echo_list_file);w.s(n.nodelist_file);w.s(n.default_link);w.n(n.msg_netmail_enabled?(*n.msg_netmail_enabled?2:1):0,1);w.s(n.msg_netmail_directory);}
    for(const auto& l:c.links)w.n(l.contact_on_mail,1);
    for(const auto& e:c.message_echoes){w.n(e.maximum_messages.value_or(0),4);w.n(e.maximum_age_days.value_or(0),4);w.n(e.update_local_retention,1);w.n(e.ansi_colors?(*e.ansi_colors?2:1):0,1);}
    w.n(c.setup_subscriptions.has_value(),1);if(c.setup_subscriptions){const auto& r=*c.setup_subscriptions;w.s(r.network);w.s(r.link);w.n(r.tags.size(),4);for(const auto& tag:r.tags)w.s(tag);}
    return w;
}
}
std::vector<std::uint8_t> encode_ftn_configuration(const FtnConfiguration& c){
    if(!validate_ftn_configuration(c).empty())return {};
    Put w;w.n(7,1);w.n(static_cast<std::uint64_t>(c.revision),8);w.n(static_cast<std::uint64_t>(c.message_catalog_revision),8);w.n(static_cast<std::uint64_t>(c.file_catalog_revision),8);
    const auto& s=c.settings;w.n(static_cast<unsigned>(s.mode),1);w.n(s.listener_enabled,1);w.s(s.bind_address);w.n(s.port,2);w.n(s.allow_direct_local_netmail,1);w.s(s.external_executable);
    w.n(c.networks.size(),4);for(const auto& v:c.networks){w.s(v.code);w.s(v.name);w.s(v.address);w.s(v.origin_line);w.s(v.echo_list_url);w.strings(v.additional_addresses);w.n(v.enabled,1);w.n(v.keep_processed,1);w.n(v.retention_days,2);w.n(static_cast<std::uint64_t>(v.message_group),8);w.s(v.new_group_name);}
    w.n(c.links.size(),4);for(const auto& v:c.links){w.s(v.code);w.s(v.network);w.s(v.name);w.s(v.address);w.s(v.host);w.strings(v.additional_addresses);w.n(static_cast<unsigned>(v.use),1);w.n(v.port,2);w.n(v.poll_minutes,2);w.n(v.enabled,1);w.n(v.tls,1);w.n(v.allow_legacy_plain_auth,1);w.n(v.forward_missing_echoes,1);w.n(v.areafix_enabled?(*v.areafix_enabled?2:1):0,1);w.b(v.credentials.session);w.b(v.credentials.packet);w.b(v.credentials.areafix);w.b(v.credentials.tic);}
    w.n(c.message_echoes.size(),4);for(const auto& v:c.message_echoes){w.s(v.network);w.s(v.tag);w.s(v.description);w.n(static_cast<std::uint64_t>(v.local_echo),8);w.n(v.store_locally,1);w.n(v.require_real_name,1);w.n(v.access_level,2);w.n(v.read_level,2);w.n(v.post_level,2);w.s(v.local_name);w.n(v.update_local_name,1);}
    w.n(c.file_echoes.size(),4);for(const auto& v:c.file_echoes){w.s(v.network);w.s(v.tag);w.s(v.description);w.n(static_cast<std::uint64_t>(v.file_area),8);}
    w.n(c.permissions.size(),4);for(const auto& v:c.permissions){w.s(v.network);w.s(v.link);w.s(v.tag);w.n(v.file_echo,1);w.n(v.receive,1);w.n(v.send,1);w.n(v.manage,1);}
    w.n(c.routes.size(),4);for(const auto& v:c.routes){w.s(v.network);w.s(v.destination);w.s(v.via_link);w.n(static_cast<unsigned>(v.flavor),1);}
    w.s(c.settings.tls_certificate);w.s(c.settings.tls_private_key);
    for(const auto& v:c.links){w.s(v.tls_server_name);w.s(v.tls_ca_file);w.s(v.forward_via_link);}
    for(const auto& v:c.message_echoes){w.n(v.pipe_colors,1);w.n(v.update_local_permissions,1);}
    for(const auto& v:c.networks)w.s(v.description);
    w.n(c.settings.tls_listener_enabled,1);w.n(c.settings.tls_port,2);
    w.n(c.settings.msg_netmail_enabled,1);w.s(c.settings.msg_netmail_directory);
    const auto extra=setup_extension(c);
    if(!extra.ok||extra.bytes.size()>kFtnConfigurationMaximumBytes-w.bytes.size())return {};
    w.bytes.insert(w.bytes.end(),extra.bytes.begin(),extra.bytes.end());
    return w.ok?std::move(w.bytes):std::vector<std::uint8_t>{};
}
std::vector<std::uint8_t> encode_ftn_configuration_legacy(const FtnConfiguration& c,std::uint8_t version){
    if(c.setup_subscriptions)return {};
    if(version<2||version>6)return {};
    for(const auto& n:c.networks)if(n.echo_list_source!=FtnEchoListSource::url||!n.echo_list_file.empty()||!n.nodelist_file.empty()||!n.default_link.empty()||n.msg_netmail_enabled||!n.msg_netmail_directory.empty())return {};
    for(const auto& l:c.links)if(l.contact_on_mail)return {};
    for(const auto& e:c.message_echoes)if(e.update_local_retention||e.ansi_colors)return {};
    auto bytes=encode_ftn_configuration(c);const auto extra=setup_extension(c);
    if(bytes.empty()||extra.bytes.size()>=bytes.size())return {};
    bytes.resize(bytes.size()-extra.bytes.size());bytes[0]=6;
    if(version==6)return bytes;
    if(c.settings.msg_netmail_enabled||!c.settings.msg_netmail_directory.empty()||bytes.size()<4)return {};
    bytes.resize(bytes.size()-3);bytes[0]=5;if(version==5)return bytes;
    if(c.settings.tls_listener_enabled||c.settings.tls_port!=24555||bytes.size()<4)return {};
    bytes.resize(bytes.size()-3);bytes[0]=4;if(version==4)return bytes;
    for(const auto& n:c.networks)if(!n.description.empty())return {};
    if(bytes.size()<1+c.networks.size()*2)return {};
    bytes.resize(bytes.size()-c.networks.size()*2);bytes[0]=3;if(version==3)return bytes;
    if(!c.settings.tls_certificate.empty()||!c.settings.tls_private_key.empty())return {};
    for(const auto& l:c.links)if(!l.tls_server_name.empty()||!l.tls_ca_file.empty()||!l.forward_via_link.empty())return {};
    for(const auto& e:c.message_echoes)if(e.pipe_colors||e.update_local_permissions)return {};
    const auto size=4+c.links.size()*6+c.message_echoes.size()*2;if(bytes.size()<1+size)return {};
    bytes.resize(bytes.size()-size);bytes[0]=2;return bytes;
}
std::optional<FtnConfiguration> decode_ftn_configuration(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>kFtnConfigurationMaximumBytes)return {};
    Get r{bytes};const auto version=r.n(1);if(version<2||version>7)return {};
    FtnConfiguration c;c.revision=r.id();c.message_catalog_revision=r.id();c.file_catalog_revision=r.id();auto& s=c.settings;
    s.mode=static_cast<FtnMailerMode>(r.n(1));s.listener_enabled=r.flag();s.bind_address=r.s(253);s.port=static_cast<std::uint16_t>(r.n(2));s.allow_direct_local_netmail=r.flag();s.external_executable=r.s(1024);
    auto count=r.n(4);if(count>128)return {};
    for(std::uint64_t i=0;i<count&&r.ok;++i){FtnNetwork v;v.code=r.s(48);v.name=r.s(96);v.address=r.s(96);v.origin_line=r.s(128);v.echo_list_url=r.s(2048);v.additional_addresses=r.strings();v.enabled=r.flag();v.keep_processed=r.flag();v.retention_days=static_cast<std::uint16_t>(r.n(2));v.message_group=r.id();v.new_group_name=r.s(64);c.networks.push_back(std::move(v));}
    count=r.n(4);if(count>4096)return {};
    for(std::uint64_t i=0;i<count&&r.ok;++i){FtnLink v;v.code=r.s(48);v.network=r.s(48);v.name=r.s(96);v.address=r.s(96);v.host=r.s(253);v.additional_addresses=r.strings();v.use=static_cast<FtnConnectionUse>(r.n(1));v.port=static_cast<std::uint16_t>(r.n(2));v.poll_minutes=static_cast<std::uint16_t>(r.n(2));v.enabled=r.flag();v.tls=r.flag();v.allow_legacy_plain_auth=r.flag();v.forward_missing_echoes=r.flag();const auto flag=r.n(1);if(flag>2)r.ok=false;if(flag)v.areafix_enabled=flag==2;v.credentials.session=r.data(4096);v.credentials.packet=r.data(4096);v.credentials.areafix=r.data(4096);v.credentials.tic=r.data(4096);c.links.push_back(std::move(v));}
    count=r.n(4);if(count>16384)return {};
    for(std::uint64_t i=0;i<count&&r.ok;++i){FtnMessageEcho v;v.network=r.s(48);v.tag=r.s(128);v.description=r.s(512);v.local_echo=r.id();v.store_locally=r.flag();v.require_real_name=r.flag();v.access_level=static_cast<std::uint16_t>(r.n(2));v.read_level=static_cast<std::uint16_t>(r.n(2));v.post_level=static_cast<std::uint16_t>(r.n(2));v.local_name=r.s(64);v.update_local_name=r.flag();c.message_echoes.push_back(std::move(v));}
    count=r.n(4);if(count>16384)return {};
    for(std::uint64_t i=0;i<count&&r.ok;++i){FtnFileEcho v;v.network=r.s(48);v.tag=r.s(128);v.description=r.s(512);v.file_area=r.id();c.file_echoes.push_back(std::move(v));}
    count=r.n(4);if(count>65536)return {};
    for(std::uint64_t i=0;i<count&&r.ok;++i){FtnEchoPermission v;v.network=r.s(48);v.link=r.s(48);v.tag=r.s(128);v.file_echo=r.flag();v.receive=r.flag();v.send=r.flag();v.manage=r.flag();c.permissions.push_back(std::move(v));}
    count=r.n(4);if(count>16384)return {};
    for(std::uint64_t i=0;i<count&&r.ok;++i){FtnRoute v;v.network=r.s(48);v.destination=r.s(96);v.via_link=r.s(48);v.flavor=static_cast<FtnMailFlavor>(r.n(1));c.routes.push_back(std::move(v));}
    if(version>=3){c.settings.tls_certificate=r.s(1024);c.settings.tls_private_key=r.s(1024);for(auto& v:c.links){v.tls_server_name=r.s(253);v.tls_ca_file=r.s(1024);v.forward_via_link=r.s(48);}for(auto& v:c.message_echoes){v.pipe_colors=r.flag();v.update_local_permissions=r.flag();}}
    if(version>=4)for(auto& v:c.networks)v.description=r.s(256);
    if(version>=5){c.settings.tls_listener_enabled=r.flag();c.settings.tls_port=static_cast<std::uint16_t>(r.n(2));}
    if(version>=6){c.settings.msg_netmail_enabled=r.flag();c.settings.msg_netmail_directory=r.s(1024);}
    if(version>=7){
        for(auto& n:c.networks){n.echo_list_source=static_cast<FtnEchoListSource>(r.n(1));n.echo_list_file=r.s(1024);n.nodelist_file=r.s(1024);n.default_link=r.s(48);const auto flag=r.n(1);if(flag>2)r.ok=false;if(flag)n.msg_netmail_enabled=flag==2;n.msg_netmail_directory=r.s(1024);}
        for(auto& l:c.links)l.contact_on_mail=r.flag();
        for(auto& e:c.message_echoes){const auto limit=r.n(4),age=r.n(4);e.maximum_messages=limit?std::optional<std::uint32_t>(static_cast<std::uint32_t>(limit)):std::nullopt;e.maximum_age_days=age?std::optional<std::uint32_t>(static_cast<std::uint32_t>(age)):std::nullopt;e.update_local_retention=static_cast<std::uint8_t>(r.n(1));const auto flag=r.n(1);if(flag>2)r.ok=false;if(flag)e.ansi_colors=flag==2;}
        if(r.flag()){FtnSetupSubscriptions request;request.network=r.s(48);request.link=r.s(48);const auto tags=r.n(4);if(tags>16384)return {};for(std::uint64_t i=0;i<tags&&r.ok;++i)request.tags.push_back(r.s(128));c.setup_subscriptions=std::move(request);}
    }
    if(!r.ok||r.pos!=bytes.size()||!validate_ftn_configuration(c).empty())return {};
    return c;
}
} // namespace sfx::application
