#include "sfx/application/door_bulletin.hpp"
#include "sfx/application/display.hpp"
#include "sfx/application/file.hpp"
#include <algorithm>
#include <array>
#include <charconv>

namespace sfx::application {
bool valid_door_bulletin_id(DoorBulletinId id) noexcept {return id.value>=1&&id.value<=999;}
bool valid_door_bulletin_relative_path(std::string_view path) noexcept {
    if(path.empty()||path.size()>240||path.front()=='/'||path.front()=='\\')return false;
    std::size_t begin{};
    for(std::size_t i=0;i<=path.size();++i){
        if(i==path.size()||path[i]=='/'||path[i]=='\\'){
            const auto part=path.substr(begin,i-begin);
            // Match existing portable native filename admission before Save,
            // including 128-byte components and DOS device/alias spelling.
            if(!valid_file_name(part)||(i<path.size()&&part.find('~')!=part.npos))return false;
            begin=i+1;
        }else{const auto c=static_cast<unsigned char>(path[i]);if(c<32||c>126||path[i]==':'||path[i]=='*'||path[i]=='?'||path[i]=='"'||path[i]=='<'||path[i]=='>'||path[i]=='|')return false;}
    }return true;
}
bool valid_door_bulletin_definition(const DoorBulletinDefinition& d) noexcept {
    if(!valid_door_bulletin_id(d.id)||!valid_door_text(d.title,64)||d.revision<0)return false;
    if(d.format!=GraphicsMode::ascii&&d.format!=GraphicsMode::ansi)return false;
    if(d.source==DoorBulletinSourceKind::display)return !d.door&&d.relative_path.empty()&&d.format==GraphicsMode::ascii;
    return d.source==DoorBulletinSourceKind::door_file&&d.door>=1&&d.door<=999&&valid_door_bulletin_relative_path(d.relative_path);
}
bool valid_door_bulletin_change(const DoorBulletinChange& r) noexcept {
    return r.operation.valid()&&r.expected_revision>=1&&r.expected_revision<INT64_MAX&&valid_door_bulletin_id(r.definition.id)&&
        (!r.remove||!r.create_only)&&(r.remove||valid_door_bulletin_definition(r.definition));
}
bool valid_door_bulletin_publication(const DoorBulletinPublication& p) noexcept {
    if(!valid_door_bulletin_id(p.id)||p.definition_revision<1||p.door_revision<0||p.variant.empty()||p.variant.size()>32||p.captured_content.size()>kMaximumTrustedDisplaySourceBytes||p.fingerprint_input.size()>kMaximumTrustedDisplayAggregateSourceBytes+8192)return false;
    for(const auto c:p.variant)if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'))return false;
    if(!p.available)return p.variant=="UNAVAILABLE"&&p.captured_content.empty()&&p.fingerprint_input.empty();
    if(p.fingerprint_input.empty())return false;
    if(p.variant=="GENERATED-ANS"||p.variant=="GENERATED-ASC")return true;
    const auto v=std::string_view(p.variant);if(v.size()<9||v.substr(0,2)!="DB"||v[2]!='0'+p.id.value/100||v[3]!='0'+p.id.value/10%10||v[4]!='0'+p.id.value%10)return false;
    const auto ext=v.substr(v.size()-4);if(ext!=".ANS"&&ext!=".ASC")return false;
    const auto level=v.substr(5,v.size()-9);if(level.empty())return true;
    if(level.size()>3||level.front()=='0')return false;
    unsigned n{};const auto parsed=std::from_chars(level.data(),level.data()+level.size(),n);return parsed.ec==std::errc{}&&parsed.ptr==level.data()+level.size()&&n>=1&&n<=999;
}
namespace {
struct Writer {std::vector<std::uint8_t> b;void number(std::uint64_t n){for(unsigned i=0;i<8;++i)b.push_back(static_cast<std::uint8_t>(n>>(8*i)));}void text(std::string_view s){number(s.size());b.insert(b.end(),s.begin(),s.end());}};
struct Reader {
    const std::vector<std::uint8_t>& b;std::size_t p{};bool ok{true};
    std::uint64_t number(){if(b.size()-p<8){ok=false;return 0;}std::uint64_t n{};for(unsigned i=0;i<8;++i)n|=std::uint64_t(b[p++])<<(8*i);return n;}
    std::uint16_t small(unsigned max=999){auto n=number();if(n>max)ok=false;return static_cast<std::uint16_t>(n);}
    std::string text(std::size_t max){auto n=number();if(!ok||n>max||n>b.size()-p){ok=false;return {};}std::string out(b.begin()+p,b.begin()+p+static_cast<std::size_t>(n));p+=static_cast<std::size_t>(n);return out;}
};
void put(Writer& w,const DoorBulletinDefinition& d){w.number(d.id.value);w.text(d.title);w.number(d.enabled);w.number(static_cast<unsigned>(d.source));w.number(d.door);w.text(d.relative_path);w.number(static_cast<unsigned>(d.format));w.number(d.revision);}
DoorBulletinDefinition get(Reader& r){DoorBulletinDefinition d;d.id={r.small()};d.title=r.text(64);d.enabled=r.small(1)!=0;d.source=static_cast<DoorBulletinSourceKind>(r.small(1));d.door=r.small();d.relative_path=r.text(240);d.format=static_cast<GraphicsMode>(r.small(1));auto revision=r.number();if(revision>INT64_MAX)r.ok=false;d.revision=static_cast<std::int64_t>(revision);return d;}
}
std::vector<std::uint8_t> encode_door_bulletin_catalog(const DoorBulletinCatalog& c){
    if(c.revision<1||c.entries.size()>999)return {};
    Writer w;w.number(1);w.number(c.revision);w.number(c.entries.size());unsigned last{};
    for(const auto& e:c.entries){if(!valid_door_bulletin_definition(e.definition)||e.definition.id.value<=last||e.definition.revision<1||static_cast<unsigned>(e.health)>2|| (e.published_utc&&!valid_utc_seconds(*e.published_utc)))return {};last=e.definition.id.value;put(w,e.definition);w.number(static_cast<unsigned>(e.health));w.number(e.published_utc.has_value());if(e.published_utc)w.number(static_cast<std::uint64_t>(*e.published_utc));}return w.b;
}
std::optional<DoorBulletinCatalog> decode_door_bulletin_catalog(const std::vector<std::uint8_t>& b){
    if(b.size()>512U*1024U)return {};
    Reader r{b};if(r.number()!=1)return {};DoorBulletinCatalog c;auto rev=r.number();if(!rev||rev>INT64_MAX)return {};c.revision=static_cast<std::int64_t>(rev);auto n=r.small();for(unsigned i=0;i<n&&r.ok;++i){DoorBulletinEntry e;e.definition=get(r);e.health=static_cast<DoorBulletinHealth>(r.small(2));if(r.small(1)){const auto raw=r.number();const auto t=raw<=INT64_MAX?static_cast<std::int64_t>(raw):-1-static_cast<std::int64_t>(~raw);if(!valid_utc_seconds(t))r.ok=false;e.published_utc=t;}c.entries.push_back(std::move(e));}if(!r.ok||r.p!=b.size()||encode_door_bulletin_catalog(c).empty())return {};return c;
}
std::vector<std::uint8_t> encode_door_bulletin_change(const DoorBulletinChange& c){if(!valid_door_bulletin_change(c))return {};Writer w;w.number(1);w.b.insert(w.b.end(),c.operation.bytes.begin(),c.operation.bytes.end());w.number(c.expected_revision);w.number(c.remove);w.number(c.create_only);put(w,c.definition);return w.b;}
std::optional<DoorBulletinChange> decode_door_bulletin_change(const std::vector<std::uint8_t>& b){if(b.size()<24||b.size()>1024)return {};Reader r{b};if(r.number()!=1)return {};DoorBulletinChange c;std::copy_n(b.begin()+8,16,c.operation.bytes.begin());r.p=24;auto rev=r.number();if(rev>INT64_MAX)return {};c.expected_revision=static_cast<std::int64_t>(rev);c.remove=r.small(1)!=0;c.create_only=r.small(1)!=0;c.definition=get(r);if(!r.ok||r.p!=b.size()||!valid_door_bulletin_change(c))return {};return c;}
} // namespace sfx::application
