#include "sfx/application/botgate.hpp"
#include "sfx/application/botgate_rules.hpp"
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <array>

using namespace sfx::application;
int main() {
    unsigned checks=0;
    const auto require=[&](bool ok) { ++checks; if (!ok) { std::cerr<<"FAILED BotGate check "<<checks<<'\n'; std::exit(1); } };
    require(valid_botgate_configuration({}));
    for(bool global:{false,true}) for(bool challenge:{false,true}) for(bool telnet:{false,true}) {
        BotGateConfiguration config; config.enabled=global; config.ssh_enabled=challenge;
        require(botgate_filters_enabled(config,telnet,true)==global);
        require(botgate_filters_enabled(config,telnet,false)==(global && telnet));
        for(unsigned index:{1U,2U,3U,4U}) {
            BotGateRuleSnapshot rules; rules.ready=true;
            rules.lists[index]=*parse_botgate_patterns(index==3 ? "blocked.example" :
                index==4 ? "192.0.2.1\te=20990101T000000+0000" : "192.0.2.1",index==3,index==4);
            require((botgate_filters_enabled(config,telnet,true) &&
                !rules.refusal("192.0.2.1","blocked.example",1).empty())==global);
            if(index==3) require(rules.refusal("192.0.2.1","",1).empty());
        }
        BotGateRuleSnapshot geo; geo.ready=true;
        const auto country=parse_botgate_geo("fixture.txt","deny from 192.0.2.0/24\n");
        require(country.has_value());
        geo.geo.push_back(*country);
        require((botgate_filters_enabled(config,telnet,true) && !geo.refusal("192.0.2.1","",1).empty())==global);
    }
    require(valid_logging_configuration({}));
    BotGateFeedStatus progress;
    require(render_botgate_feed_progress(progress).empty()); // cached feed is not a new attempt
    progress.last_attempt=100;progress.updating=true;progress.result="feed_downloading";
    auto display=render_botgate_feed_progress(progress);
    require(display.find("update started\r\nReceived 0 bytes")!=display.npos);
    require(display.find('#')==display.npos && display.find("update complete")==display.npos);
    for (const std::uint64_t bytes:std::array<std::uint64_t,8>{1,32768,32769,2359296,2359297,8388608,16777216,UINT64_MAX}) {
        progress.bytes_received=bytes;display=render_botgate_feed_progress(progress);
        const auto bounded=std::min<std::uint64_t>(bytes,kBotGateFeedMaximumBytes);
        require(std::count(display.begin(),display.end(),'#')==static_cast<std::ptrdiff_t>((bounded+32767)/32768));
        require(display.find(std::string(73,'#'))==display.npos && display.size()<1024);
        require(display.find("update complete")==display.npos);
    }
    progress.bytes_received=8388608;progress.result="feed_validating";
    display=render_botgate_feed_progress(progress);
    require(display.find("Validating and activating")!=display.npos && display.find("update complete")==display.npos);
    progress.result="feed_update_success";progress.available=true;
    require(render_botgate_feed_progress(progress).find("update complete")==std::string::npos); // still active
    progress.updating=false;display=render_botgate_feed_progress(progress);
    require(display.find("update complete\r\n")!=display.npos && display.find("update failed")==display.npos);
    for (const auto* failure:{"feed_candidate_invalid","feed_cache_publication_failed","feed_fixture_network_failure","feed_worker_failed"}) {
        progress.result=failure;display=render_botgate_feed_progress(progress);
        require(display.find("update failed: "+std::string(failure))!=display.npos);
        require(display.find("last good feed retained")!=display.npos && display.find("update complete")==display.npos);
    }
    progress.available=false;
    require(render_botgate_feed_progress(progress).find("no valid cached feed")!=std::string::npos);
    progress.result="bad\x1b\r\n";display=render_botgate_feed_progress(progress);
    require(display.find("bad???")!=display.npos && display.find('\x1b')==display.npos);
    progress.last_attempt=101;progress.updating=true;progress.bytes_received=0;progress.result="feed_downloading";
    display=render_botgate_feed_progress(progress);
    require(display.find('#')==display.npos && display.find("update failed")==display.npos); // next attempt clears progress
    require(botgate_ansi_terminal("ANSI") && botgate_ansi_terminal("xterm-256color"));
    require(!botgate_ansi_terminal("dumb") && !botgate_ansi_terminal("xterm\x1b"));
    const std::string artwork="\x1b[31mEmail a@b.example, ## seconds\x1b[0m\r\n\r\n";
    const auto art=render_botgate_art(artwork,19,true);
    require(art.find("a@b.example, 19 seconds")!=art.npos && artwork.find("##")!=artwork.npos);
    require(art.find("\r\n")==art.npos);
    require(render_botgate_art(artwork,9,true).size()==render_botgate_art(artwork,10,true).size());
    require(render_botgate_art(artwork,9,true).find(" 9 seconds")!=std::string::npos);
    require(render_botgate_art(artwork,19,false).find("\x1b")==std::string::npos);
    for (const auto bad : {"\x1b]52;c;secret\a", "\x1b[6n", "\x1b[?25l", "\x1bPquery", "\x01"})
        require(render_botgate_art(bad,20,true).find("Press ESC")!=std::string::npos);
    for (const auto invalid : {"", "01.2.3.4", "1.2.3", "256.0.0.1", "1.2.3.4x", "-1.2.3.4"})
        require(!parse_botgate_ipv4(invalid));
    require(parse_botgate_ipv4("255.255.255.255")==UINT32_MAX);
    require(parse_botgate_network("0.0.0.0/0").has_value());
    require(!parse_botgate_network("192.168.1.1/24"));
    require(!parse_botgate_network("1.2.3.4/33"));
    const auto ip=*parse_botgate_ipv4("192.0.2.1");
    BotGateConfiguration policy;
    policy.manual_blocks="192.0.2.0/24"; policy.trusted_sources="192.0.2.1"; policy.feed_exceptions="192.0.2.1";
    require(evaluate_botgate(policy,true,ip,{}, {ip})==BotGateDisposition::manual_block);
    policy.manual_blocks="-"; policy.domains="bad.example";
    require(evaluate_botgate(policy,true,ip,"x.bad.example",{ip})==BotGateDisposition::domain_block);
    require(!botgate_domain_matches(policy.domains,"notbad.example"));
    require(evaluate_botgate(policy,true,ip,{}, {ip})==BotGateDisposition::trusted);
    policy.feed_exceptions="-";
    require(evaluate_botgate(policy,true,ip,{}, {ip})==BotGateDisposition::feed_block);
    policy.enabled=false;
    require(evaluate_botgate(policy,true,ip,{}, {ip})==BotGateDisposition::off);
    policy.enabled=true;
    require(evaluate_botgate(policy,false,ip,{}, {ip})==BotGateDisposition::off);
    auto feed=parse_botgate_feed("# test\n192.0.2.1 # comment\n192.0.2.1\n203.0.113.4\n");
    require(feed && feed->size()==2);
    auto replacement=parse_botgate_feed("203.0.113.4\n");
    require(replacement && replacement->size()==1 && replacement->front()!=ip);
    for (const auto bad : {"", "# empty\n", "1.2.3.4", "1.2.3.4\nnot-an-ip\n", "1.2.3.0/24\n", "<html>failure</html>\n"})
        require(!parse_botgate_feed(bad));
    using R=BotGateChallenge::Result;
    for (const auto sequence : {"**", "\x1b*", "*\x1b", "\x1b\x1b"}) {
        BotGateChallenge c(1000,20);
        require(c.remaining(1000)==20 && c.remaining(2000)==19);
        require(c.input(std::string_view(sequence,1),1100)==R::waiting);
        require(c.input(std::string_view(sequence+1,1),1200)==R::waiting);
        require(c.poll(1299)==R::waiting && c.poll(1300)==R::passed);
    }
    for (const auto bad : {"GET /* HTTP/1.1", "**GET", "hello**"}) {
        BotGateChallenge c(0,20); require(c.input(bad,10)==R::invalid);
    }
    BotGateChallenge scanner(0,20);
    require(scanner.input("**",10)==R::waiting);
    require(scanner.input("GET",50)==R::invalid);
    BotGateChallenge timeout(0,20);
    require(timeout.poll(19999)==R::waiting && timeout.input("**",20000)==R::timed_out);
    BotGateChallenge tail(0,20);
    require(tail.input("**\r\nlogin",10)==R::invalid);
    BotGateChallenge coalesced(0,20);
    require(coalesced.input("**",10)==R::waiting && coalesced.poll(110)==R::passed);
    BotGateChallenge extra(0,20);
    require(extra.input("***",10)==R::waiting && extra.poll(110)==R::passed);
    BotGateChallenge typo(0,20);
    require(typo.input("x",10)==R::waiting && typo.input("**",200)==R::waiting && typo.poll(300)==R::passed);
    BotGateChallenge arrow(0,20);
    require(arrow.input("\x1b",10)==R::waiting && arrow.input("[A",20)==R::waiting && arrow.poll(200)==R::waiting);
    require(arrow.input("**",300)==R::waiting && arrow.poll(400)==R::passed);
    BotGateChallenge split_scanner(0,20);
    require(split_scanner.input("GET /",10)==R::waiting && split_scanner.input("* HTTP/",20)==R::invalid);
    require(render_botgate_art(artwork,120,true,3).find("120 seconds")!=std::string::npos);
    require(render_botgate_art(artwork,9,true,3).size()==render_botgate_art(artwork,120,true,3).size());
    const auto rules=parse_botgate_patterns("# note\r\n192.0.2.*\n!192.0.2.3\n203.0.113.0/24\n");
    require(rules && botgate_patterns_match(*rules,"192.0.2.2",0));
    require(!botgate_patterns_match(*rules,"192.0.2.3",0));
    require(botgate_patterns_match(*rules,"203.0.113.255",0));
    require(!botgate_patterns_match(*rules,"203.0.112.255",0));
    auto hosts=parse_botgate_patterns("*.bad.example\n!safe.bad.example",true);
    require(hosts && botgate_patterns_match(*hosts,"EVIL.BAD.EXAMPLE",0));
    require(!botgate_patterns_match(*hosts,"safe.bad.example",0));
    for(const auto* bad:{"garbage","192.0.2.256","192.0.2.1/24","!","1.2.3.4/33"}) require(!parse_botgate_patterns(bad));
    require(!parse_botgate_patterns(std::string(1024*1024+1,'x')));
    require(!parse_botgate_patterns(std::string("1.2.3.4\0",8)));
    auto temporary=parse_botgate_patterns("192.0.2.1\tt=19700101T000000+0000\te=19700101T000010+0000\n",false,true);
    require(temporary && botgate_patterns_match(*temporary,"192.0.2.1",9));
    require(!botgate_patterns_match(*temporary,"192.0.2.1",10));
    require(!parse_botgate_patterns("1.2.3.4\te=20260230T000000+0000",false,true));
    require(!parse_botgate_patterns("1.2.3.4",false,true));
    const auto geo=parse_botgate_geo("country.txt","<Limit GET POST>\norder allow,deny\nallow from all\ndeny from 192.0.2.0/24\ndeny from 192.0.2.128/25\ndeny from 203.0.113.7\n</Limit>");
    require(geo && geo->ranges.size()==2 && geo->contains(*parse_botgate_ipv4("192.0.2.254")));
    require(!geo->contains(*parse_botgate_ipv4("192.0.3.0")));
    require(geo->contains(*parse_botgate_ipv4("203.0.113.7")));
    require(!parse_botgate_geo("bad.txt","deny from 192.0.2.1\ninvalid\n"));
    BotGateRuleSnapshot snapshot;snapshot.ready=true;snapshot.geo.push_back(*geo);snapshot.lists[1]=*rules;
    require(snapshot.refusal("192.0.2.3","",0)=="geo/country.txt");
    snapshot.lists[0]=*parse_botgate_patterns("192.0.2.3");
    require(snapshot.exempt("192.0.2.3",0) && snapshot.refusal("192.0.2.3","",0).empty());
    BotGateArrivalLimits arrivals;
    for(unsigned i=0;i<20;++i) require(arrivals.admit(1,100));
    require(!arrivals.admit(1,100) && arrivals.banned_until(1)==5500);
    require(arrivals.admit(2,100));
    require(!arrivals.admit(1,5499) && arrivals.admit(1,5500));
    BotGateArrivalLimits window;
    for(unsigned i=0;i<20;++i) require(window.admit(1,0));
    require(window.admit(1,10));
    BotGateArrivalLimits capacity;
    for(unsigned i=0;i<4096;++i) require(capacity.admit(i,0));
    require(!capacity.admit(4096,0) && capacity.admit(4096,10));
    std::cout << "PASS BotGate checks=" << checks << '\n';
}
