#include "sfx/platform/network.hpp"
#include <algorithm>
namespace { int send_limit{},send_calls_left{-1}; }
namespace sfx::platform::network {
// Deterministic short/would-block writes still cross the real TCP socket.
int controlled_send_bytes(Socket socket,const char* data,int size){
    if(send_calls_left==0){
#ifdef _WIN32
        WSASetLastError(WSAEWOULDBLOCK);
#else
        errno=EWOULDBLOCK;
#endif
        return socket_error;
    }
    if(send_calls_left>0)--send_calls_left;
    return send_bytes(socket,data,send_limit?std::min(size,send_limit):size);
}
}
#define send_bytes controlled_send_bytes
#include "alpha_telnet_stream.hpp"
#undef send_bytes
#include <iostream>
#include <stdexcept>

namespace net=sfx::platform::network;
namespace app=sfx::application;
namespace runtime=sfx::apps::runtime;
namespace {
unsigned checks{};
void check(bool okay,const char* why){++checks;if(!okay)throw std::runtime_error(why);}
struct Socket {
    net::Socket value{net::invalid_socket};
    ~Socket(){if(value!=net::invalid_socket){
#ifdef _WIN32
        closesocket(value);
#else
        close(value);
#endif
    }}
};
void send_all(net::Socket socket,std::string_view text){while(!text.empty()){
    const auto n=net::send_bytes(socket,text.data(),static_cast<int>(text.size()));check(n>0,"peer write succeeds");text.remove_prefix(static_cast<std::size_t>(n));}}
std::string receive(net::Socket socket,std::size_t bytes){std::string text(bytes,'\0');std::size_t offset{};
    while(offset<bytes){const auto n=net::receive_bytes(socket,text.data()+offset,static_cast<int>(bytes-offset));check(n>0,"peer receives expected bytes");offset+=static_cast<std::size_t>(n);}return text;}
std::string wire(std::string_view bytes){std::string result;for(const unsigned char c:bytes){result+=static_cast<char>(c);if(c==255)result+=static_cast<char>(c);}return result;}
std::string option(unsigned command){return std::string{static_cast<char>(255),static_cast<char>(command),0};}
}
int main(){try{
#ifdef _WIN32
    WSADATA startup{};check(WSAStartup(MAKEWORD(2,2),&startup)==0,"Winsock initialized");
    struct Winsock {~Winsock(){WSACleanup();}} winsock;
#endif
    Socket listening,peer,server;listening.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(listening.value!=net::invalid_socket,"create TCP socket");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(bind(listening.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(listening.value,1)==0,"isolated ephemeral loopback listener");
#ifdef _WIN32
    int size=sizeof(address);
#else
    socklen_t size=sizeof(address);
#endif
    check(getsockname(listening.value,reinterpret_cast<sockaddr*>(&address),&size)==0,"read actual ephemeral port");
    peer.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(peer.value!=net::invalid_socket&&connect(peer.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"peer connects");
    server.value=accept(listening.value,nullptr,nullptr);check(server.value!=net::invalid_socket,"accept real caller socket");
    check(net::set_timeout(peer.value,SO_RCVTIMEO,2000)&&net::set_timeout(server.value,SO_RCVTIMEO,80)&&net::set_timeout(server.value,SO_SNDTIMEO,500),"finite real socket timeouts");
    std::atomic<bool> stopping{};std::string observed;unsigned inbound{},outbound{};
    runtime::AlphaTelnetStream stream(server.value,stopping,[&](std::string_view text){observed+=text;},[&](bool input){if(input)++inbound;else ++outbound;});
    stream.enable_menu_polling();const auto start=net::monotonic_millis();
    check(stream.read_ready(64).outcome==app::InteractiveStreamOutcome::timed_out&&net::monotonic_millis()-start<50,"empty readiness probe does not block helper output");
    check(stream.read_some(0).outcome==app::InteractiveStreamOutcome::failed,"zero read refused");
    check(stream.write_some("").outcome==app::InteractiveStreamOutcome::failed,"empty write refused");
    const std::string greeting="Hello\r\n"+std::string(1,static_cast<char>(255));
    const auto written=stream.write_some(greeting);check(written.outcome==app::InteractiveStreamOutcome::completed&&written.bytes_written==greeting.size(),"text write accounts logical bytes");
    check(receive(peer.value,wire(greeting).size())==wire(greeting)&&observed==greeting&&outbound>0,"IAC doubled only on wire and observer sees original output");
    send_all(peer.value,std::string{static_cast<char>(255),static_cast<char>(251),24});
    check(stream.read_some(64).outcome==app::InteractiveStreamOutcome::timed_out&&inbound==0,"terminal negotiation is not keyboard activity");
    check(receive(peer.value,6)==std::string{static_cast<char>(255),static_cast<char>(250),24,1,static_cast<char>(255),static_cast<char>(240)},"terminal type request exact");
    send_all(peer.value,std::string{static_cast<char>(255),static_cast<char>(250),24,0}+"ANSI"+std::string{static_cast<char>(255),static_cast<char>(240)});
    check(stream.read_some(64).outcome==app::InteractiveStreamOutcome::timed_out&&stream.ansi_terminal(),"ANSI terminal negotiated through actual stream");
    const std::string input="A"+std::string(1,static_cast<char>(255))+"\r\n";send_all(peer.value,wire(input));
    const auto read=stream.read_some(64);check(read.outcome==app::InteractiveStreamOutcome::completed&&std::string(read.bytes.begin(),read.bytes.end())==input&&inbound==1,"input IAC unquoting and keyboard activity");
    const auto activity_before=inbound;
    send_all(peer.value,option(251).substr(0,2));
    check(stream.read_some(64).outcome==app::InteractiveStreamOutcome::timed_out&&net::readable_now(peer.value)==0,"partial option waits for its number");
    send_all(peer.value,std::string(1,'\0')+option(253));
    check(stream.read_some(64).outcome==app::InteractiveStreamOutcome::timed_out&&inbound==activity_before,"BINARY negotiation is not keyboard input");
    check(receive(peer.value,6)==option(253)+option(251),"WILL BINARY and DO BINARY receive exact acknowledgments");
    send_all(peer.value,option(251)+option(253));
    check(stream.read_some(64).outcome==app::InteractiveStreamOutcome::timed_out&&net::readable_now(peer.value)==0,"repeated options do not create negotiation loops");
    check(stream.begin_gateway()&&stream.end_gateway(),"negotiated BINARY does not acquire transfer lease or block Doors");
    send_all(peer.value,option(252)+option(254));
    check(stream.read_some(64).outcome==app::InteractiveStreamOutcome::timed_out&&receive(peer.value,6)==option(254)+option(252),"WONT and DONT disable the corresponding directions");
    send_all(peer.value,option(252)+option(254));
    check(stream.read_some(64).outcome==app::InteractiveStreamOutcome::timed_out&&net::readable_now(peer.value)==0,"repeated negative acknowledgments are silent");
    check(stream.message_binary_mode(true)&&!stream.begin_gateway(),"binary transfer owns transport exclusively");
    std::string binary;for(unsigned n=0;n<256;++n)binary+=static_cast<char>(n);
    const auto sent=stream.write_some(binary);check(sent.outcome==app::InteractiveStreamOutcome::completed&&sent.bytes_written==binary.size(),"all byte values transfer with logical count");
    check(receive(peer.value,wire(binary).size())==wire(binary),"binary output preserved including NUL and IAC");
    send_all(peer.value,wire(binary));const auto binary_read=stream.read_some(4096);
    check(binary_read.outcome==app::InteractiveStreamOutcome::completed&&std::string(binary_read.bytes.begin(),binary_read.bytes.end())==binary,"binary input preserved including NUL and IAC");
    const std::string literal_iac(1,static_cast<char>(255));
    send_limit=1;
    check(stream.write_some(literal_iac).bytes_written==0,"first half of literal IAC is not reported as a logical byte");
    send_all(peer.value,option(251));
    check(stream.read_some(64).outcome==app::InteractiveStreamOutcome::timed_out,"late BINARY request arrives during split IAC");
    check(stream.write_some(literal_iac+"Z").bytes_written==1,"literal IAC completes before queued negotiation");
    check(receive(peer.value,2)==wire(literal_iac),"no control bytes inserted inside quoted literal IAC");
    send_calls_left=1;
    check(stream.write_some("Z").outcome==app::InteractiveStreamOutcome::timed_out,"partial option reply survives would-block write");
    send_calls_left=-1;
    check(stream.write_some("Z").bytes_written==1&&receive(peer.value,4)==option(253)+"Z","retry preserves control bytes exactly once before payload");
    send_limit=0;
    check(stream.message_binary_mode(false)&&stream.begin_gateway()&&!stream.begin_gateway(),"gateway ownership enters once");
    check(stream.read_ready(64).outcome==app::InteractiveStreamOutcome::timed_out,"gateway readiness remains nonblocking");
    check(stream.write_ready("door\r\n").bytes_written==6&&receive(peer.value,6)=="door\r\n","Door output crosses shared stream");
    check(stream.end_gateway()&&!stream.end_gateway(),"gateway restores blocking mode once");
    stopping=true;check(stream.read_ready(64).outcome==app::InteractiveStreamOutcome::shutdown_requested,"shutdown boundary is prompt");
    stopping=false;shutdown(peer.value,net::shutdown_send);
    check(stream.read_some(64).outcome==app::InteractiveStreamOutcome::disconnected,"EOF distinguished from timeout");
    std::cout<<"PASS "<<checks<<" shared Telnet stream checks on actual TCP sockets\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
