#include "linux_web_access.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <sodium.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <charconv>
#include <set>
#include <array>
#include <algorithm>
#include <cstring>
#include <map>

namespace sfx::apps::runtime::web {
namespace {
namespace native=sfx::platform::linux_host;
using Json=nlohmann::json;
void require(bool value,const char* detail){if(!value)throw std::runtime_error(detail);}
void keys(const Json& value,std::initializer_list<const char*> names){require(value.is_object()&&value.size()==names.size(),"web_access_fields_invalid");for(const auto* name:names)require(value.contains(name),"web_access_fields_invalid");}
Json decode(const std::vector<std::uint8_t>& bytes){
    std::map<int,std::set<std::string>> fields;
    return Json::parse(bytes.begin(),bytes.end(),[&](int depth,Json::parse_event_t event,Json& value){
        require(depth<=4,"web_access_depth_invalid");if(event==Json::parse_event_t::object_start)fields[depth+1].clear();
        if(event==Json::parse_event_t::key)require(fields[depth].insert(value.get<std::string>()).second,"web_access_duplicate_field");
        return true;});
}
using Bio=std::unique_ptr<BIO,decltype(&BIO_free)>;
using Certificate=std::unique_ptr<X509,decltype(&X509_free)>;
using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
struct Secret {
    std::string bytes;
    ~Secret(){sodium_memzero(bytes.data(),bytes.size());}
};
Bio input(std::string_view bytes){require(bytes.size()<=1048576,"web_certificate_too_large");Bio result(BIO_new_mem_buf(bytes.data(),static_cast<int>(bytes.size())),BIO_free);require(bool(result),"web_certificate_buffer_failed");return result;}
std::string output(BIO* value){char* data{};const auto size=BIO_get_mem_data(value,&data);require(size>0&&size<=1048576,"web_certificate_encoding_failed");return {data,static_cast<std::size_t>(size)};}
int no_password(char*,int,int,void*){return 0;}
WebTlsIdentity identity(std::string_view cert_bytes,std::string_view key_bytes,std::string_view address){
    const auto canonical=web_server_name(address);auto certificates=input(cert_bytes),private_key=input(key_bytes);
    Certificate cert(PEM_read_bio_X509(certificates.get(),nullptr,nullptr,nullptr),X509_free);
    Key key(PEM_read_bio_PrivateKey(private_key.get(),nullptr,no_password,nullptr),EVP_PKEY_free);
    require(cert&&key&&EVP_PKEY_get_security_bits(key.get())>=128,"web_certificate_or_key_invalid");
    require(X509_check_purpose(cert.get(),X509_PURPOSE_SSL_SERVER,0)==1,"web_certificate_not_for_server");
    require(X509_cmp_current_time(X509_get0_notBefore(cert.get()))<0&&X509_cmp_current_time(X509_get0_notAfter(cert.get()))>0,"web_certificate_not_current");
    bool numeric{};try{numeric=web_ip(canonical)==canonical;}catch(const std::exception&){}
    require((numeric?X509_check_ip_asc(cert.get(),canonical.c_str(),0):X509_check_host(cert.get(),canonical.c_str(),canonical.size(),X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS|X509_CHECK_FLAG_NEVER_CHECK_SUBJECT,nullptr))==1,"web_certificate_address_mismatch");
    WebTlsIdentity result;result.context={SSL_CTX_new(TLS_server_method()),SSL_CTX_free};require(bool(result.context),"web_tls_context_failed");
    require(SSL_CTX_set_min_proto_version(result.context.get(),TLS1_2_VERSION)==1,"web_tls_minimum_failed");
    SSL_CTX_set_options(result.context.get(),SSL_OP_NO_COMPRESSION);
    require(SSL_CTX_use_certificate(result.context.get(),cert.get())==1&&SSL_CTX_use_PrivateKey(result.context.get(),key.get())==1&&SSL_CTX_check_private_key(result.context.get())==1,"web_certificate_key_mismatch");
    unsigned count{};
    for(;;){X509* next=PEM_read_bio_X509(certificates.get(),nullptr,nullptr,nullptr);if(!next){ERR_clear_error();break;}
        if(++count>16||SSL_CTX_add_extra_chain_cert(result.context.get(),next)!=1){X509_free(next);throw std::runtime_error("web_certificate_chain_invalid");}}
    std::array<unsigned char,EVP_MAX_MD_SIZE> digest{};unsigned size{};
    require(X509_digest(cert.get(),EVP_sha256(),digest.data(),&size)==1&&size==32,"web_certificate_fingerprint_failed");
    constexpr char hex[]="0123456789ABCDEF";for(unsigned i=0;i<size;++i){if(i)result.fingerprint+=':';result.fingerprint+=hex[digest[i]>>4];result.fingerprint+=hex[digest[i]&15];}
    result.certificate=std::string(cert_bytes);return result;
}
void publish_identity(const std::filesystem::path& root,const char* name,const std::string& certificate,const std::string& key,bool replace){
    auto directory=native::open_owned_directory(root/"config");
    Secret encoded{Json{{"version",1},{"certificate",certificate},{"key",key}}.dump()};
    std::vector<std::uint8_t> bytes(encoded.bytes.begin(),encoded.bytes.end());
    struct Wipe{std::vector<std::uint8_t>& bytes;~Wipe(){sodium_memzero(bytes.data(),bytes.size());}}wipe{bytes};
    native::publish_private_file(directory.get(),name,bytes,replace);
}
}
std::string web_ip(std::string_view value){
    require(!value.empty()&&value.size()<INET6_ADDRSTRLEN&&value.find('\0')==value.npos,"web_ip_invalid");
    const std::string text(value);std::array<unsigned char,16> address{};std::array<char,INET6_ADDRSTRLEN> normalized{};
    const int family=inet_pton(AF_INET,text.c_str(),address.data())==1?AF_INET:inet_pton(AF_INET6,text.c_str(),address.data())==1?AF_INET6:0;
    require(family!=0,"web_ip_invalid");
    if(family==AF_INET)require(address[0]!=0&&address[0]<224&&!(address[0]==169&&address[1]==254),"web_ip_not_unicast");
    else{in6_addr ipv6{};std::memcpy(&ipv6,address.data(),sizeof(ipv6));require(!IN6_IS_ADDR_UNSPECIFIED(&ipv6)&&!IN6_IS_ADDR_MULTICAST(&ipv6)&&!IN6_IS_ADDR_LINKLOCAL(&ipv6)&&!IN6_IS_ADDR_V4MAPPED(&ipv6),"web_ip_not_unicast");}
    require(inet_ntop(family,address.data(),normalized.data(),normalized.size())!=nullptr,"web_ip_invalid");return normalized.data();
}
std::string web_server_name(std::string_view value){
    try{return web_ip(value);}catch(const std::exception&){}
    require(!value.empty()&&value.size()<=253,"web_server_name_invalid");std::string normalized(value);
    require(value.find_first_not_of("0123456789.")!=value.npos,"web_server_name_invalid_numeric_address");
    for(auto& c:normalized){if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');require((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.',"web_server_name_invalid");}
    std::size_t offset{};while(offset<normalized.size()){
        const auto end=normalized.find('.',offset),last=end==std::string::npos?normalized.size():end;
        require(last>offset&&last-offset<=63&&normalized[offset]!='-'&&normalized[last-1]!='-',"web_server_name_invalid");
        if(end==std::string::npos)return normalized;
        offset=end+1;
    }
    throw std::runtime_error("web_server_name_invalid");
}
WebAccessSettings parse_web_access(const Json& value){
    keys(value,{"revision","enabled","address","server_name","port","approved","custom_certificate"});
    WebAccessSettings result;require(value.at("revision").is_string(),"web_access_revision_invalid");const auto revision=value.at("revision").get<std::string>();
    const auto parsed=std::from_chars(revision.data(),revision.data()+revision.size(),result.revision);
    require(!revision.empty()&&revision[0]!='0'&&parsed.ec==std::errc{}&&parsed.ptr==revision.data()+revision.size()&&result.revision<UINT64_MAX,"web_access_revision_invalid");
    require(value.at("enabled").is_boolean()&&value.at("custom_certificate").is_boolean(),"web_access_switch_invalid");result.enabled=value.at("enabled").get<bool>();result.custom_certificate=value.at("custom_certificate").get<bool>();
    result.address=value.at("address").get<std::string>();if(!result.address.empty())result.address=web_ip(result.address);
    result.server_name=value.at("server_name").get<std::string>();if(!result.server_name.empty())result.server_name=web_server_name(result.server_name);
    require(value.at("port").is_number_integer()&&value.at("port")>=1&&value.at("port")<=65535,"web_access_port_invalid");result.port=value.at("port").get<std::uint16_t>();
    require(value.at("approved").is_array()&&value.at("approved").size()<=32,"web_access_acl_invalid");std::set<std::string> seen;
    for(const auto& item:value.at("approved")){const auto ip=web_ip(item.get<std::string>());require(seen.insert(ip).second,"web_access_duplicate_ip");result.approved.push_back(ip);}
    require(!result.enabled||(!result.address.empty()&&!result.approved.empty()),"web_access_enabled_without_address_or_acl");return result;
}
Json web_access_json(const WebAccessSettings& value){return {{"revision",std::to_string(value.revision)},{"enabled",value.enabled},{"address",value.address},{"server_name",value.server_name},{"port",value.port},{"approved",value.approved},{"custom_certificate",value.custom_certificate}};}
WebAccessSettings read_web_access(const std::filesystem::path& root){
    const auto directory=native::open_owned_directory(root/"config");struct stat info{};
    if(fstatat(directory.get(),"web-access-linux.v1",&info,AT_SYMLINK_NOFOLLOW)!=0){require(errno==ENOENT,"web_access_unreadable");return {};}
    return parse_web_access(decode(native::read_private_file(directory.get(),"web-access-linux.v1",16384)));
}
void write_web_access(const std::filesystem::path& root,const WebAccessSettings& value,std::uint64_t expected){
    native::InstallationLock publication;require(publication.acquire(root/"management","web-access.lock"),"web_access_busy");
    const auto checked=parse_web_access(web_access_json(value));require(checked.revision==expected+1&&expected<UINT64_MAX-1,"web_access_revision_invalid");
    require(read_web_access(root).revision==expected,"web_access_changed_elsewhere");
    const auto directory=native::open_owned_directory(root/"config");const auto bytes=web_access_json(checked).dump()+"\n";
    native::publish_private_file(directory.get(),"web-access-linux.v1",{bytes.begin(),bytes.end()},true);
}
bool web_ip_allowed(const WebAccessSettings& value,std::string_view address){
    if(!value.enabled)return false;
    try{const auto ip=web_ip(address);return std::find(value.approved.begin(),value.approved.end(),ip)!=value.approved.end();}catch(const std::exception&){return false;}
}
std::string web_access_error(std::string_view code){
    if(code=="web_certificate_address_mismatch")return "The certificate does not match the configured browser server name or IP address.";
    if(code=="web_certificate_not_current")return "The certificate is expired or is not valid yet. Check the Linux clock and certificate dates.";
    if(code=="web_certificate_key_mismatch")return "The certificate and private key do not match.";
    if(code=="web_certificate_or_key_invalid")return "The certificate or key is unreadable, encrypted, or too weak for HTTPS.";
    if(code=="web_certificate_not_for_server")return "The certificate is not valid for a web server.";
    if(code=="web_access_changed_elsewhere")return "Web Access changed elsewhere. Reload its saved settings.";
    if(code=="web_access_local_setup_required")return "Finish the BBS setup and System Password through the local launcher first.";
    if(code=="web_missing_active_identity")return "The saved HTTPS identity is missing. Turn LAN access off and save before explicitly replacing the certificate.";
    if(code=="private_file_unreadable_or_insecure"||code=="private_directory_permissions"||code=="private_directory_component_unavailable")return "A required file or folder is missing, linked, or not private to your Linux account. Check the paths and permissions.";
    if(code=="web_certificate_chain_invalid")return "The supplied certificate chain could not be loaded.";
    if(code=="web_access_busy")return "Another local Web Access change is in progress. Reload after it finishes.";
    if(code.find("HTTPS could not bind.")==0)return std::string(code);
    return "Check the Linux address, HTTPS port, exact approved IP list, certificate files and permissions.";
}
WebTlsIdentity load_web_tls(const std::filesystem::path& root,const WebAccessSettings& settings){
    const auto directory=native::open_owned_directory(root/"config");auto bytes=native::read_private_file(directory.get(),settings.custom_certificate?"web-https-custom.v1":"web-https-generated.v1",2097152);
    struct Wipe{std::vector<std::uint8_t>& bytes;~Wipe(){sodium_memzero(bytes.data(),bytes.size());}}wipe{bytes};
    auto value=decode(bytes);keys(value,{"version","certificate","key"});require(value.at("version")==1,"web_certificate_version_invalid");
    Secret secret{value.at("key").get<std::string>()};auto& original=value.at("key").get_ref<std::string&>();sodium_memzero(original.data(),original.size());
    return identity(value.at("certificate").get<std::string>(),secret.bytes,settings.server_name.empty()?settings.address:settings.server_name);
}
void generate_web_tls(const std::filesystem::path& root,std::string_view address,bool replace){
    require(sodium_init()>=0,"web_certificate_randomness_unavailable");
    const auto name_identity=web_server_name(address);bool numeric{};try{numeric=web_ip(name_identity)==name_identity;}catch(const std::exception&){}
    Key key(EVP_PKEY_Q_keygen(nullptr,nullptr,"EC","prime256v1"),EVP_PKEY_free);Certificate cert(X509_new(),X509_free);
    require(key&&cert&&X509_set_version(cert.get(),2)==1,"web_certificate_generation_failed");
    std::array<unsigned char,16> serial{};randombytes_buf(serial.data(),serial.size());serial[0]&=0x7f;serial[0]|=1;
    std::unique_ptr<BIGNUM,decltype(&BN_free)> number(BN_bin2bn(serial.data(),serial.size(),nullptr),BN_free);
    require(number&&BN_to_ASN1_INTEGER(number.get(),X509_get_serialNumber(cert.get())),"web_certificate_serial_failed");
    require(X509_gmtime_adj(X509_getm_notBefore(cert.get()),-300)&&X509_gmtime_adj(X509_getm_notAfter(cert.get()),365L*24L*60L*60L)&&X509_set_pubkey(cert.get(),key.get())==1,"web_certificate_dates_failed");
    auto* name=X509_get_subject_name(cert.get());const auto title=std::string("Spitfire-X Web Controls");
    require(X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>(title.c_str()),-1,-1,0)==1&&X509_set_issuer_name(cert.get(),name)==1,"web_certificate_name_failed");
    X509V3_CTX context{};X509V3_set_ctx(&context,cert.get(),cert.get(),nullptr,nullptr,0);
    const auto extension=[&](int nid,const std::string& text){std::unique_ptr<X509_EXTENSION,decltype(&X509_EXTENSION_free)> value(X509V3_EXT_conf_nid(nullptr,&context,nid,text.c_str()),X509_EXTENSION_free);require(value&&X509_add_ext(cert.get(),value.get(),-1)==1,"web_certificate_extension_failed");};
    extension(NID_basic_constraints,"critical,CA:FALSE");extension(NID_key_usage,"critical,digitalSignature");extension(NID_ext_key_usage,"serverAuth");extension(NID_subject_alt_name,(numeric?"IP:":"DNS:")+name_identity);
    require(X509_sign(cert.get(),key.get(),EVP_sha256())>0,"web_certificate_sign_failed");
    Bio certificates(BIO_new(BIO_s_mem()),BIO_free),private_key(BIO_new(BIO_s_mem()),BIO_free);require(certificates&&private_key&&PEM_write_bio_X509(certificates.get(),cert.get())==1&&PEM_write_bio_PrivateKey(private_key.get(),key.get(),nullptr,nullptr,0,nullptr,nullptr)==1,"web_certificate_encoding_failed");
    const auto encoded=output(certificates.get());Secret secret{output(private_key.get())};(void)identity(encoded,secret.bytes,name_identity);publish_identity(root,"web-https-generated.v1",encoded,secret.bytes,replace);
}
void import_web_tls(const std::filesystem::path& root,std::string_view address,const std::filesystem::path& certificate,const std::filesystem::path& private_key){
    const auto cert_directory=native::open_owned_directory(certificate.parent_path()),key_directory=native::open_owned_directory(private_key.parent_path());
    const auto cert=native::read_private_file(cert_directory.get(),certificate.filename().string(),1048576);auto key=native::read_private_file(key_directory.get(),private_key.filename().string(),1048576);
    struct Wipe{std::vector<std::uint8_t>& bytes;~Wipe(){sodium_memzero(bytes.data(),bytes.size());}}wipe{key};Secret secret{{key.begin(),key.end()}};const std::string encoded(cert.begin(),cert.end());
    (void)identity(encoded,secret.bytes,address);publish_identity(root,"web-https-custom.v1",encoded,secret.bytes,true);
}
} // namespace sfx::apps::runtime::web
