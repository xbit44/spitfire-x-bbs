// Actual Runtime operational journal with synthetic trusted notices, not malware.
#include "alpha_operational_journal.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <iostream>
namespace app=sfx::application;
namespace win=sfx::platform::win32;
namespace ins=win::installation;
int main(int argc,char** argv)try{
    unsigned checks{};const auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
    check(argc==2,"fresh native journal fixture argument");const auto root=std::filesystem::absolute(argv[1]);
    check(root.parent_path()==ins::Path(L"C:\\sfxtst\\runs")&&root.filename().string().rfind("section9-clamav-visibility-journal-",0)==0&&!std::filesystem::exists(root),"bounded new journal fixture only");
    const auto owner=ins::operator_sid();win::WindowsStorageDaclPolicy policy(owner);
    for(const auto& folder:{root,root/L"Logs"}){std::filesystem::create_directory(folder);check(policy.apply(folder,win::StorageDaclObjectKind::directory).succeeded(),"private journal fixture directory");}
    app::Identifier128 epoch;epoch.bytes[0]=1;
    std::vector<app::AttentionRecord> notices;
    sfx::apps::runtime::AlphaOperationalJournal journal(root/L"Logs"/L"SFXRuntime.log",owner,epoch);
    journal.clamav_health(false,false,true,"clamav_update_vendor_cooldown");journal.clamav_health(false,false,true,"clamav_update_vendor_cooldown");
    check(notices.empty(),"startup notices wait for actual durable sink");
    journal.attention_sink([&](app::AttentionRecord r){notices.push_back(std::move(r));});
    check(notices.size()==1&&notices[0].kind==app::AttentionKind::clamav_failure&&notices[0].definitions_usable,"one early failure delivered after sink attachment, no repeated flood");
    journal.clamav_health(false,true,true,{});journal.clamav_health(false,true,true,{});
    check(notices.size()==2&&notices[1].kind==app::AttentionKind::clamav_recovery,"one recovery follows failure, no routine-success attention");
    app::AttentionRecord finding;finding.kind=app::AttentionKind::clamav_finding;finding.utc_seconds=1792316600;finding.node=1;finding.identity=app::AttentionIdentity::verified;finding.account_name="Test Caller";finding.filename="synthetic.txt";finding.signatures={"Synthetic.Not.Malware"};finding.deletion_confirmed=true;finding.deletion_journal_confirmed=true;
    journal.clamav_finding(finding);finding.deletion_confirmed=false;finding.deletion_journal_confirmed=false;journal.clamav_finding(finding);
    check(notices.size()==4&&notices[2].deletion_confirmed&&!notices[3].deletion_confirmed,"distinct injected findings retain confirmed and unconfirmed dispositions");
    finding.filename="../forged";journal.clamav_finding(finding);check(notices.size()==4,"unsafe finding refused before publication");
    check(journal.flush(),"actual operational file writer flushes normally");
    const auto entries=journal.snapshot();bool failure{},recovery{},deleted{},held{};
    for(const auto& entry:entries)if(entry.event){const auto& c=entry.event->context;failure=failure||c.diagnostic_code=="clamav_update_vendor_cooldown";recovery=recovery||c.diagnostic_code=="clamav_recovered";deleted=deleted||c.diagnostic_code=="clamav_detection_deleted";held=held||c.diagnostic_code=="clamav_detection_retained";}
    check(failure&&recovery&&deleted&&held,"Events/System contain concise failure, recovery and truthful finding outcomes");
    for(const auto& entry:entries)if(entry.event&&(entry.event->context.diagnostic_code=="clamav_detection_deleted"||entry.event->context.diagnostic_code=="clamav_detection_retained")){
        check(entry.message.find("synthetic.txt")!=std::string::npos&&entry.message.find("Synthetic.Not.Malware")!=std::string::npos&&entry.message.find("Test Caller")!=std::string::npos,"Events retain the HEYSYSOP finding identity and first signature");
        check(entry.message.find("temporarily unavailable")==std::string::npos&&app::decode_operational_record(app::encode_operational_record(entry),entry.sequence).has_value(),"finding outcome has no unavailable suffix and survives durable log roundtrip");
        if(entry.event->context.diagnostic_code=="clamav_detection_deleted")check(entry.message.find("upload rejected; staged file deletion confirmed")!=std::string::npos,"confirmed finding uses exact HEYSYSOP disposition");
        else check(entry.message.find("deletion NOT confirmed; private staging requires Sysop review")!=std::string::npos,"unconfirmed deletion never claims successful removal");
    }
    finding.filename=u8"\u00c9preuve.zip";finding.account_name=std::string(30,'A');finding.node=128;finding.signatures={std::string(128,'|'),"Synthetic.Second"};finding.deletion_confirmed=true;finding.deletion_journal_confirmed=true;
    journal.clamav_finding(finding);check(notices.size()==5,"valid Unicode finding and long signature still deliver the durable notice");
    const auto bounded=journal.snapshot().back();check(bounded.message.find("\\xC3\\x89preuve.zip")!=std::string::npos&&bounded.message.find("details in HEYSYSOP")!=std::string::npos&&bounded.message.find("staged file deletion confirmed")!=std::string::npos,"Events safely represent Unicode and clipped fields while preserving the full disposition");
    check(bounded.message.size()<=app::kOperationalPresentationMaximum&&app::decode_operational_record(app::encode_operational_record(bounded),bounded.sequence).has_value(),"maximum finding stays inside existing durable and private display bounds");
    std::cout<<"PASS native ClamAV journal "<<checks<<" checks; injected notices, actual journal, not positive malware/GUI\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL ClamAV journal: "<<e.what()<<'\n';return 1;}
