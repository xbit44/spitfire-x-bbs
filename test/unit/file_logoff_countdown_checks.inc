    for(const bool ansi:{false,true}){
        const auto trial=[&](std::vector<int> input,std::optional<bool> expected,bool connection=true,bool output=true){
            Harness h;h.dimensions.ansi=ansi;auto p=h.ports();
            using Clock=std::chrono::steady_clock;auto time=Clock::time_point{};
            std::size_t at{};p.byte=[&](){
                const int c=at<input.size()?input[at++]:-1;
                time+=std::chrono::milliseconds(c<0?100:1);
                if(c==-2)return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::disconnected,{}};
                if(c==-3){h.live=false;return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::cancelled,{}};}
                return c<0?app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::timed_out,{}}:
                    app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::completed,static_cast<unsigned char>(c)};
            };
            h.live=connection;h.fail_write=!output;
            const auto result=app::file_transfer_logoff_countdown(p,[&]{return time;});
            check(result==expected,"countdown returns only explicit action, expiry, or actual failure");
            if(expected&&h.frames.find("Waiting for transfer input")==h.frames.npos){
                check(h.frames.find("<enter>")==h.frames.npos&&h.frames.find(app::file_option('A',"Abort logoff",ansi))!=h.frames.npos,"countdown offers hotkeys without a final Enter instruction");
                if(!ansi)check(h.frames.find('\x1b')==h.frames.npos,"plain countdown has no ANSI");
                if(*expected&&time>=Clock::time_point{}+std::chrono::seconds(10)){
                    check(time<Clock::time_point{}+std::chrono::milliseconds(11200),"idle countdown expires at ten seconds with no input");
                    for(unsigned n=1;n<=10;++n)check(h.frames.find("Logging off in "+std::to_string(n)+" second(s).")!=h.frames.npos,"each countdown second is visible");
                }else check(time<Clock::time_point{}+std::chrono::seconds(1),"fresh Abort/Disconnect responds on first command poll");
            }
            return h.frames;
        };
        trial({},true);
        trial({-1,'A',-1},false); // first key immediately after visible prompt
        trial({-1,'d',-1},true);
        trial(std::vector<int>(12000,'A'),true); // bounded continuous noise
        trial({'A','D','\r','\n',-1},true); // stale keys/Enter before first quiet poll
        trial({-1,'\r','\n','*','*',24,'B','D','A','D','\r',-1,27,'[','D',-1},true);
        trial({-1,'a',-1},false);
        trial({-1,'D','\r'},true);
        trial({-1,'A','\r','\n'},false);
        trial({-1,-2},std::nullopt);
        trial({-1,-3},std::nullopt);
        trial({},std::nullopt,false);
        trial({},std::nullopt,true,false);
    }
