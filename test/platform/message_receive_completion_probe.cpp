// Diagnostic only: production receiver + immutable helper, independent sender.
// Never executes the supplied executable. Reads both owner files as inert bytes.
#define wmain historical_host_fault_entrypoint
#include "message_receive_host_tests.cpp"
#undef wmain
#include <fstream>
#include <iterator>

int wmain(int argc,wchar_t** argv)try{
    check(argc==3,"exact disposable probe root and retained input directory");
    const auto root=inst::canonical_root(argv[1]);
    check(root==inst::Path(L"C:\\xtest-sfx-section8b-empty-batch-fault-r1-20261009"),"new fixed empty-batch qualification fixture only; old diagnostic fixture preserved");
    check(!inst::elevated(),"ordinary owner diagnostic");inst::verify_runtime_installation(root,inst::operator_sid());
    const inst::Path inputs(argv[2]);
    WSADATA ws{};check(WSAStartup(MAKEWORD(2,2),&ws)==0,"Winsock initialized");
    struct End{~End(){WSACleanup();}} end;
    for(const auto* filename:{L"TEST60K.TXT",L"reglotf.exe"}){
        const auto file=inputs/filename;
        const bool text=std::wstring(filename)==L"TEST60K.TXT";
        check(inst::file_sha256(file)==(text?"6563AE59A38B2D28C505946B16C801A72D5C45931C50E9EC678448ED057D24C7":"6681298A5AC4AD5FEC7C4A12648FBEA24EF70EB05D833CF3B6AE6B53361961E9"),"exact owner input hash");
        std::ifstream stream(file,std::ios::binary);const std::string body((std::istreambuf_iterator<char>(stream)),{});
        check(body.size()==(text?61440U:55402U),"exact inert input length");
        for(const auto tail:{std::string("\r"),std::string("\n"),std::string("\r\n")})
        for(int repeat=0;repeat<3;++repeat)for(int delay:{-2,-1,0,1,2,5,10,100}){
            Channel c;c.wire=transfer(body);const auto finish_size=c.wire.size();bool key=false;std::uint64_t at{};
            if(delay==-1){c.wire.insert(c.wire.end(),tail.begin(),tail.end());key=true;}
            if(delay>=0)c.after_read=[&]{
                if(c.offset<finish_size)return;
                if(!at)at=GetTickCount64();
                if(!key && GetTickCount64()-at>=static_cast<unsigned>(delay)){c.wire.insert(c.wire.end(),tail.begin(),tail.end());key=true;}
            };
            sfx::apps::runtime::AlphaMessageSourceReceiver receiver(root,inst::operator_sid(),c.ports(),
                [&](std::string_view phase,std::uint32_t error,std::uint32_t exit,std::size_t pending){
                    std::cout<<"OBS "<<(text?"text":"exe")<<" repeat="<<repeat<<" delay="<<delay
                        <<" tail_bytes="<<tail.size()<<" phase="<<phase<<" error="<<error<<" helper_exit="<<exit<<" pending="<<pending<<std::endl;
                });
            check(receiver.available(request()),"exact protected helper preflight");++cases;
            const auto result=receiver.receive(request());
            check(result.code==(text?app::MessageReceiveCode::completed:app::MessageReceiveCode::refused) && c.synchronized,"all early/coalesced/delayed completion Enters synchronize; executable content never posts");
            std::cout<<"PROBE "<<(text?"text":"exe")<<" repeat="<<repeat<<" delay="<<delay
                <<" code="<<static_cast<int>(result.code)<<" synchronized="<<c.synchronized
                <<" tail_bytes="<<tail.size()<<" key_read="<<(c.offset>finish_size)<<" unread="<<(c.wire.size()-c.offset)<<" reads="<<c.reads<<std::endl;
        }
    }
    std::cout<<"PROBE COMPLETE cases="<<cases<<" checks="<<checks<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"PROBE FAILED "<<e.what()<<std::endl;return 1;}
