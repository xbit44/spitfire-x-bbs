// SFX-owned read-only PE32 export inspection. No module/code is loaded.
#pragma once
#include "bridge_model.hpp"
namespace sfxdos {
inline std::set<std::string> pe32_exports(const std::string& bytes){
    auto word=[&](std::size_t p)->unsigned{require(p<=bytes.size()&&bytes.size()-p>=2,"PE word outside file");return unsigned((unsigned char)bytes[p])|(unsigned((unsigned char)bytes[p+1])<<8);};
    auto dword=[&](std::size_t p)->std::uint32_t{require(p<=bytes.size()&&bytes.size()-p>=4,"PE DWORD outside file");return std::uint32_t(word(p))|(std::uint32_t(word(p+2))<<16);};
    require(bytes.size()>=64&&word(0)==0x5a4d,"VDM file is not MZ");std::size_t pe=dword(60);require(pe<bytes.size()&&dword(pe)==0x4550&&word(pe+4)==0x14c,"VDM file is not PE32 x86");
    auto sections=word(pe+6),optional=word(pe+20);std::size_t start=pe+24;
    require(sections&&sections<=96&&optional>=104&&word(start)==0x10b&&start+optional<=bytes.size()&&sections*40<=bytes.size()-start-optional,"invalid PE32 header");
    auto rva=[&](std::uint32_t value,std::size_t length)->std::size_t{
        for(unsigned n=0;n<sections;++n){auto at=start+optional+n*40;auto address=dword(at+12),rawsize=dword(at+16),raw=dword(at+20);
            if(value>=address&&value-address<rawsize){std::size_t offset=value-address;require(length<=rawsize-offset&&raw<=bytes.size()&&offset<=bytes.size()-raw&&length<=bytes.size()-raw-offset,"export outside PE file");return raw+offset;}}
        throw std::runtime_error("export RVA not backed by file");};
    auto directory=dword(start+96);if(!directory)return {};auto exp=rva(directory,40);auto count=dword(exp+24);require(count<=16384,"too many export names");
    auto names=count?rva(dword(exp+32),count*4):0;std::set<std::string> out;
    for(std::uint32_t n=0;n<count;++n){auto address=dword(names+n*4);std::string name;for(unsigned j=0;j<256;++j){auto at=rva(address+j,1);auto c=(unsigned char)bytes[at];if(!c)break;require(c>=33&&c<127&&j<255,"invalid PE export name");name+=(char)c;}require(!name.empty(),"empty PE export name");out.insert(name);}
    return out;
}
}
