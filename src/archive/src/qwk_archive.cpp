#include "sfx/archive/qwk_archive.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <set>
#include <utility>

namespace sfx::archive {
namespace {
struct ReaderFree { void operator()(struct archive* a) const { if(a)archive_read_free(a); } };
struct WriterFree { void operator()(struct archive* a) const { if(a)archive_write_free(a); } };
struct EntryFree { void operator()(struct archive_entry* e) const { if(e)archive_entry_free(e); } };
QwkArchiveResult fail(Error error,const char* detail) { return {{error,detail},{},{}}; }
std::string uppercase(std::string_view text){std::string r(text);for(auto& c:r)if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');return r;}
std::uint16_t le16(std::string_view data,std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(data[offset])|(static_cast<unsigned char>(data[offset+1])<<8));
}
std::uint32_t le32(std::string_view data,std::size_t offset) {
    return static_cast<std::uint32_t>(le16(data,offset))|(static_cast<std::uint32_t>(le16(data,offset+2))<<16);
}
struct Directory {std::uint16_t count{};std::size_t offset{},end{};};
std::optional<Directory> envelope(std::string_view data) {
    // Require one ordinary single-disk ZIP with an exact final directory, not
    // an embedded ZIP in an executable, a multipart ZIP or trailing payload.
    if(data.size()<22 || data.substr(0,4)!=std::string_view("PK\3\4",4))return {};
    const auto minimum=data.size()>65557?data.size()-65557:0;
    for(std::size_t p=data.size()-22;;--p){
        if(data.substr(p,4)==std::string_view("PK\5\6",4) && le16(data,p+20)==data.size()-p-22){
            const auto entries=le16(data,p+10);
            if(le16(data,p+4)!=0 || le16(data,p+6)!=0 || entries==0 || entries!=le16(data,p+8))return {};
            const auto size=le32(data,p+12), offset=le32(data,p+16);
            if(offset>p || size!=p-offset)return {};
            return Directory{entries,offset,p};
        }
        if(p==minimum)break;
    }
    return {};
}
bool reply_directory(std::string_view zip,const Directory& directory,const std::string& message_name){
    std::set<std::string> names;std::vector<std::pair<std::size_t,std::size_t>> local_ranges;
    std::size_t pos=directory.offset;
    for(unsigned index=0;index<directory.count;++index){
        if(directory.end-pos<46 || zip.substr(pos,4)!=std::string_view("PK\1\2",4))return false;
        const auto flags=le16(zip,pos+8),method=le16(zip,pos+10),name_length=le16(zip,pos+28);
        const auto extra=le16(zip,pos+30),comment=le16(zip,pos+32);
        const auto central_size=46U+static_cast<std::size_t>(name_length)+extra+comment;
        if(central_size>directory.end-pos || !name_length || (flags&~0x080eU)!=0 || (method!=0 && method!=8) || le16(zip,pos+34)!=0)return false;
        const auto raw_name=zip.substr(pos+46,name_length);const auto name=uppercase(raw_name);
        if((name!=message_name && name!="HEADERS.DAT" && name!="TODOOR.EXT") || !names.insert(name).second)return false;
        const auto local=static_cast<std::size_t>(le32(zip,pos+42));
        const auto compressed=static_cast<std::size_t>(le32(zip,pos+20));const auto expanded=le32(zip,pos+24),crc=le32(zip,pos+16);
        if(local>directory.offset || directory.offset-local<30 || zip.substr(local,4)!=std::string_view("PK\3\4",4) || le16(zip,local+6)!=flags || le16(zip,local+8)!=method)return false;
        const auto local_name=le16(zip,local+26),local_extra=le16(zip,local+28);const auto header_size=30U+static_cast<std::size_t>(local_name)+local_extra;
        if(header_size>directory.offset-local || raw_name!=zip.substr(local+30,local_name) || compressed>directory.offset-local-header_size)return false;
        std::size_t end=local+header_size+compressed;
        if(flags&8){
            if(directory.offset-end<12)return false;
            if(zip.substr(end,4)==std::string_view("PK\7\10",4)){end+=4;if(directory.offset-end<12)return false;}
            if(le32(zip,end)!=crc || le32(zip,end+4)!=compressed || le32(zip,end+8)!=expanded)return false;
            end+=12;
        }else if(le32(zip,local+14)!=crc || le32(zip,local+18)!=compressed || le32(zip,local+22)!=expanded)return false;
        local_ranges.emplace_back(local,end);pos+=central_size;
    }
    if(pos!=directory.end || !names.count(message_name))return false;
    std::sort(local_ranges.begin(),local_ranges.end());std::size_t next{};
    for(const auto& range:local_ranges){if(range.first!=next)return false;next=range.second;}
    return next==directory.offset;
}
bool leaf(std::string_view name){
    return !name.empty() && name.size()<=12 && std::all_of(name.begin(),name.end(),[](unsigned char c){return (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='.' || c=='-' || c=='_';});
}
bool download_name(const std::string& name){
    if(name=="MESSAGES.DAT" || name=="CONTROL.DAT" || name=="DOOR.ID" || name=="HEADERS.DAT" || name=="PERSONAL.NDX" || name=="TOREADER.EXT")return true;
    if(name.size()<7 || name.size()>9 || name.substr(name.size()-4)!=".NDX")return false;
    return std::all_of(name.begin(),name.end()-4,[](char c){return c>='0' && c<='9';});
}
struct Sink { std::string bytes;CancelCheck cancelled;Error error{Error::none}; };
la_ssize_t write_memory(struct archive*,void* client,const void* data,std::size_t size){
    auto& sink=*static_cast<Sink*>(client);
    try {
    if(sink.cancelled && sink.cancelled()){sink.error=Error::cancelled;return -1;}
    if(size>qwk_zip_limit-sink.bytes.size()){sink.error=Error::limit_exceeded;return -1;}
    sink.bytes.append(static_cast<const char*>(data),size);return static_cast<la_ssize_t>(size);
    }catch(...){sink.error=Error::archive_io;return -1;}
}
} // namespace
QwkArchiveResult read_qwk_reply_zip(std::string_view zip,std::string_view board_id,CancelCheck cancelled){
    if(zip.size()>qwk_zip_limit)return fail(Error::limit_exceeded,"Compressed reply packet exceeds 16 MiB.");
    const auto directory=envelope(zip);if(!directory || directory->count>3)return fail(Error::archive_invalid,"Invalid or unsupported reply ZIP envelope.");
    const auto message_name=uppercase(board_id)+".MSG";
    if(board_id.empty() || board_id.size()>8 || message_name.find_first_of("/\\:")!=std::string::npos)return fail(Error::invalid_request,"Invalid configured board identity.");
    if(!reply_directory(zip,*directory,message_name))return fail(Error::archive_invalid,"Reply ZIP directory and local records disagree or contain unsupported data.");
    std::unique_ptr<struct archive,ReaderFree> reader(archive_read_new());
    if(!reader || archive_read_support_format_zip(reader.get())!=ARCHIVE_OK || archive_read_support_filter_none(reader.get())!=ARCHIVE_OK ||
        archive_read_open_memory(reader.get(),zip.data(),zip.size())!=ARCHIVE_OK)return fail(Error::archive_invalid,"Reply packet is not a readable ZIP.");
    QwkArchiveResult result;std::set<std::string> names;std::uint64_t total{};struct archive_entry* entry=nullptr;
    for(;;){
        if(cancelled && cancelled())return fail(Error::cancelled,"Reply packet validation cancelled.");
        const int status=archive_read_next_header(reader.get(),&entry);
        if(status==ARCHIVE_EOF)break;
        if(status!=ARCHIVE_OK || !entry)return fail(Error::archive_invalid,"Malformed reply ZIP entry.");
        const auto* raw=archive_entry_pathname(entry);if(!raw)return fail(Error::unsafe_path,"Reply ZIP entry has no name.");
        const auto name=uppercase(raw);
        if(name!=message_name && name!="HEADERS.DAT" && name!="TODOOR.EXT")return fail(Error::unsafe_path,"Reply ZIP contains an unsupported entry.");
        if(!names.insert(name).second || archive_entry_filetype(entry)!=AE_IFREG || archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            archive_entry_is_encrypted(entry)!=0 || archive_entry_size(entry)<0 || static_cast<std::uint64_t>(archive_entry_size(entry))>qwk_expanded_limit)
            return fail(Error::archive_invalid,"Duplicate, linked, encrypted or invalid reply ZIP entry.");
        QwkArchiveFile file{name,{}};std::array<char,65536> buffer{};
        for(;;){
            if(cancelled && cancelled())return fail(Error::cancelled,"Reply packet validation cancelled.");
            const auto got=archive_read_data(reader.get(),buffer.data(),buffer.size());
            if(got<0)return fail(Error::archive_invalid,"Reply ZIP data or CRC validation failed.");
            if(got==0)break;
            const auto bytes=static_cast<std::uint64_t>(got);
            if(bytes>qwk_expanded_limit-total)return fail(Error::limit_exceeded,"Actual expanded reply packet exceeds 64 MiB.");
            total+=bytes;file.bytes.append(buffer.data(),static_cast<std::size_t>(got));
        }
        if(file.bytes.size()!=static_cast<std::uint64_t>(archive_entry_size(entry)))return fail(Error::archive_invalid,"Reply ZIP entry size disagrees with actual data.");
        result.files.push_back(std::move(file));
    }
    if(result.files.size()!=directory->count || !names.count(message_name) || archive_read_has_encrypted_entries(reader.get())>0 || archive_read_close(reader.get())!=ARCHIVE_OK)
        return fail(Error::archive_invalid,"Incomplete, encrypted or mismatched reply ZIP directory.");
    return result;
}
QwkArchiveResult write_qwk_zip(const std::vector<QwkArchiveFile>& files,CancelCheck cancelled){
    if(files.empty() || files.size()>1007)return fail(Error::invalid_request,"Invalid QWK file set.");
    std::set<std::string> names;std::uint64_t total{};
    for(const auto& file:files){const auto name=uppercase(file.name);if(!leaf(name) || !download_name(name) || !names.insert(name).second)return fail(Error::unsafe_path,"Invalid or duplicate QWK output file.");
        if(file.bytes.size()>qwk_expanded_limit-total)return fail(Error::limit_exceeded,"Expanded QWK including metadata exceeds 64 MiB.");
        total+=file.bytes.size();}
    if(!names.count("MESSAGES.DAT") || !names.count("CONTROL.DAT") || !names.count("DOOR.ID"))return fail(Error::invalid_request,"QWK requires message and control files.");
    std::unique_ptr<struct archive,WriterFree> writer(archive_write_new());
    if(!writer || archive_write_set_format_zip(writer.get())!=ARCHIVE_OK || archive_write_set_format_option(writer.get(),"zip","compression","deflate")!=ARCHIVE_OK ||
        archive_write_set_bytes_per_block(writer.get(),0)!=ARCHIVE_OK)return fail(Error::archive_io,"Cannot initialize QWK ZIP writer.");
    Sink sink{{},std::move(cancelled),Error::none};
    if(archive_write_open(writer.get(),&sink,nullptr,write_memory,nullptr)!=ARCHIVE_OK)return fail(Error::archive_io,"Cannot open QWK ZIP writer.");
    for(const auto& file:files){
        if(sink.cancelled && sink.cancelled())return fail(Error::cancelled,"QWK construction cancelled.");
        std::unique_ptr<struct archive_entry,EntryFree> entry(archive_entry_new());if(!entry)return fail(Error::archive_io,"Cannot create QWK ZIP entry.");
        archive_entry_set_pathname(entry.get(),file.name.c_str());archive_entry_set_filetype(entry.get(),AE_IFREG);archive_entry_set_perm(entry.get(),0600);
        archive_entry_set_size(entry.get(),static_cast<la_int64_t>(file.bytes.size()));archive_entry_set_mtime(entry.get(),0,0);
        if(archive_write_header(writer.get(),entry.get())!=ARCHIVE_OK)return fail(sink.error==Error::none?Error::archive_io:sink.error,"Cannot write complete QWK ZIP header.");
        for(std::size_t offset=0;offset<file.bytes.size();){const auto size=std::min<std::size_t>(65536,file.bytes.size()-offset);
            const auto written=archive_write_data(writer.get(),file.bytes.data()+offset,size);
            if(written<=0 || static_cast<std::size_t>(written)>size)return fail(sink.error==Error::none?Error::archive_io:sink.error,"Cannot write complete QWK ZIP data.");
            offset+=static_cast<std::size_t>(written);}
        if(archive_write_finish_entry(writer.get())!=ARCHIVE_OK)return fail(sink.error==Error::none?Error::archive_io:sink.error,"Cannot finish complete QWK ZIP entry.");
    }
    if(archive_write_close(writer.get())!=ARCHIVE_OK || sink.error!=Error::none)return fail(sink.error==Error::none?Error::archive_io:sink.error,"Cannot finish complete QWK ZIP.");
    if(!envelope(sink.bytes))return fail(Error::archive_invalid,"Generated QWK ZIP envelope is inconsistent.");
    QwkArchiveResult result;result.zip=std::move(sink.bytes);return result;
}
} // namespace sfx::archive
