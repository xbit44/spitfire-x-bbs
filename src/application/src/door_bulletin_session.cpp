#include "sfx/application/door_bulletin_session.hpp"
#include "sfx/application/message_source.hpp"
#include "sfx/application/file_presentation.hpp"
#include "sfx/application/control_panel.hpp"
#include <algorithm>
#include <map>

namespace sfx::application {
namespace {
struct RecordedReader final:ITrustedDisplayAssetReader {
    ITrustedDisplayAssetReader& source;std::map<std::string,TrustedDisplayAssetReadResult> assets;std::size_t bytes{};bool stable{true};
    explicit RecordedReader(ITrustedDisplayAssetReader& s):source(s){}
    TrustedDisplayAssetReadResult read_asset(std::string_view name,std::size_t maximum)override{
        auto a=source.read_asset(name,maximum);if(a.code!=TrustedDisplayAssetReadCode::available)return a;
        auto old=assets.find(a.logical_identity);
        if(old!=assets.end()){if(old->second.bytes!=a.bytes||old->second.stable_file_identity!=a.stable_file_identity)stable=false;}
        else{if(assets.size()>kMaximumTrustedDisplayIncludeInstances||a.bytes.size()>kMaximumTrustedDisplayAggregateSourceBytes-bytes){stable=false;return {TrustedDisplayAssetReadCode::invalid,{},{},{},{}};}bytes+=a.bytes.size();assets.emplace(a.logical_identity,a);}
        return a;
    }
    bool verify(){if(!stable)return false;for(const auto& item:assets){const auto a=source.read_asset(item.first,kMaximumTrustedDisplaySourceBytes);if(a.code!=TrustedDisplayAssetReadCode::available||a.logical_identity!=item.second.logical_identity||a.bytes!=item.second.bytes||a.stable_file_identity!=item.second.stable_file_identity)return false;}return true;}
    std::string fingerprint()const{std::string out;for(const auto& item:assets){for(auto n:{item.first.size(),item.second.bytes.size()})for(unsigned i=0;i<8;++i)out+=static_cast<char>(static_cast<std::uint64_t>(n)>>(8*i));out+=item.first;out+=item.second.bytes;}return out;}
};
struct CapturedReader final:ITrustedDisplayAssetReader {
    TrustedDisplayAssetReadResult asset;
    TrustedDisplayAssetReadResult read_asset(std::string_view name,std::size_t maximum)override{if(name!=asset.logical_identity)return {TrustedDisplayAssetReadCode::not_found,{},{},{},{}};if(asset.bytes.size()>maximum)return {TrustedDisplayAssetReadCode::invalid,{},{},{},{}};return asset;}
};
std::string key(std::string_view k,std::string_view text,bool ansi){return (ansi?menu_text_sgr(7):std::string{})+"["+(ansi?menu_text_sgr(15):std::string{})+std::string(k)+(ansi?menu_text_sgr(7):std::string{})+"] "+std::string(text);}
}
DoorBulletinRefreshResult refresh_door_bulletin(IDoorStorage& storage,const MessageConfigurationActor& actor,
    DoorBulletinId id,TrustedDisplaySelectionRequest request,const DoorBulletinSourcePorts& ports,std::int64_t expected){
    const auto source=storage.door_bulletin_source(actor,id);
    if(!source||!source.value)return {source.status,{}, {},"Door Bulletin definition is unavailable."};
    if(expected&&expected!=source.value->definition.revision)return {{StorageErrorCode::conflict,"Door Bulletin changed"},{},{},"Door Bulletin settings changed. Choose again."};
    const auto& s=*source.value;const auto& d=s.definition;
    DoorBulletinPublication publication;publication.id=id;publication.definition_revision=d.revision;publication.door_revision=s.door_revision;publication.variant="UNAVAILABLE";
    std::optional<RenderedTrustedDisplayProgram> display;std::string diagnostic;
    try{
        request.door_asset_number=id.value;request.bulletin_number=request.welcome_number=1;
        if(d.source==DoorBulletinSourceKind::display){
            auto reader=ports.display_reader?ports.display_reader():nullptr;
            if(reader){RecordedReader recorded(*reader);TrustedDisplaySelector selector(recorded);request.identity=TrustedDisplayIdentity::door_bulletin_page;auto selected=selector.select(request);
                if(selected.available()&&recorded.verify()){display=std::move(selected.program);publication.variant=display->selected_logical_asset;publication.fingerprint_input=recorded.fingerprint();}
                else diagnostic=selected.available()?"door_bulletin_static_source_changing":selected.code==TrustedDisplaySelectionCode::not_found?"door_bulletin_static_source_missing":selected.code==TrustedDisplaySelectionCode::invalid?"door_bulletin_static_source_invalid":"door_bulletin_static_source_unreadable";}
            else diagnostic="door_bulletin_display_root_unavailable";
        }else{
            auto read=ports.generated_reader?ports.generated_reader(s):DoorBulletinFileCapture{};
            auto captured=std::move(read.asset);diagnostic=std::move(read.diagnostic);
            if(captured.code==TrustedDisplayAssetReadCode::available&&captured.bytes.size()<=kMaximumTrustedDisplaySourceBytes){
                const auto caller_graphics=request.graphics;request.identity=TrustedDisplayIdentity::door_bulletin_generated;request.graphics=d.format;
                captured.logical_identity="DB"+std::to_string(1000U+id.value).substr(1)+(d.format==GraphicsMode::ansi?".ANS":".ASC");
                CapturedReader reader;reader.asset=std::move(captured);TrustedDisplaySelector selector(reader);auto selected=selector.select(request);
                std::optional<MessageCanvas> canvas;
                if(selected.available()&&d.format==GraphicsMode::ansi){const auto content=display_asset_content(reader.asset.bytes);if(content)canvas=message_received_ansi_canvas(*content,false);}
                if(selected.available()&&(d.format!=GraphicsMode::ansi||canvas)){
                    display=std::move(selected.program);publication.variant=d.format==GraphicsMode::ansi?"GENERATED-ANS":"GENERATED-ASC";
                    publication.captured_content.assign(reader.asset.bytes.begin(),reader.asset.bytes.end());publication.fingerprint_input=publication.variant+"\n"+reader.asset.bytes;
                    if(caller_graphics==GraphicsMode::ascii&&d.format==GraphicsMode::ansi){
                        // Reuse the bounded 80-column terminal canvas: cursor
                        // positioning must preserve scoreboard rows/columns.
                        TerminalDisplayProgram plain;
                        for(std::size_t row=0;row<canvas->extent;row+=100){auto chunk=message_canvas_page(*canvas,row,100,false);if(!chunk){display.reset();break;}plain.output_byte_count+=chunk->output_byte_count;plain.operations.insert(plain.operations.end(),chunk->operations.begin(),chunk->operations.end());}
                        if(display){for(auto& op:plain.operations)if(op.kind==TerminalDisplayOperationKind::logical_newline)op.kind=TerminalDisplayOperationKind::paging_newline;if(!valid_terminal_display_program(plain))display.reset();else display->terminal_program=std::move(plain);}
                    }
                }
            }
            if(!display&&diagnostic.empty())diagnostic=captured.code==TrustedDisplayAssetReadCode::not_found?"door_bulletin_generated_source_missing":captured.code==TrustedDisplayAssetReadCode::available?"door_bulletin_generated_content_invalid":"door_bulletin_generated_source_unavailable";
        }
    }catch(...){display.reset();diagnostic="door_bulletin_capture_failed";}
    publication.available=display.has_value();
    if(!publication.available){publication.variant="UNAVAILABLE";publication.fingerprint_input.clear();publication.captured_content.clear();}
    const auto published=storage.publish_door_bulletin(actor,publication);
    if(!published||!published.value){display.reset();diagnostic=published.status.code==StorageErrorCode::conflict?"door_bulletin_publication_conflict":"door_bulletin_publication_failed";}
    if(!diagnostic.empty()&&ports.diagnostic)ports.diagnostic(id,door_bulletin_diagnostic_code(diagnostic));
    return {published.status,std::move(display),published.value.value_or(DoorBulletinPublicationResult{}),std::move(diagnostic)};
}
std::string door_bulletin_diagnostic_code(std::string_view text){
    if(!text.empty()&&text.size()<=96&&std::all_of(text.begin(),text.end(),[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';}))return std::string(text);
    if(text.find("permission")!=text.npos||text.find("security")!=text.npos||text.find("read/write/delete")!=text.npos||text.find("DACL")!=text.npos||text.find("owner")!=text.npos)return "door_bulletin_source_permissions";
    return "door_bulletin_source_validation_failed";
}
DoorBulletinPreview preview_door_bulletin(IDoorStorage& storage,DoorBulletinId id,TrustedDisplaySelectionRequest request,const DoorBulletinSourcePorts& sources,std::int64_t expected_catalog_revision){
    DoorBulletinPreview preview;preview.id=id;preview.title="Door Bulletin "+std::to_string(id.value);
    const auto catalog=storage.door_bulletin_configuration({true,{}});if(!catalog||!catalog.value){preview.diagnostic="Door Bulletin settings could not be read.";return preview;}
    if(expected_catalog_revision&&catalog.value->revision!=expected_catalog_revision){preview.diagnostic="Door Bulletin settings changed elsewhere. Reload before checking this source.";return preview;}
    const auto found=std::find_if(catalog.value->entries.begin(),catalog.value->entries.end(),[&](const auto& e){return e.definition.id==id;});if(found==catalog.value->entries.end()){preview.diagnostic="Door Bulletin definition is no longer available.";return preview;}preview.title=found->definition.title;
    const auto captured=refresh_door_bulletin(storage,{true,{}},id,std::move(request),sources,found->definition.revision);
    if(!captured.status||!captured.display){preview.diagnostic=captured.diagnostic.empty()?"Door Bulletin source is unavailable.":captured.diagnostic;for(auto& c:preview.diagnostic)if(static_cast<unsigned char>(c)<32||c==127)c=' ';if(preview.diagnostic.size()>512)preview.diagnostic.resize(512);return preview;}
    ControlPanelTerminalMirror mirror;for(const auto& op:captured.display->terminal_program.operations)switch(op.kind){
        case TerminalDisplayOperationKind::cp437_text:case TerminalDisplayOperationKind::ansi_sequence:mirror.consume(op.bytes);break;
        case TerminalDisplayOperationKind::logical_newline:case TerminalDisplayOperationKind::paging_newline:mirror.consume("\r\n");break;
        case TerminalDisplayOperationKind::clear_screen:mirror.clear();break;default:break;
    }
    preview.available=true;preview.rows=25;for(const auto& cell:mirror.snapshot().cells)preview.cells.push_back({cell.character,cell.attribute});preview.diagnostic="Preview shows the last screen of long bulletins. Callers can read the whole bulletin.";return preview;
}
bool run_door_bulletin_session(IDoorStorage& storage,const MenuSessionPorts& ports,
    const std::function<std::optional<MessageActor>()>& actor,const DoorBulletinRefresh& refresh,MenuInputDecoder* invoker_input){
    std::size_t page{},highlight{};bool highlighted{};std::string feedback;MenuInputDecoder local_input;auto& input=invoker_input?*invoker_input:local_input;
    if(ports.menu_entered)ports.menu_entered(MenuId::door_bulletins);
    for(;;){
        const auto a=actor();const auto state=ports.snapshot();if(!a||!state)return false;
        const auto catalog=storage.door_bulletins(*a);if(!catalog||!catalog.value)return ports.write(std::string(kDoorBulletinUnavailable)+"\r\n");
        const auto& items=catalog.value->entries;const bool ansi=state->graphics==GraphicsMode::ansi;
        const auto width=std::clamp<unsigned>(state->columns,40,200);const auto height=std::clamp<unsigned>(state->rows,12,50)-9;
        const auto pages=std::max<std::size_t>(1,(items.size()+height-1)/height);page=std::min(page,pages-1);
        const bool lightbar=ansi&&state->lightbars_enabled&&state->preferences.preferences.lightbar;
        if(highlight>=items.size()){highlight=0;highlighted=false;}
        TrustedDisplaySelectionRequest art;art.identity=TrustedDisplayIdentity::menu_door_bulletins;art.graphics=state->graphics;art.authenticated_security_level=state->access.security_level;art.context=state->context;
        const auto artwork=ports.artwork(art);
        const auto draw=[&]{
            if(!ports.write(ansi?"\x1b[0m\x1b[2J\x1b[H":"\r\n"))return false;
            if(artwork.available()){auto program=artwork.program->terminal_program;for(auto& op:program.operations)if(op.kind==TerminalDisplayOperationKind::paging_newline)op.kind=TerminalDisplayOperationKind::logical_newline;program.operations.erase(std::remove_if(program.operations.begin(),program.operations.end(),[&](const auto& op){if(op.kind!=TerminalDisplayOperationKind::pause)return false;program.output_byte_count-=2;return true;}),program.operations.end());if(!ports.display(program)||!ports.write("\r\n"))return false;}
            if(!ports.write((ansi?menu_text_sgr(7):std::string{})+"Door Bulletins\r\n\r\n"))return false;
            for(auto i=page*height;i<std::min(items.size(),(page+1)*height);++i){const auto& item=items[i];const auto title=file_terminal_text(item.title,width-8);const bool selected=lightbar&&highlighted&&i==highlight;const auto row=selected?menu_text_sgr(state->selected_foreground,state->selected_background)+"["+std::to_string(item.id.value)+"] "+title:key(std::to_string(item.id.value),title,ansi);
                if(!ports.write(row+(ansi?"\x1b[0m":"")+"\r\n"))return false;}
            if(items.empty()&&!ports.write("No Door Bulletins are enabled.\r\n"))return false;
            if(!feedback.empty()&&!ports.write("\r\n"+feedback+"\r\n"))return false;
            if(!ports.write("\r\n"+key("P","Prev",ansi)+"  "+key("N","Next",ansi)+"  "+std::to_string(page+1)+"/"+std::to_string(pages)+"\r\n"+key("Q","Return to Door Menu",ansi)+"\r\n"))return false;
            return ports.write("Number + Enter, or command: ")&&ports.write(ansi?menu_text_sgr(state->input_color):std::string{});
        };
        // Reuse the invoker's CR/LF state across X/Q. Numeric IDs always need
        // Enter, so a selection's trailing Enter cannot acknowledge its page.
        // Letter hotkeys and lightbar Enter retain the caller's normal mode.
        input.reset(state->preferences.preferences.hotkey,true,false,lightbar);
        if(!draw())return false;
        const auto chosen=ports.read(input,draw);if(!chosen)return false;
        if(chosen->kind==MenuInputKind::back)return true;
        if(chosen->kind==MenuInputKind::command){if(chosen->value=='Q')return true;if(chosen->value=='N'){if(page+1<pages)++page;highlighted=false;continue;}if(chosen->value=='P'){if(page)--page;highlighted=false;continue;}}
        if(lightbar&&(chosen->kind==MenuInputKind::up||chosen->kind==MenuInputKind::down)){if(!items.empty()){if(!highlighted){highlight=page*height;highlighted=true;}else if(chosen->kind==MenuInputKind::up)highlight=highlight?highlight-1:items.size()-1;else highlight=(highlight+1)%items.size();page=highlight/height;}continue;}
        unsigned number{};if(chosen->kind==MenuInputKind::number)number=chosen->value;else if(lightbar&&chosen->kind==MenuInputKind::activate&&highlighted&&!items.empty())number=items[highlight].id.value;
        const auto found=std::find_if(items.begin(),items.end(),[&](const auto& item){return item.id.value==number;});if(found==items.end()){feedback="Choose a listed Bulletin ID, N, P or Q.";continue;}
        art.identity=TrustedDisplayIdentity::door_bulletin_page;art.door_asset_number=static_cast<std::uint16_t>(number);
        const auto result=refresh?refresh({static_cast<std::uint16_t>(number)},art,found->revision):DoorBulletinRefreshResult{};
        if(!result.status||!result.display){feedback=std::string(kDoorBulletinUnavailable);continue;}
        auto program=result.display->terminal_program;if(!prepare_acknowledged_display(program,state->graphics,std::nullopt,true)||!ports.display(program))return false;
        input.discard_pending();feedback.clear();
    }
}
} // namespace sfx::application
