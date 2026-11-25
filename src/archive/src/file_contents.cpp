#include "sfx/archive/file_contents.hpp"
#include "sfx/application/file.hpp"
#include <algorithm>
#include <charconv>
#include <set>
#include <sstream>
#include <stdexcept>

namespace sfx::archive {
namespace {
constexpr std::size_t bound=32U*1024U*1024U;
bool digest(const std::string& s){return s.size()==64&&s.find_first_not_of("0123456789ABCDEF")==std::string::npos;}
std::string hex(const std::string& s){constexpr char digits[]="0123456789ABCDEF";std::string out;for(const auto c:s){const auto b=static_cast<unsigned char>(c);out+=digits[b>>4];out+=digits[b&15];}return out;}
std::optional<std::string> unhex(const std::string& s){
    if(s.empty()||s.size()>8192||s.size()%2)return {};
    std::string out;
    const auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:-1;};
    for(std::size_t i=0;i<s.size();i+=2){const auto a=digit(s[i]),b=digit(s[i+1]);if(a<0||b<0||(!a&&!b))return {};out+=static_cast<char>((a<<4)|b);}return out;
}
template<class T> bool number(const std::string& s,T& n){if(s.empty()||(s.size()>1&&s[0]=='0'))return false;const auto r=std::from_chars(s.data(),s.data()+s.size(),n);return r.ec==std::errc{}&&r.ptr==s.data()+s.size();}
std::vector<std::string> fields(const std::string& s){std::vector<std::string> out;std::size_t start{};for(;;){const auto end=s.find('\t',start);out.push_back(s.substr(start,end==std::string::npos?end:end-start));if(end==std::string::npos)break;start=end+1;}return out;}
bool safe_text(const std::string& s){return !s.empty()&&s.size()<=2048&&application::valid_file_utf8(s)&&std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32||c==127;});}
}
std::optional<ContentIndex> decode_content_index(const std::vector<std::uint8_t>& bytes){
    if(bytes.empty()||bytes.size()>bound||bytes.back()!='\n')return {};
    std::istringstream stream(std::string(bytes.begin(),bytes.end()));std::string line;
    if(!std::getline(stream,line)||line!="SFX-FILE-CONTENTS-1")return {};
    ContentIndex result;std::set<std::int64_t> areas;std::set<std::pair<std::int64_t,std::string>> names;bool files_started{};
    while(std::getline(stream,line)){
        if(line.size()>20000)return {};
        const auto f=fields(line);
        if(f.size()==5&&f[0]=="AREA"&&!files_started){
            ContentArea a;auto name=unhex(f[3]),path=unhex(f[4]);
            if(result.areas.size()>=10000||!number(f[1],a.id)||a.id<=0||!number(f[2],a.revision)||a.revision<=0||!name||!path||!safe_text(*name)||!safe_text(*path)||!areas.insert(a.id).second)return {};
            a.name=*name;a.configured_path=*path;result.areas.push_back(std::move(a));
        }else if(f.size()==8&&f[0]=="FILE"){
            files_started=true;ContentFile entry;auto name=unhex(f[2]);
            if(result.files.size()>=100000||!number(f[1],entry.area)||!areas.count(entry.area)||!name||!application::valid_file_name(*name)||!number(f[3],entry.bytes)||entry.bytes>64ULL*1024*1024*1024||!digest(f[4]))return {};
            entry.name=*name;entry.sha256=f[4];entry.member=f[7];
            if(entry.member!="FileContents/Objects/"+entry.sha256)return {};
            // Case folding matches the application's filename identity. Reject
            // ambiguous duplicate Windows names before any payload publication.
            const auto key=application::file_case_key(entry.name);
            if(!key||!names.insert({entry.area,*key}).second)return {};
            if(f[5]!="-"||f[6]!="-"){
                const auto archive_name=unhex(f[5]);if(!archive_name||!application::valid_file_name(*archive_name)||archive_name->size()<5||archive_name->substr(archive_name->size()-4)!=".zip"||!digest(f[6]))return {};
                entry.archive=*archive_name;entry.archive_sha256=f[6];
            }
            result.files.push_back(std::move(entry));
        }else return {};
    }
    return result;
}
std::vector<std::uint8_t> encode_content_index(const ContentIndex& index){
    std::string text="SFX-FILE-CONTENTS-1\n";
    for(const auto& a:index.areas)text+="AREA\t"+std::to_string(a.id)+'\t'+std::to_string(a.revision)+'\t'+hex(a.name)+'\t'+hex(a.configured_path)+'\n';
    for(const auto& f:index.files)text+="FILE\t"+std::to_string(f.area)+'\t'+hex(f.name)+'\t'+std::to_string(f.bytes)+'\t'+f.sha256+'\t'+(f.archive.empty()?"-":hex(f.archive))+'\t'+(f.archive_sha256.empty()?"-":f.archive_sha256)+'\t'+f.member+'\n';
    std::vector<std::uint8_t> bytes(text.begin(),text.end());if(!decode_content_index(bytes))throw std::runtime_error("file_backup_index_invalid");return bytes;
}
bool has_content_index(const BackupInspection& inspection){return std::any_of(inspection.files.begin(),inspection.files.end(),[](const auto& f){return f.path==file_contents_index;});}
ContentIndex inspect_content_index(const std::filesystem::path& path,const BackupInspection& inspection){
    if(!inspection.result.ok())throw std::runtime_error("file_backup_archive_invalid");
    if(!has_content_index(inspection))return {};
    const auto member=read_zip_member(path,file_contents_index,bound);
    const auto index=member.result.ok()?decode_content_index(member.bytes):std::nullopt;
    if(!index)throw std::runtime_error("file_backup_index_invalid");
    for(const auto& f:index->files)if(f.archive.empty()){
        const auto found=std::find_if(inspection.files.begin(),inspection.files.end(),[&](const auto& m){return m.path==f.member;});
        if(found==inspection.files.end()||found->size!=f.bytes||found->sha256!=f.sha256)throw std::runtime_error("file_backup_payload_manifest_mismatch");
    }
    return *index;
}
} // namespace sfx::archive
