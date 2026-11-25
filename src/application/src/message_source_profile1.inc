// Immutable stored profile1 parser, preserved from the R15 source.
struct Profile1CanvasParser {
    MessageCanvas out;
    std::size_t x{},y{},sx{},sy{};
    std::uint8_t attr{7};
    bool sgr_colors{true};
    bool wrap{true},pending{},saved{},saved_pending{};
    Profile1CanvasParser(){out.cells.resize(width*kMessageCanvasRows);out.extent=1;}
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
        if(x+1<width)++x;else pending=true;
    }
    void sgr(unsigned n){
        const auto prior=attr;
        if(n==0)attr=7;
        else if(n==1)attr|=8;
        else if(n==22)attr&=247;
        else if(n==5)attr|=128;
        else if(n==25)attr&=127;
        else if(n>=30 && n<=37)attr=(attr&248)|ansi_to_vga[n-30];
        else if(n==39)attr=(attr&248)|7;
        else if(n>=40 && n<=47)attr=(attr&143)|(ansi_to_vga[n-40]<<4);
        else if(n==49)attr&=143;
        else refuse(MessageSourceValidation::control);
        if(!sgr_colors)attr=prior;
    }
    bool pipe(std::string_view source,std::size_t& i,bool enabled,std::uint8_t b,unsigned maximum=31){
        if(!enabled || b!='|' || i+1>=source.size() || source[i]<'0' || source[i]>'9' || source[i+1]<'0' || source[i+1]>'9')return false;
        const unsigned n=(source[i]-'0')*10+source[i+1]-'0';if(n>maximum)return false;
        i+=2;attr=static_cast<std::uint8_t>(n<16?(attr&240)|n:(attr&15)|((n-16)<<4));return true;
    }
    MessageCanvas finish(){
        const bool plain_text=!out.text.rows.empty();
        for(std::size_t row=0;row<out.extent;++row){
            std::string text;
            for(std::size_t col=0;col<width;++col){
                const auto c=out.cells[row*width+col];text+=static_cast<char>(c.glyph);
                if(c.glyph!=' ' || (c.attribute&112)!=0)out.nonblank=true;
            }
            while(!text.empty() && text.back()==' ' && (out.cells[row*width+text.size()-1].attribute&112)==0)text.pop_back();
            if(!plain_text){out.text.rows.push_back(text);out.text.canonical_lf+=text;out.text.canonical_lf+='\n';}
        }
        return std::move(out);
    }
    MessageCanvas ansi(std::string_view src,bool pipes){
        for(std::size_t i=0;i<src.size();){
            if(++out.operations>kMessageSourceBytes)refuse(MessageSourceValidation::budget);
            const auto b=static_cast<std::uint8_t>(src[i++]);
            if(b==27){
                if(i==src.size() || src[i++]!='[')refuse(MessageSourceValidation::control);
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
                    if(p!=std::vector<unsigned>{7} || (final!='h' && final!='l'))refuse(MessageSourceValidation::control);
                    wrap=final=='h';pending=false;
                }else if(final=='m'){if(p.empty())sgr(0);for(auto v:p)sgr(v);}
                else if(final=='C'){x+=one();pending=false;touch();}
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
                    if(p!=std::vector<unsigned>{2})refuse(MessageSourceValidation::control);
                    charge(width*kMessageCanvasRows);std::fill(out.cells.begin(),out.cells.end(),MessageCanvasCell{32,attr});
                }else refuse(MessageSourceValidation::control);
            }else if(b=='\r'){x=0;pending=false;}
            else if(b=='\n'){++y;pending=false;touch();}
            else if(b=='\t'){
                // Fixed eight-cell tab, including delayed wrap at column 80.
                const auto spaces=8-x%8;for(std::size_t s=0;s<spaces;++s)put(32);
            }else if(b<32 || b==127)refuse(MessageSourceValidation::control);
            else if(!pipe(src,i,pipes,b))put(b);
        }
        return finish();
    }
    MessageCanvas plain(std::string_view src,bool pipes,std::size_t operation_limit=kMessageSourceBytes,
        bool reset_pipe_at_newline=false){
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
        const auto base=attr;
        const auto maximum_pipe=reset_pipe_at_newline?23U:31U;
        for(std::size_t i=0;i<src.size();){
            if(++out.operations>operation_limit)refuse(MessageSourceValidation::budget);
            const auto b=static_cast<std::uint8_t>(src[i++]);
            if(b=='\r' || b=='\n'){
                if(b=='\r' && i<src.size() && src[i]=='\n')++i;
                flush();ended=true;
                if(reset_pipe_at_newline)attr=base;
            }else{
                if((b<32 && b!='\t') || b==127)refuse(MessageSourceValidation::control);
                if(pipe(src,i,pipes,b,maximum_pipe))continue;
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
