#include "sfx/application/message_source.hpp"
#include <algorithm>
#include <array>

namespace sfx::application {
namespace {
struct Refusal {MessageSourceValidation why;};
[[noreturn]] void refuse(MessageSourceValidation why){throw Refusal{why};}
constexpr std::size_t width=80;
constexpr std::uint8_t ansi_to_vga[]={0,4,2,6,1,5,3,7};
constexpr std::uint32_t no_rgb=0xffffffffU;
constexpr std::uint32_t vga_rgb[]={0x000000,0x0000aa,0x00aa00,0x00aaaa,0xaa0000,0xaa00aa,0xaa5500,0xaaaaaa,0x555555,0x5555ff,0x55ff55,0x55ffff,0xff5555,0xff55ff,0xffff55,0xffffff};
#include "message_source_profile1.inc"
// Received-only parsing is compiled independently of the preserved profile1
// implementation. Its byte/profile conditions intentionally fold to constants.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4127)
#endif
template<bool received_v2=false> struct CanvasParser {
    MessageCanvas out;
    std::size_t x{},y{},sx{},sy{};
    std::uint8_t attr{7};
    bool sgr_colors{true};
    std::uint32_t foreground{no_rgb},background{no_rgb};
    bool wrap{true},pending{},saved{},saved_pending{};
    CanvasParser(){out.cells.resize(width*kMessageCanvasRows);out.extent=1;}
    void rgb(bool front,unsigned red,unsigned green,unsigned blue){
        if(red>255||green>255||blue>255)refuse(MessageSourceValidation::control);
        const auto value=(red<<16)|(green<<8)|blue;unsigned nearest{},distance=~0U;
        for(unsigned i=0;i<(front?16U:8U);++i){const auto r=int(red)-int((vga_rgb[i]>>16)&255),g=int(green)-int((vga_rgb[i]>>8)&255),b=int(blue)-int(vga_rgb[i]&255);const auto d=static_cast<unsigned>(r*r+g*g+b*b);if(d<distance){distance=d;nearest=i;}}
        if(front){foreground=value;attr=static_cast<std::uint8_t>((attr&240)|nearest);}else{background=value;attr=static_cast<std::uint8_t>((attr&143)|(nearest<<4));}
    }
    void erase(std::size_t begin,std::size_t end){
        charge(end-begin);std::fill(out.cells.begin()+begin,out.cells.begin()+end,MessageCanvasCell{32,attr});
        if(received_v2){std::fill(out.foreground_rgb.begin()+begin,out.foreground_rgb.begin()+end,foreground);std::fill(out.background_rgb.begin()+begin,out.background_rgb.begin()+end,background);}
    }
    void touch(){
        if(x>=width || y>=kMessageCanvasRows)refuse(MessageSourceValidation::bounds);
        out.extent=static_cast<std::uint16_t>(std::max<std::size_t>(out.extent,y+1));
    }
    void charge(std::size_t n){
        if(n>kMessageCanvasWork-out.work)refuse(MessageSourceValidation::budget);
        out.work+=n;
    }
    void put(std::uint8_t b){
        if(pending && wrap){x=0;++y;pending=false;}
        touch();charge(1);out.cells[y*width+x]={b,attr};
        if(received_v2){out.foreground_rgb[y*width+x]=foreground;out.background_rgb[y*width+x]=background;}
        if(x+1<width)++x;else pending=true;
    }
    void sgr(unsigned n){
        const auto prior=attr;
        if(received_v2){if(!n)foreground=background=no_rgb;else if((n>=30&&n<=37)||n==39||(n>=90&&n<=97))foreground=no_rgb;else if((n>=40&&n<=47)||n==49||(n>=100&&n<=107))background=no_rgb;}
        if(n==0)attr=7;
        else if(n==1)attr|=8;
        else if(n==22)attr&=247;
        else if(n==5)attr|=128;
        else if(n==25)attr&=127;
        else if(n>=30 && n<=37)attr=(attr&248)|ansi_to_vga[n-30];
        else if(n==39)attr=(attr&248)|7;
        else if(n>=40 && n<=47)attr=(attr&143)|(ansi_to_vga[n-40]<<4);
        else if(n==49)attr&=143;
        else if(received_v2&&n>=90&&n<=97)attr=(attr&240)|8|ansi_to_vga[n-90];
        else if(received_v2&&n>=100&&n<=107)attr=(attr&15)|128|(ansi_to_vga[n-100]<<4);
        else if(!received_v2)refuse(MessageSourceValidation::control);
        if(!sgr_colors)attr=prior;
    }
    bool pipe(std::string_view source,std::size_t& i,bool enabled,std::uint8_t b){
        if(!enabled || b!='|' || i+1>=source.size() || source[i]<'0' || source[i]>'9' || source[i+1]<'0' || source[i+1]>'9')return false;
        const unsigned n=(source[i]-'0')*10+source[i+1]-'0';if(n>31)return false;
        i+=2;attr=static_cast<std::uint8_t>(n<16?(attr&240)|n:(attr&15)|((n-16)<<4));if(received_v2)(n<16?foreground:background)=no_rgb;return true;
    }
    MessageCanvas finish(){
        const bool plain_text=!out.text.rows.empty();
        for(std::size_t row=0;row<out.extent;++row){
            std::string text;
            for(std::size_t col=0;col<width;++col){
                const auto c=out.cells[row*width+col];text+=static_cast<char>(c.glyph);
                if(c.glyph!=' ' || (c.attribute&112)!=0 || (received_v2&&out.background_rgb[row*width+col]!=no_rgb&&out.background_rgb[row*width+col]!=0))out.nonblank=true;
            }
            while(!text.empty() && text.back()==' ' && (out.cells[row*width+text.size()-1].attribute&112)==0&&(!received_v2||out.background_rgb[row*width+text.size()-1]==no_rgb||out.background_rgb[row*width+text.size()-1]==0))text.pop_back();
            if(!plain_text){out.text.rows.push_back(text);out.text.canonical_lf+=text;out.text.canonical_lf+='\n';}
        }
        return std::move(out);
    }
    MessageCanvas ansi(std::string_view src,bool pipes){
        for(std::size_t i=0;i<src.size();){
            if(++out.operations>kMessageSourceBytes)refuse(MessageSourceValidation::budget);
            const auto b=static_cast<std::uint8_t>(src[i++]);
            if(b==27){
                if(i==src.size())refuse(MessageSourceValidation::control);
                if(received_v2&&src[i]!='['){
                    const auto command=src[i++];
                    if(command=='7'){sx=x;sy=y;saved_pending=pending;saved=true;}
                    else if(command=='8'&&saved){x=sx;y=sy;pending=saved_pending;touch();}
                    else if(command==']'||command=='P'||command=='_'||command=='^'){
                        bool ended=false;while(i<src.size()){const auto c=src[i++];if(c==7){ended=true;break;}if(c==27&&i<src.size()&&src[i]=='\\'){++i;ended=true;break;}}
                        if(!ended)refuse(MessageSourceValidation::control);
                    }else if(command=='('||command==')'){if(i==src.size())refuse(MessageSourceValidation::control);++i;}
                    continue;
                }
                if(src[i++]!='[')refuse(MessageSourceValidation::control);
                const auto start=i;const bool priv=i<src.size() && src[i]=='?';if(priv)++i;
                std::vector<unsigned> p;unsigned n=0,digits=0;bool seen=false;
                while(i<src.size() && (src[i]==';' || (src[i]>='0' && src[i]<='9'))){
                    if(i-start>=32)refuse(MessageSourceValidation::budget);
                    if(src[i]==';'){p.push_back(n);n=digits=0;seen=false;}
                    else {if(++digits>4)refuse(MessageSourceValidation::control);n=n*10+src[i]-'0';seen=true;if(n>1000)refuse(MessageSourceValidation::bounds);}
                    if(p.size()>=16)refuse(MessageSourceValidation::control);
                    ++i;
                }
                if(seen || !p.empty())p.push_back(n);
                if(i==src.size() || i-start>=32)refuse(MessageSourceValidation::control);
                const char final=src[i++];
                const auto one=[&](){if(p.size()>1)refuse(MessageSourceValidation::control);return p.empty() || !p[0]?1U:p[0];};
                if(priv){
                    if(p==std::vector<unsigned>{7} && (final=='h'||final=='l')){wrap=final=='h';pending=false;}
                    else if(!received_v2)refuse(MessageSourceValidation::control);
                }else if(final=='m'){if(p.empty())sgr(0);for(std::size_t si=0;si<p.size();++si){
                    if(received_v2&&(p[si]==38||p[si]==48)){
                        const bool front=p[si]==38;
                        if(si+4<p.size()&&p[si+1]==2){rgb(front,p[si+2],p[si+3],p[si+4]);si+=4;}
                        else if(si+2<p.size()&&p[si+1]==5){const auto color=p[si+2];if(color>255)refuse(MessageSourceValidation::control);unsigned value{};if(color<16)value=vga_rgb[color<8?ansi_to_vga[color]:8+ansi_to_vga[color-8]];else if(color>=232){value=8+10*(color-232);value=(value<<16)|(value<<8)|value;}else{const auto level=[](unsigned v){return v?55+40*v:0;};const auto index=color-16;value=(level(index/36)<<16)|(level(index/6%6)<<8)|level(index%6);}rgb(front,(value>>16)&255,(value>>8)&255,value&255);si+=2;}
                        else refuse(MessageSourceValidation::control);
                        continue;
                    }sgr(p[si]);}}
                else if(received_v2&&final=='t'){if(p.size()==4&&p[0]<=1&&p[1]<=255&&p[2]<=255&&p[3]<=255)rgb(p[0]==1,p[1],p[2],p[3]);}
                else if(received_v2&&(final=='A'||final=='B'||final=='D'||final=='E'||final=='F')){
                    const auto amount=one();pending=false;
                    if(final=='A'||final=='F')y=amount>y?0:y-amount;
                    else if(final=='B'||final=='E')y+=amount;
                    else x=amount>x?0:x-amount;
                    if(final=='E'||final=='F')x=0;
                    touch();
                }else if(received_v2&&(final=='G'||final=='d')){const auto position=one()-1;pending=false;if(final=='G')x=position;else y=position;touch();}
                else if(final=='C'){const auto amount=one();x=received_v2?std::min(width-1,x+amount):x+amount;pending=false;touch();}
                else if(final=='H' || final=='f'){
                    if(p.size()>2)refuse(MessageSourceValidation::control);
                    y=(p.empty() || !p[0]?1:p[0])-1;x=(p.size()<2 || !p[1]?1:p[1])-1;pending=false;touch();
                }else if(final=='s'){
                    if(!p.empty())refuse(MessageSourceValidation::control);
                    sx=x;sy=y;saved_pending=pending;saved=true;
                }else if(final=='u'){
                    if(!p.empty() || !saved)refuse(MessageSourceValidation::control);
                    x=sx;y=sy;pending=saved_pending;touch();
                }else if(final=='J'){
                    if(p==std::vector<unsigned>{2}){erase(0,out.cells.size());}
                    else if(received_v2&&p.size()<=1&&(p.empty()||p[0]<=1)){const bool before=!p.empty()&&p[0]==1;const auto begin=before?0:y*width+x,end=before?y*width+x+1:out.cells.size();erase(begin,end);}
                    else refuse(MessageSourceValidation::control);
                }else if(received_v2&&final=='K'){
                    if(p.size()>1||(!p.empty()&&p[0]>2))refuse(MessageSourceValidation::control);
                    const auto mode=p.empty()?0:p[0];
                    const auto begin=y*width+(mode==0?x:0),end=y*width+(mode==1?x+1:width);erase(begin,end);
                }else if(!received_v2)refuse(MessageSourceValidation::control);
            }else if(received_v2&&b==7){continue;}
            else if(b=='\r'){x=0;pending=false;}
            else if(b=='\n'){++y;pending=false;touch();}
            else if(b=='\t'){
                // Fixed eight-cell tab, including delayed wrap at column 80.
                const auto spaces=8-x%8;for(std::size_t s=0;s<spaces;++s)put(32);
            }else if(b<32 || b==127)refuse(MessageSourceValidation::control);
            else if(!pipe(src,i,pipes,b))put(b);
        }
        return finish();
    }
    MessageCanvas plain(std::string_view src,bool pipes,std::size_t operation_limit=kMessageSourceBytes){
        // Wrap the glyph/attribute pairs by the exact existing plain-body word
        // rule. Codes do not contribute cells; raw source remains untouched.
        std::vector<MessageCanvasCell> line;bool ended=false;
        const auto flush=[&]{
            std::size_t start=0;
            do {
                std::size_t count=std::min(width,line.size()-start);
                if(line.size()-start>width){
                    for(std::size_t n=count;n>0;--n)if(line[start+n-1].glyph==' '){count=n;break;}
                }
                if(y>=kMessageCanvasRows)refuse(MessageSourceValidation::bounds);
                out.extent=static_cast<std::uint16_t>(y+1);
                charge(count);std::copy_n(line.begin()+start,count,out.cells.begin()+y*width);
                std::string text;
                for(std::size_t n=0;n<count;++n)text+=static_cast<char>(line[start+n].glyph);
                out.text.rows.push_back(text);out.text.canonical_lf+=text;out.text.canonical_lf+='\n';
                ++y;start+=count;
            }while(start<line.size());
            line.clear();
        };
        for(std::size_t i=0;i<src.size();){
            if(++out.operations>operation_limit)refuse(MessageSourceValidation::budget);
            const auto b=static_cast<std::uint8_t>(src[i++]);
            if(b=='\r' || b=='\n'){
                if(b=='\r' && i<src.size() && src[i]=='\n')++i;
                flush();ended=true;
            }else{
                if((b<32 && b!='\t') || b==127)refuse(MessageSourceValidation::control);
                if(pipe(src,i,pipes,b))continue;
                const auto count=b=='\t'?8-line.size()%8:1;
                if(count>kMessageBodyBytes-line.size())refuse(MessageSourceValidation::size);
                for(std::size_t n=0;n<count;++n)line.push_back({b=='\t'?std::uint8_t{32}:b,attr});
                ended=false;
            }
        }
        if(!ended)flush();
        return finish();
    }
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
std::string rendition(std::uint8_t attr){
    std::string s="\x1b[0";
    if(attr&8)s+=";1";
    if(attr&128)s+=";5";
    s+=';'+std::to_string(30+ansi_to_vga[attr&7]);
    s+=';'+std::to_string(40+ansi_to_vga[(attr>>4)&7]);s+='m';return s;
}
} // namespace
ValidatedMessageSource validate_message_source(const MessageSource& source,std::uint32_t limit,std::uint8_t plain_color){
    ValidatedMessageSource result;
    if(plain_color>15 || source.profile!=1 || (source.format!=MessageSourceFormat::plain && source.format!=MessageSourceFormat::ansi_canvas))return result;
    if(source.bytes.empty() || source.bytes.size()>kMessageSourceBytes){result.validation=MessageSourceValidation::size;return result;}
    if(!limit || limit>kMaximumMessageLines){result.validation=MessageSourceValidation::line_limit;return result;}
    try{
        for(bool pipes:{false,true}){
            Profile1CanvasParser parser;
            if(source.format==MessageSourceFormat::plain)parser.attr=plain_color;
            auto canvas=source.format==MessageSourceFormat::ansi_canvas?parser.ansi(source.bytes,pipes):parser.plain(source.bytes,pipes);
            (pipes?result.on:result.off)=std::move(canvas);
        }
        result.charged_extent=std::max(result.off.extent,result.on.extent);
        if(result.charged_extent>limit)refuse(MessageSourceValidation::line_limit);
        if(!result.off.nonblank || !result.on.nonblank)refuse(MessageSourceValidation::blank);
        result.validation=MessageSourceValidation::valid;
    }catch(const Refusal& e){result.validation=e.why;result.off={};result.on={};result.charged_extent=0;}
    return result;
}
std::optional<MessageCanvas> message_plain_canvas(const MessageBody& body,bool pipes,std::uint8_t color){
    if(color>15 || !body || body.canonical_lf.size()>kMessageBodyBytes)return {};
    try {Profile1CanvasParser parser;parser.attr=color;return parser.plain(body.canonical_lf,pipes,kMessageBodyBytes);}
    catch(const Refusal&){return {};}
}
std::optional<MessageCanvas> message_synchronet_pipe_canvas(std::string_view source,std::uint8_t color){
    if(color>15 || source.empty() || source.size()>kMessageBodyBytes)return {};
    try{
        Profile1CanvasParser parser;
        parser.attr=color;
        return parser.plain(source,true,kMessageBodyBytes,true);
    }catch(const Refusal&){return {};}
}
std::optional<MessageCanvas> message_received_ansi_canvas(std::string_view bytes,bool pipes){
    if(bytes.empty()||bytes.size()>kMessageSourceBytes)return {};
    try{CanvasParser<true> parser;parser.out.foreground_rgb.assign(width*kMessageCanvasRows,no_rgb);parser.out.background_rgb.assign(width*kMessageCanvasRows,no_rgb);auto canvas=parser.ansi(bytes,pipes);if(!canvas.nonblank)return {};return canvas;}
    catch(const Refusal&){return {};}
}
std::optional<MessageCanvas> message_source_rendition(const MessageSource& source,bool pipes,bool colors,std::uint8_t color){
    auto validated=validate_message_source(source,kMaximumMessageLines,color);
    if(!validated)return {};
    if(colors||source.format!=MessageSourceFormat::ansi_canvas)return std::move(pipes?validated.on:validated.off);
    try{Profile1CanvasParser parser;parser.attr=color;parser.sgr_colors=false;return parser.ansi(source.bytes,pipes);}
    catch(const Refusal&){return {};}
}
std::optional<TerminalDisplayProgram> message_canvas_page(const MessageCanvas& c,std::size_t first,std::size_t count,bool ansi){
    if(!c.extent || c.extent>kMessageCanvasRows || c.cells.size()!=width*kMessageCanvasRows || first>=c.extent || !count || count>100)return {};
    const bool rgb=!c.foreground_rgb.empty()||!c.background_rgb.empty();
    if(rgb&&(c.foreground_rgb.size()!=c.cells.size()||c.background_rgb.size()!=c.cells.size()))return {};
    const auto front=[&](std::size_t i){return rgb?c.foreground_rgb[i]:no_rgb;};
    const auto back=[&](std::size_t i){return rgb?c.background_rgb[i]:no_rgb;};
    TerminalDisplayProgram p;
    const auto add=[&](TerminalDisplayOperationKind kind,std::string bytes){p.output_byte_count+=kind==TerminalDisplayOperationKind::logical_newline?2:bytes.size();p.operations.push_back({kind,std::move(bytes)});};
    for(auto row=first;row<std::min<std::size_t>(c.extent,first+count);++row){
        std::size_t end=width;
        while(end && c.cells[row*width+end-1].glyph==' ' && (c.cells[row*width+end-1].attribute&112)==0&&(back(row*width+end-1)==no_rgb||back(row*width+end-1)==0))--end;
        for(std::size_t col=0;col<end;){
            const auto attr=c.cells[row*width+col].attribute;const auto foreground=front(row*width+col),background=back(row*width+col);std::string run;
            do{const auto glyph=c.cells[row*width+col++].glyph;if(glyph<32 || glyph==127)return {};run+=static_cast<char>(glyph);}while(col<end && c.cells[row*width+col].attribute==attr&&front(row*width+col)==foreground&&back(row*width+col)==background);
            if(ansi)add(TerminalDisplayOperationKind::ansi_sequence,rendition(attr));
            for(unsigned channel=0;channel<2;++channel){const auto value=channel?background:foreground;if(value==no_rgb)continue;if(value>0xffffffU)return {};if(ansi)add(TerminalDisplayOperationKind::ansi_sequence,"\x1b["+std::to_string(channel?0:1)+';'+std::to_string((value>>16)&255)+';'+std::to_string((value>>8)&255)+';'+std::to_string(value&255)+'t');}
            add(TerminalDisplayOperationKind::cp437_text,std::move(run));
        }
        if(ansi)add(TerminalDisplayOperationKind::ansi_sequence,"\x1b[0m");
        add(TerminalDisplayOperationKind::logical_newline,{});
    }
    if(!valid_terminal_display_program(p))return {};
    return p;
}
} // namespace sfx::application
