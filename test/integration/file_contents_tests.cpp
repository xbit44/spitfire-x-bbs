#include "sfx/archive/file_contents.hpp"
#include <iostream>
#include <fstream>
#include <algorithm>
namespace arc=sfx::archive;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void write(const std::filesystem::path& file,const std::vector<std::uint8_t>& data){std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));if(!out)throw std::runtime_error("fixture write");}
}
int main(int argc,char** argv)try{
    check(argc==2,"new preserved scratch root required");const auto root=std::filesystem::absolute(argv[1]);check(root.filename()=="file-contents"&&!std::filesystem::exists(root),"bounded new fixture, no cleanup");std::filesystem::create_directories(root/"source"/"Config");
    const std::string hash="2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824";
    arc::ContentIndex index;index.areas.push_back({1,1,"Library","Files/General"});index.files.push_back({1,"hello.txt",5,hash,{},{},"FileContents/Objects/"+hash});
    const auto bytes=arc::encode_content_index(index);const auto decoded=arc::decode_content_index(bytes);check(decoded&&decoded->files.size()==1&&decoded->files[0].name=="hello.txt","roundtrip exact index");
    check(arc::decode_content_index(arc::encode_content_index({})).has_value(),"empty membership preserves Off point");
    auto bad=bytes;bad.pop_back();check(!arc::decode_content_index(bad),"truncated index refused");bad=bytes;bad[0]='x';check(!arc::decode_content_index(bad),"unknown version refused");
    const auto refuses=[&](arc::ContentIndex value){try{(void)arc::encode_content_index(value);return false;}catch(...){return true;}};
    auto copy=index;copy.files.push_back(copy.files[0]);check(refuses(copy),"duplicate membership refused");copy=index;copy.files.push_back(copy.files[0]);copy.files.back().name="HELLO.TXT";check(refuses(copy),"case collision refused");
    copy=index;copy.files[0].name="../escape.txt";check(refuses(copy),"parent path refused");copy=index;copy.files[0].name="name:stream";check(refuses(copy),"ADS refused");
    copy=index;copy.files[0].area=2;check(refuses(copy),"unlisted area refused");copy=index;copy.files[0].bytes=UINT64_MAX;check(refuses(copy),"member size bound");
    copy=index;copy.files[0].member="Config/SFX.conf";check(refuses(copy),"payload namespace cannot target core");copy=index;copy.files[0].archive="../other.zip";copy.files[0].archive_sha256=hash;check(refuses(copy),"dependency sibling only");
    copy=index;copy.files[0].archive="SFX-backup-20261017-010101.zip";copy.files[0].archive_sha256=hash;check(arc::decode_content_index(arc::encode_content_index(copy)).has_value(),"exact dependency record roundtrip");
    write(root/"source"/"Config"/"core.txt",{'c','o','r','e'});write(root/"payload",{'h','e','l','l','o'});write(root/"index",bytes);
    arc::CreateBackupRequest request;request.source_root=root/"source";request.destination_path=root/"point.zip";request.partial_path=root/"point.partial";request.included_root_names={"Config"};request.source_overrides={{arc::file_contents_index,root/"index"},{index.files[0].member,root/"payload"}};
    request.metadata={"manual","AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","1.0.0-alpha.1","1",{"20261017-010101","2026-10-17T01:01:01","-07:00","2026-10-17T08:01:01Z"}};
    check(arc::create_backup(request).ok(),"actual full core plus payload archive");const auto inspection=arc::inspect_backup(request.destination_path);check(inspection.result.ok()&&arc::has_content_index(inspection),"actual manifest and CRC verification");
    check(arc::inspect_content_index(request.destination_path,inspection).files.size()==1,"index binds member size/hash");
    std::vector<std::uint8_t> received;const auto sink=[&](const auto* data,std::size_t n){received.insert(received.end(),data,data+n);return true;};
    check(arc::stream_zip_member(request.destination_path,index.files[0].member,5,sink).ok()&&received==std::vector<std::uint8_t>({'h','e','l','l','o'}),"bounded streamed payload");
    check(!arc::stream_zip_member(request.destination_path,index.files[0].member,4,sink).ok(),"wrong size before sink refused");
    check(!arc::stream_zip_member(request.destination_path,"absent",5,sink).ok(),"absent member refused");
    check(arc::stream_zip_member(request.destination_path,index.files[0].member,5,sink,[]{return true;}).error==arc::Error::cancelled,"stream cancellation");
    check(!arc::stream_zip_member(request.destination_path,index.files[0].member,5,[](const auto*,std::size_t){return false;}).ok(),"staging sink refusal");
    auto broken=inspection;for(auto& file:broken.files)if(file.path==index.files[0].member)++file.size;bool rejected{};try{(void)arc::inspect_content_index(request.destination_path,broken);}catch(...){rejected=true;}check(rejected,"index cannot contradict verified member metadata");
    std::cout<<"PASS file-content format/stream checks="<<checks<<"; native chains, retention and installed restore separate\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL file-content format after "<<checks<<": "<<e.what()<<'\n';return 1;}
