#include "linux_web_ftn_drafts.hpp"
#include "linux_web_ftn_view.hpp"
#include <iostream>

namespace {
namespace app=sfx::application;namespace web=sfx::apps::runtime::web;using Code=app::FtnConfigurationCode;using Json=nlohmann::json;
unsigned checks{};
void check(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
template<class F>void refuses(F f){bool refused{};try{f();}catch(const std::exception&){refused=true;}check(refused,"unsafe draft operation refused");}
app::FtnConfigurationResponse response(Code code,std::int64_t revision=0){app::FtnConfigurationResponse out;out.code=code;out.revision=revision;return out;}
struct Fixture {
    app::FtnConfiguration saved;std::uint64_t now{100};unsigned serial{},changes{};bool lose_commit{},lose_receipt{};
    std::map<app::FtnTransferToken,std::int64_t> receipts;app::FtnConfigurationPublisher publisher;
    Fixture():publisher([&]{return std::optional<app::FtnConfiguration>{saved};},[&](const auto& token,const auto& candidate){
        if(candidate.revision!=saved.revision)return response(Code::conflict);
        saved=candidate;++saved.revision;++changes;receipts[token]=saved.revision;return response(Code::ok,saved.revision);
    },[&](const auto& token){if(lose_receipt)return response(Code::unavailable);const auto i=receipts.find(token);return i==receipts.end()?response(Code::conflict):response(Code::ok,i->second);},[&](const auto& token,const auto& candidate){
        app::FtnRetentionPreview preview;preview.token=token;preview.configuration_revision=candidate.revision;preview.reviewed_utc=1700000000;preview.draft[0]=1;preview.changed=preview.shortened=1;preview.networks.push_back({"TEST","Test",1,0,1,1700000000,1700000000});auto out=response(Code::ok,candidate.revision);out.bytes=app::encode_ftn_retention_preview(preview);out.total=static_cast<std::uint32_t>(out.bytes.size());return out;
    }){saved.settings.mode=app::FtnMailerMode::disabled;saved.settings.listener_enabled=false;app::FtnNetwork n;n.code="TEST";n.name="Test";n.msg_netmail_enabled=false;saved.networks.push_back(n);}
    app::FtnTransferToken token(){app::FtnTransferToken out{};out[0]=static_cast<std::uint8_t>(++serial);return out;}
    app::FtnConfigurationResponse exchange(const app::FtnConfigurationRequest& request){auto out=publisher.handle(request,now);if(lose_commit&&request.operation==app::FtnConfigurationOperation::commit)throw std::runtime_error("lost after commit");return out;}
};
Json rename(const web::WebFtnDraft& d,const char* name){auto values=web::web_ftn_network(d.configuration.networks[0]);values["name"]=name;return {{"revision",std::to_string(d.configuration.revision)},{"action","network_save"},{"network","TEST"},{"record",""},{"values",values},{"secrets",Json::object()}};}
}
int main(){try{
    {
        Fixture f;web::WebFtnDrafts d([&](const auto& r){return f.exchange(r);},{},[&]{return f.token();},[&]{return f.now;});auto value=d.open("setup");
        auto first=rename(value,"First staged step"),second=rename(value,"Second staged step");second["network"]="MISSING";
        refuses([&]{d.edit_batch("setup",value.handle,value.edit_revision,Json::array({first,second}));});
        check(d.read("setup",value.handle).configuration.networks[0].name=="Test"&&d.read("setup",value.handle).edit_revision==1&&f.changes==0,"failed later setup field publishes none of the batch or SQL");
        second["network"]="TEST";value=d.edit_batch("setup",value.handle,value.edit_revision,Json::array({first,second}));
        check(value.edit_revision==2&&value.configuration.networks[0].name=="Second staged step"&&f.changes==0,"successful multi-record setup step advances one draft revision only");
        refuses([&]{d.edit_batch("other",value.handle,value.edit_revision,Json::array({first}));});refuses([&]{d.edit_batch("setup",value.handle,1,Json::array({first}));});
        d.discard("setup",value.handle);check(f.saved.networks[0].name=="Test"&&f.changes==0,"cancel setup leaves durable network unchanged");
    }
    Fixture f;web::WebFtnDrafts drafts([&](const auto& request){return f.exchange(request);},{},[&]{return f.token();},[&]{return f.now;});
    auto a=drafts.open("owner-a"),b=drafts.open("owner-b");check(a.handle!=b.handle&&a.configuration.revision==1,"separate current drafts opened");
    refuses([&]{drafts.read("owner-b",a.handle);});refuses([&]{drafts.discard("owner-b",a.handle);});
    a=drafts.edit("owner-a",a.handle,a.edit_revision,rename(a,"Draft A"));check(a.edit_revision==2&&f.saved.networks[0].name=="Test"&&f.changes==0,"stage without publication");
    refuses([&]{drafts.edit("owner-a",a.handle,1,rename(a,"old browser tab"));});check(drafts.read("owner-a",a.handle).configuration.networks[0].name=="Draft A","lost edit response can read back without repeat");
    a=drafts.prepare("owner-a",a.handle,a.edit_revision);check(a.state==web::WebFtnDraftState::review&&a.review&&a.review->shortened==1&&f.changes==0,"exact shortening preview precedes publication");
    refuses([&]{drafts.edit("owner-a",a.handle,a.edit_revision,rename(a,"mutated review"));});
    b=drafts.prepare("owner-b",b.handle,b.edit_revision);check(b.state==web::WebFtnDraftState::editing&&b.result&&b.result->code==Code::busy,"busy peer keeps draft and cannot steal prepared transfer");
    const auto cancelled=a.operation;a=drafts.cancel_review("owner-a",a.handle);check(a.state==web::WebFtnDraftState::editing&&!a.review&&f.changes==0,"Cancel preserves settings and dates");
    a=drafts.prepare("owner-a",a.handle,a.edit_revision);check(a.operation!=cancelled,"new explicit review receives distinct operation");
    refuses([&]{drafts.commit("owner-a",a.handle,cancelled);});refuses([&]{drafts.commit("owner-b",a.handle,a.operation);});
    f.lose_commit=true;a=drafts.commit("owner-a",a.handle,a.operation);check(a.state==web::WebFtnDraftState::submitted&&a.result&&a.result->code==Code::unavailable&&f.changes==1,"lost commit reply locks draft with unknown result");
    refuses([&]{drafts.commit("owner-a",a.handle,a.operation);});refuses([&]{drafts.prepare("owner-a",a.handle,a.edit_revision);});refuses([&]{drafts.cancel_review("owner-a",a.handle);});
    a=drafts.check_result("owner-a",a.handle);check(a.result->code==Code::ok&&a.result->revision==2&&f.changes==1,"read-only receipt resolves exact operation once");
    f.lose_receipt=true;a=drafts.check_result("owner-a",a.handle);check(a.result->code==Code::ok,"unavailable later lookup does not erase known saved result");f.lose_receipt=false;f.lose_commit=false;
    b=drafts.prepare("owner-b",b.handle,b.edit_revision);check(b.result&&b.result->code==Code::conflict&&b.state==web::WebFtnDraftState::editing,"stale catalog remains visible and refused");
    drafts.discard("owner-a",a.handle);drafts.discard("owner-b",b.handle);refuses([&]{drafts.read("owner-a",a.handle);});
    a=drafts.open("owner-a");a=drafts.prepare("owner-a",a.handle,a.edit_revision);f.now+=60001;a=drafts.commit("owner-a",a.handle,a.operation);
    check(a.result&&a.result->code==Code::conflict&&f.changes==1,"expired native retention review cannot restart itself");drafts.discard("owner-a",a.handle);
    a=drafts.open("owner-a");a=drafts.prepare("owner-a",a.handle,a.edit_revision);f.now+=900001;
    refuses([&]{drafts.read("owner-a",a.handle);});b=drafts.open("owner-b");b=drafts.prepare("owner-b",b.handle,b.edit_revision);check(b.state==web::WebFtnDraftState::review,"expired draft releases its own prepared native writer");
    drafts.clear();check(f.changes==1,"clear all drafts never publishes");
    a=drafts.open("owner-a");a=drafts.edit("owner-a",a.handle,a.edit_revision,rename(a,"Long editing session"));
    for(unsigned i=0;i<20;++i){f.now+=60000;a=drafts.read("owner-a",a.handle);}
    check(a.configuration.networks[0].name=="Long editing session"&&f.changes==1,"read-only renewal preserves active draft beyond original idle lifetime without publication");
    f.now+=900001;refuses([&]{drafts.read("owner-a",a.handle);});
    for(unsigned i=0;i<4;++i)(void)drafts.open("owner-a");
    refuses([&]{drafts.open("owner-a");});drafts.clear();
    a=drafts.open("owner-a");auto bad=rename(a,"bad");bad["values"]["enabled"]=true;refuses([&]{drafts.edit("owner-a",a.handle,1,bad);});check(drafts.read("owner-a",a.handle).edit_revision==1,"invalid candidate preserves previous draft revision");
    check(web::web_ftn_token(web::web_ftn_token_text(a.handle))==a.handle,"canonical opaque handle roundtrip");
    refuses([&]{web::web_ftn_token(std::string(32,'0'));});refuses([&]{web::web_ftn_token(std::string(32,'G'));});
    Json query={{"edit_revision",std::to_string(a.edit_revision)},{"section","networks"},{"network",""},{"record","TEST"},{"offset","0"},{"search",""}};
    auto page=web::web_ftn_draft_page(a,query);check(page["rows"].size()==1&&page["values"]["name"]=="Draft A","named network page uses exact draft");
    query["edit_revision"]="999";refuses([&]{web::web_ftn_draft_page(a,query);});query["edit_revision"]=std::to_string(a.edit_revision);
    query["record"]="MISSING";refuses([&]{web::web_ftn_draft_page(a,query);});
    for(unsigned i=0;i<60;++i){app::FtnMessageEcho e;e.network="TEST";e.tag="CHAT."+std::to_string(i);e.description="Searchable";a.configuration.message_echoes.push_back(e);}
    app::FtnLink link;link.network="TEST";link.code="HUB";link.name="Hub";link.credentials.session={1,2,3};a.configuration.links.push_back(link);
    query["section"]="echoes";query["network"]="TEST";query["record"]="";
    page=web::web_ftn_draft_page(a,query);check(page["rows"].size()==25&&page["total"]==60&&page["next"]==true,"bounded first echo page");
    query["offset"]="50";page=web::web_ftn_draft_page(a,query);check(page["rows"].size()==10&&page["next"]==false,"last page retains exact total");
    query["search"]="chat.59";query["offset"]="0";page=web::web_ftn_draft_page(a,query);check(page["rows"].size()==1&&page["rows"][0]["id"]=="CHAT.59","case-insensitive named echo search");
    query["search"]="";query["section"]="links";query["record"]="HUB";page=web::web_ftn_draft_page(a,query);check(!page["values"].contains("credentials")&&!page["values"].contains("session"),"ordinary selected connection projection omits credentials");
    query["section"]="permissions";page=web::web_ftn_draft_page(a,query);check(page["rows"].size()==25&&page["rows"][0]["receive"]==false&&page["rows"][0]["manage"]==false,"ungranted named echo permission projection");
    query["record"]="MISSING";refuses([&]{web::web_ftn_draft_page(a,query);});
    a.configuration.networks[0].code="TeSt";a.configuration.message_echoes[0].network="tEsT";a.configuration.message_echoes[0].tag="chat.Mixed";
    a.configuration.permissions.push_back({"test","hub","CHAT.MIXED",false,true,false,false});
    query["section"]="permissions";query["network"]="TEST";query["record"]="HUB";query["search"]="mixed";page=web::web_ftn_draft_page(a,query);
    check(page["rows"].size()==1&&page["rows"][0]["tag"]=="chat.Mixed"&&page["rows"][0]["receive"]==true&&!page["rows"][0]["send"].get<bool>(),"permission projection joins valid mixed-case references without hiding saved access");
    query["section"]="echoes";query["record"]="CHAT.MIXED";page=web::web_ftn_draft_page(a,query);check(page["values"]["tag"]=="chat.Mixed","selected echo projection returns stored mixed-case identity");
    Fixture imported;web::WebFtnDrafts import_drafts([&](const auto& request){return imported.exchange(request);},{},[&]{return imported.token();},[&]{return imported.now;},[](std::string source,bool,const std::atomic<bool>& cancelled){
        if(source=="/cancel.na"){while(!cancelled.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));return web::FtnEchoListDownload{web::FtnEchoListDownloadCode::cancelled,{}};}
        if(source=="/failed.na")return web::FtnEchoListDownload{};
        return web::FtnEchoListDownload{web::FtnEchoListDownloadCode::ok,"TEST.NEW New discussion\nTEST.OTHER Other discussion\nTEST.NEW Duplicate\n"};
    });
    auto editor=import_drafts.open("import-owner");auto list=import_drafts.start_list("import-owner",editor.handle,editor.edit_revision,"TEST","/test.na",false,false);
    const auto wait_list=[&](app::FtnTransferToken token){for(unsigned attempt=0;attempt<100;++attempt){auto result=import_drafts.read_list("import-owner",editor.handle,token);if(!result.active)return result;std::this_thread::sleep_for(std::chrono::milliseconds(1));}throw std::runtime_error("list worker did not finish");};
    list=wait_list(list.token);check(list.ready&&list.list.entries.size()==2&&list.list.issues.size()==1&&imported.changes==0,"source load parses duplicate diagnostics without publishing or subscribing");
    refuses([&]{import_drafts.read_list("other-owner",editor.handle,list.token);});
    web::WebFtnImportCatalog context;context.revision=editor.configuration.message_catalog_revision;context.groups={{1,"Local"}};context.echoes={{1,1}};
    auto plan=import_drafts.import_preview("import-owner",editor.handle,editor.edit_revision,list.token,context,{"TEST.NEW"},{10,20,30},false);
    check(plan.issues.empty()&&plan.selected==1&&plan.created==1&&import_drafts.read("import-owner",editor.handle).configuration.message_echoes.empty(),"import preview leaves actual draft unchanged and limits explicit selection");
    auto failed=import_drafts.start_list("import-owner",editor.handle,editor.edit_revision,"TEST","/failed.na",false,false);failed=wait_list(failed.token);check(!failed.ready&&import_drafts.read_list("import-owner",editor.handle,list.token).ready,"failed replacement load retains previous reviewed list");
    const auto previous_revision=editor.edit_revision;editor=import_drafts.import_apply("import-owner",editor.handle,editor.edit_revision,list.token,context,{"TEST.NEW"},{10,20,30},false);
    check(editor.configuration.message_echoes.size()==1&&editor.configuration.message_echoes[0].read_level==20&&editor.configuration.message_echoes[0].post_level==30&&imported.changes==0,"explicit import Apply stages selected levels without saving");
    refuses([&]{import_drafts.import_apply("import-owner",editor.handle,previous_revision,list.token,context,{"TEST.NEW"},{10,20,30},false);});
    refuses([&]{import_drafts.import_preview("import-owner",editor.handle,editor.edit_revision,list.token,context,{"UNKNOWN"},{10,20,30},false);});
    auto changed_catalog=context;++changed_catalog.revision;refuses([&]{import_drafts.import_preview("import-owner",editor.handle,editor.edit_revision,list.token,changed_catalog,{"TEST.NEW"},{10,20,30},false);});
    const auto cancelling=import_drafts.start_list("import-owner",editor.handle,editor.edit_revision,"TEST","/cancel.na",false,false);import_drafts.cancel_list("import-owner",editor.handle,cancelling.token);const auto cancelled_list=wait_list(cancelling.token);check(!cancelled_list.active&&!cancelled_list.ready&&import_drafts.read_list("import-owner",editor.handle,list.token).ready,"explicit load cancel preserves previous list and staged draft");
    for(const auto& paths:std::vector<std::pair<std::string,std::string>>{{"/board/FTN/setup.na","ftn/setup.na"},{"/absolute/setup.na","/absolute/setup.na"},{"https://example.invalid/list.na","https://example.invalid/list.na"}}){
        const bool url=paths.first.front()!='/';const auto pending=import_drafts.start_list("import-owner",editor.handle,editor.edit_revision,"TEST",paths.first,url,false,paths.second);const auto loaded=wait_list(pending.token);
        check(loaded.source==paths.first&&loaded.selected_source==paths.second&&loaded.url==url,"owned list binds resolved reader source separately from typed saved source");
        editor=import_drafts.import_apply("import-owner",editor.handle,editor.edit_revision,loaded.token,context,{"TEST.OTHER"},{10,20,30},false);
        const auto& net=editor.configuration.networks[0];check(url?net.echo_list_url==paths.second&&net.echo_list_source==app::FtnEchoListSource::url:net.echo_list_file==paths.second&&net.echo_list_source==app::FtnEchoListSource::local_file,"import preserves relative, absolute and URL choices without rewriting saved source");
    }
    (void)import_drafts.start_list("import-owner",editor.handle,editor.edit_revision,"TEST","/cancel.na",false,false);const auto stopping=std::chrono::steady_clock::now();import_drafts.clear();check(std::chrono::steady_clock::now()-stopping<std::chrono::seconds(2)&&imported.changes==0,"draft clear cancels and joins owned list workers without publication");
    std::cout<<"PASS "<<checks<<" FTN web draft checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<" "<<e.what()<<'\n';return 1;}}
