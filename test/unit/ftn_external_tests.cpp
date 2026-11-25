#include "sfx/application/ftn_external.hpp"
#include "sfx/archive/physical_layout.hpp"
#include <iostream>
#include <stdexcept>
using namespace sfx::application;
namespace {
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
FtnConfiguration config(){
    FtnConfiguration c;c.settings.mode=FtnMailerMode::external_binkd;
    FtnNetwork n;n.code="TEST";n.address="2:100/1@test";n.enabled=true;c.networks.push_back(n);
    FtnLink l;l.network="TEST";l.code="PEER";l.address="2:100/2@test";l.enabled=true;l.credentials.session={1};c.links.push_back(l);return c;
}
FtnExternalReceiveRecord record(){
    FtnExternalReceiveRecord r;r.id.bytes[0]=1;r.network="TEST";r.relative_file="FTN/External/test/Inbound/Secure/mail.pkt";
    r.protected_session=true;r.size=123;r.timestamp=1700000000;r.remote.push_back(*parse_ftn_address("2:100/2@test"));r.digest[0]=42;return r;
}
void inbound(){
    auto r=record();auto c=config();const auto wire=encode_ftn_external_receive(r);
    check(wire.has_value(),"bounded local record encoded");const auto decoded=decode_ftn_external_receive(*wire);
    check(decoded&&decoded->digest==r.digest&&decoded->relative_file==r.relative_file&&decoded->id.bytes==r.id.bytes,"exact file context round trip");
    check(match_ftn_external_receive(c,r).has_value(),"session-only configured link resolved");
    c.links[0].use=FtnConnectionUse::outbound_only;check(match_ftn_external_receive(c,r).has_value(),"receive hook does not invent incoming-session direction");
    c.links[0].credentials.session.clear();check(match_ftn_external_receive(c,r).has_value(),"external-only link does not duplicate BinkD session secret");
    r.protected_session=false;r.relative_file="FTN/External/test/Inbound/Nonsecure/mail.pkt";
    check(match_ftn_external_receive(c,r).has_value(),"unprotected context remains unprotected for later admission");
    r=record();c=config();auto second=c.networks[0];second.code="OTHER";second.address="2:100/1@other";c.networks.push_back(second);
    auto other=c.links[0];other.network="OTHER";other.address="2:100/2@other";c.links.push_back(other);r.remote[0].domain.clear();
    check(match_ftn_external_receive(c,r)->network=="TEST","owned profile resolves overlapping domainless network addresses");
    other=c.links[0];other.code="AMBIGUOUS";c.links.push_back(other);check(!match_ftn_external_receive(c,r),"same-profile ambiguity never selects first link");
    c=config();c.settings.mode=FtnMailerMode::native;check(!match_ftn_external_receive(c,r),"Native does not consume external records");
    for(const auto bad:{"../mail.pkt","/absolute/mail.pkt","C:/mail.pkt","FTN//mail.pkt","FTN/./mail.pkt","FTN/../mail.pkt","FTN/mail name.pkt","FTN/mail;touch.pkt","FTN/mail\npkt","FTN/mail.pkt.","FTN\\mail.pkt"})check(!valid_ftn_external_path(bad),"unsupported relative hook path refused");
    for(const auto bad:{"FTN/External/other/Inbound/Secure/mail.pkt","FTN/External/test/Inbound/Unknown/mail.pkt","FTN/External/test/Inbound/Secure/sub/mail.pkt","FTN/External/test/Inbound/Secure/CON.pkt"}){r=record();r.relative_file=bad;check(!encode_ftn_external_receive(r),"profile and leaf validation before recording");}
    r=record();r.protected_session=false;check(encode_ftn_external_receive(r).has_value(),"Secure directory name does not authenticate P=0");
    r.relative_file="FTN/External/test/Inbound/Links/peer/mail.pkt";c=config();check(match_ftn_external_receive(c,r).has_value(),"unprotected node ibox resolves configured link without inventing proof");
    r.protected_session=true;check(match_ftn_external_receive(c,r).has_value(),"same ibox accepts actual P=1 observation");
    r.relative_file="FTN/External/test/Inbound/Links/other/mail.pkt";check(!match_ftn_external_receive(c,r),"ibox cannot contradict resolved peer");
    check(!decode_ftn_external_receive(wire->substr(0,wire->size()-1)),"partial record is not complete");
    check(!decode_ftn_external_receive(*wire+"junk\n"),"unparsed tail refused");
    check(!decode_ftn_external_receive(std::string(8193,'x')),"oversized metadata refused");
    r=record();r.remote.push_back(r.remote[0]);check(!encode_ftn_external_receive(r),"duplicate AKA refused");
    r=record();r.size=kFtnTransferMaximumBytes+1;check(!encode_ftn_external_receive(r),"incoming size bounded");
    r=record();r.timestamp=UINT64_MAX;check(!encode_ftn_external_receive(r),"timestamp bounded");
    for(std::size_t i=0;i<wire->size();++i)check(!decode_ftn_external_receive(wire->substr(0,i)),"truncated one-AKA record refused");
}
void outbound(){
    auto c=config();FtnOutboundJob job;job.id.bytes[0]=1;job.network="TEST";job.link="PEER";job.file.name="demo.zip";job.kind=FtnOutboundKind::payload;
    auto paths=ftn_external_outbound_paths(c.networks[0],c.links[0],job);
    check(paths&&paths->flow=="FTN/External/test/Outbound/00640002.flo"&&paths->busy=="FTN/External/test/Outbound/00640002.bsy","standard default-zone normal BSO address");
    check(paths->copy=="FTN/External/test/Copies/01000000000000000000000000000000/demo.zip","unique disposable copy keeps wire basename");
    check(ftn_external_flow_line(*paths)=="^"+paths->copy+"\r\n","delete-after-send applies only to disposable copy");
    job.flavor=FtnMailFlavor::hold;paths=ftn_external_outbound_paths(c.networks[0],c.links[0],job);check(paths->flow=="FTN/External/test/Outbound/00640002.hlo","Hold uses hlo");
    job.flavor=FtnMailFlavor::crash;paths=ftn_external_outbound_paths(c.networks[0],c.links[0],job);check(paths->flow=="FTN/External/test/Outbound/00640002.clo","Crash uses clo");
    c.links[0].address="3:500/99.12@test";paths=ftn_external_outbound_paths(c.networks[0],c.links[0],job);
    check(paths&&paths->flow=="FTN/External/test/Outbound.003/01f40063.pnt/0000000c.clo","nondefault zone and point BSO layout");
    c.links[0].address="4096:500/99@test";paths=ftn_external_outbound_paths(c.networks[0],c.links[0],job);check(paths&&paths->flow.find("Outbound.1000/")!=std::string::npos,"zone extension not truncated");
    c.links[0].address="2:100/2@other";check(!ftn_external_outbound_paths(c.networks[0],c.links[0],job),"wrong-domain outgoing target refused");
    c=config();job.file.name="two words.zip";check(!ftn_external_outbound_paths(c.networks[0],c.links[0],job),"unsupported wire name stays unexported");
    job.file.name="demo.zip";job.network="OTHER";check(!ftn_external_outbound_paths(c.networks[0],c.links[0],job),"job cannot cross profile");
    check(ftn_external_flow_line({{}, {},"FTN/Outbound/canonical.pkt"}).empty(),"FLO formatter refuses canonical SFX pool");
}
void unknown_callers(){
    auto c=config();auto r=record();r.protected_session=false;r.remote={*parse_ftn_address("2:100/99@test")};
    check(ftn_external_nonsecure_receive(c,r),"unknown P=0 uses existing direct-local policy even in Secure folder");
    r.protected_session=true;check(!ftn_external_nonsecure_receive(c,r),"unknown P=1 never invents a configured authenticated peer");r.protected_session=false;
    c.settings.allow_direct_local_netmail=false;check(!ftn_external_nonsecure_receive(c,r),"direct-local Off retains unknown receive record");c.settings.allow_direct_local_netmail=true;
    r.remote.push_back(*parse_ftn_address("2:100/2@test"));check(!ftn_external_nonsecure_receive(c,r),"known AKA among unknown AKAs is not unknown");
    c.links[0].enabled=false;check(!ftn_external_nonsecure_receive(c,r),"disabled known link cannot fall back to unknown");
    c=config();c.links.push_back(c.links[0]);c.links.back().code="SECOND";check(!ftn_external_nonsecure_receive(c,r),"ambiguous known links cannot fall back to unknown");
    r.remote={*parse_ftn_address("2:100/99@test")};r.relative_file="FTN/External/test/Inbound/Links/peer/mail.pkt";check(!ftn_external_nonsecure_receive(c,r),"node inbox hint does not become unknown custody");
    r.relative_file="FTN/External/test/Inbound/Nonsecure/mail.pkt";c.networks[0].enabled=false;check(!ftn_external_nonsecure_receive(c,r),"disabled profile does not admit unknown callers");
    c=config();c.settings.mode=FtnMailerMode::native;check(!ftn_external_nonsecure_receive(c,r),"Native does not consume external nonsecure metadata");
}
void configuration(){
    auto c=config();c.networks[0].name="External network";c.links[0].name="External peer";
    c.settings.external_executable.clear();c.settings.bind_address.clear();c.settings.port=0;
    c.settings.tls_listener_enabled=true;c.settings.tls_port=0;c.settings.tls_certificate.clear();c.settings.tls_private_key.clear();
    c.links[0].host.clear();c.links[0].port=0;c.links[0].credentials.session.clear();
    check(validate_ftn_configuration(c).empty(),"external-only setup needs no unused Native executable/endpoints/certificate/session password");
    c.settings.mode=FtnMailerMode::native;check(!validate_ftn_configuration(c).empty(),"switching back to Native requires real Native settings");
    c.settings.mode=FtnMailerMode::disabled;check(validate_ftn_configuration(c).empty(),"disabling External retains mail without requiring inactive Native endpoints");
    c.settings.mode=FtnMailerMode::external_binkd;c.links[0].areafix_enabled=true;
    check(!validate_ftn_configuration(c).empty(),"External still requires separate AreaFix password when enabled");
}
void profiles(){
    const std::string text="domain test FTN/External/test/Outbound 2\ndomain other FTN/External/other/Outbound 3\naddress 2:100/1@test\naddress 3:200/1@other\nlisten 127.0.0.1:24554\ninbound FTN/External/Inbound/Secure\ninbound-nonsecure FTN/External/Inbound/Nonsecure\ntemp-inbound FTN/External/Temp\nnode 2:100/2@test -nr 127.0.0.1:24555 synthetic-only-key i - -\nnode 3:200/2@other -nr 127.0.0.1:24556 - i - -\n";
    const auto profile=parse_ftn_external_profile(text);
    check(profile&&profile->networks.size()==2&&profile->local.size()==2&&profile->remote.size()==2&&profile->protected_remote.size()==1,"one stock profile binds both networks with explicit protected aliases only");
    check(parse_ftn_external_profile(profile->configuration).has_value(),"canonical quoted stock profile parses exactly");
    auto ordinary=text;for(auto at=ordinary.find(" -nr ");at!=std::string::npos;at=ordinary.find(" -nr "))ordinary.erase(at,4);
    const auto normal=parse_ftn_external_profile(ordinary);check(normal&&normal->remote.size()==2&&normal->protected_remote.size()==1&&normal->configuration.find("-nr")==std::string::npos,"ordinary node profile preserves exact auth without unreliable-link option");
    check(profile->configuration.find("\"-nr\"")!=std::string::npos,"explicit optional unreliable-link flag preserved");
    auto unsupported=ordinary;unsupported.insert(unsupported.find("127.0.0.1:24555"),"-unexpected ");check(!parse_ftn_external_profile(unsupported),"other node options cannot shift credential fields");
    for(const auto extra:{"include more.cfg\n","exec arbitrary command\n","kill-old-bsy 1h\n","node * -nr host secret i - -\n","domain test FTN/External/second/Outbound 2\n","node 2:100/2@test -nr host another i - -\n","address 4:400/1@missing\n"})check(!parse_ftn_external_profile(text+extra),"unsupported expansion or inconsistent profile refused");
    for(const auto password:{"!","one,two","abc def","*","?"}){auto changed=text;const auto at=changed.find("synthetic-only-key");changed.replace(at,18,password);check(!parse_ftn_external_profile(changed),"unsupported wildcard or multiple-password semantics refused");}
    auto changed=text;changed.replace(changed.find("FTN/External/test/Outbound"),26,"C:/outside");check(!parse_ftn_external_profile(changed),"outside spool does not silently break folder portability");
    auto r=record();r.network.clear();r.run="01000000000000000000000000000000";r.run_digest[0]=1;r.relative_file="FTN/External/Inbound/Secure/mail.pkt";
    r.remote.push_back(*parse_ftn_address("2:100/99@test"));r.authenticated_remote={r.remote.front()};
    const auto encoded=encode_ftn_external_receive(r);check(encoded.has_value(),"run-bound shared receive encodes");
    const auto decoded=decode_ftn_external_receive(*encoded);check(decoded&&decoded->run==r.run&&decoded->run_digest==r.run_digest&&decoded->authenticated_remote.size()==1,"run and protected AKA scope round trip");
    auto c=config();check(match_ftn_external_receive(c,r,"TEST").has_value(),"explicit protected alias may match scoped file network");
    r.authenticated_remote={r.remote.back()};check(!match_ftn_external_receive(c,r,"TEST"),"global P does not authenticate another advertised alias");
    r.authenticated_remote={*parse_ftn_address("2:100/88@test")};check(!encode_ftn_external_receive(r),"authenticated alias must have been advertised");
    r.authenticated_remote.clear();r.protected_session=false;check(ftn_external_nonsecure_receive(c,r)==false,"known AKA in shared unprotected session cannot downgrade to unknown");
    r.remote={*parse_ftn_address("2:100/99@test")};check(ftn_external_nonsecure_receive(c,r),"shared unprotected unknown caller retains direct-local policy");
}
void lowercase_layout(){
    constexpr auto lower=FtnExternalPathStyle::linux_lowercase_v1;
    const auto c=config();FtnOutboundJob job;job.id.bytes[0]=1;job.network="TEST";job.link="PEER";job.file.name="OwnerName.ZIP";
    const auto canonical=ftn_external_outbound_paths(c.networks[0],c.links[0],job);
    const auto native=ftn_external_outbound_paths(c.networks[0],c.links[0],job,lower);
    check(native&&canonical&&native->flow=="ftn/external/test/outbound/00640002.flo"&&native->copy=="ftn/external/test/copies/01000000000000000000000000000000/OwnerName.ZIP","Linux selected paths lower owned directories and preserve wire basename");
    check(ftn_external_flow_line(*native,lower)=="^"+native->copy+"\r\n"&&ftn_external_flow_line(*canonical)=="^"+canonical->copy+"\r\n","FLO paths match selected physical platform and canonical default stays unchanged");
    for(const auto* path:{"FTN/External/test/Inbound/Secure/Owner.PKT","FTN/External/Runs/01000000000000000000000000000000/Manifest","FTN/External/Runs/01000000000000000000000000000000/Manifest.tmp","FTN/External/test/Profile","FTN/External/test/Profile.tmp","FTN/External/test/Outbound.003/01f40063.pnt/0000000c.clo","FTN/External/OwnerProfile.cfg","FTN/External/test/Outbound.NotOwned/Owner.PKT","FTN/External/runs/Profile","FTN/External/inbound/Outbound.003/01f40063.flo","FTN/External/temp/Inbound/Secure/Owner.PKT"}){
        const auto physical=ftn_external_physical_path(path,lower);
        check(ftn_external_canonical_path(physical,lower)==path,"exact owned slot mapping round trips without normalizing arbitrary leaf");
        check(physical==sfx::archive::physical_relative(path,sfx::archive::PhysicalLayout::linux_lowercase_v1)&&
            sfx::archive::archive_relative(physical,sfx::archive::PhysicalLayout::linux_lowercase_v1)==path,"profile/native/archive mapping agrees on exact fixed and owner slots");
    }
    const std::string profile="domain test ftn/external/test/outbound 2\naddress 2:100/1@test\nlisten 127.0.0.1:24554\ninbound ftn/external/inbound/secure\ninbound-nonsecure ftn/external/inbound/nonsecure\ntemp-inbound ftn/external/temp\n";
    const auto parsed=parse_ftn_external_profile(profile,lower);
    check(parsed&&parse_ftn_external_profile(parsed->configuration,lower).has_value(),"supported Linux profile uses exact lower physical directory grammar");
    check(!parse_ftn_external_profile(profile),"default canonical parser never silently accepts Linux layout");
    check(ftn_external_root("TEST")=="FTN/External/test"&&ftn_external_root("TEST",lower)=="ftn/external/test","explicit path style leaves network identity untouched");
    const auto invalid=static_cast<FtnExternalPathStyle>(999);
    check(ftn_external_physical_path("FTN/External/test/Profile",invalid).empty()&&!parse_ftn_external_profile(profile,invalid)&&!ftn_external_outbound_paths(c.networks[0],c.links[0],job,invalid),"unknown path-style capability refuses rather than choosing defaults");
    auto r=record();const auto wire=encode_ftn_external_receive(r);check(wire&&wire->find("FTN/External/test/Inbound/Secure/mail.pkt")!=std::string::npos,"receive record format remains canonical on all platforms");
}
}
int main(){try{inbound();outbound();configuration();unknown_callers();profiles();lowercase_layout();std::cout<<"PASS external BinkD contract "<<checks<<" checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
