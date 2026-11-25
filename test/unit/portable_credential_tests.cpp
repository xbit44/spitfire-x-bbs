#include "sfx/platform/portable_credential_codec.hpp"
#include <sodium.h>
#include <iostream>
#include <set>
#include <stdexcept>
namespace portable=sfx::platform::portable;
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
int main()try{
    check(sodium_init()>=0,"pinned crypto initialized");portable::PortableCredentialKey key,other;randombytes_buf(key.bytes.data(),key.bytes.size());randombytes_buf(other.bytes.data(),other.bytes.size());
    portable::PortableBoardIdentity board{},different_board{};randombytes_buf(board.data(),board.size());randombytes_buf(different_board.data(),different_board.size());
    constexpr portable::PortableCredentialFamily ftn{'S','F','X','F'},rlogin{'S','F','X','R'};const std::string purpose="FTN/FIDO/PEER/session",text="synthetic connection credential";
    const auto sealed=portable::seal_portable_credential(key,board,ftn,purpose,text);check(bool(sealed),"portable envelope sealed");
    sfx::application::PortableCredentialGeneration old_generation{};crypto_hash_sha256(old_generation.data(),reinterpret_cast<const unsigned char*>("legacy encrypted record"),23);
    const auto converted=portable::seal_portable_credential(key,board,ftn,purpose,text,&old_generation),retried=portable::seal_portable_credential(key,board,ftn,purpose,text,&old_generation);
    check(converted&&retried&&*converted!=*retried&&sfx::application::retained_portable_ftn_generation(*converted)==old_generation&&sfx::application::retained_portable_ftn_generation(*retried)==old_generation,"verified migration retains legacy fingerprint across nonce changes");
    check(portable::open_portable_credential(key,board,ftn,purpose,*converted)->view()==text,"retained migration generation is authenticated and decrypts");
    check(sfx::application::retained_portable_ftn_generation(*sealed)!=old_generation,"ordinary password save starts a fresh credential generation");
    check(!sfx::application::retained_portable_ftn_generation({'S','F','X','F',2})&&!sfx::application::retained_portable_ftn_generation(std::vector<std::uint8_t>(20,0)),"legacy or malformed envelope has no portable generation");
    check(portable::open_portable_credential(key,board,ftn,purpose,*sealed)->view()==text,"portable credential lossless without Windows/DPAPI parameters");
    check(!portable::open_portable_credential(other,board,ftn,purpose,*sealed),"another key cannot decode");check(!portable::open_portable_credential(key,different_board,ftn,purpose,*sealed),"another board cannot decode");
    check(!portable::open_portable_credential(key,board,ftn,"FTN/FIDO/PEER/packet",*sealed),"credential-purpose substitution refused");check(!portable::open_portable_credential(key,board,rlogin,purpose,*sealed),"credential-family substitution refused");
    for(std::size_t n=0;n<sealed->size();++n){auto tampered=*sealed;tampered[n]^=1;check(!portable::open_portable_credential(key,board,ftn,purpose,tampered),"every modified header/nonce/ciphertext/tag byte refused");}
    for(std::size_t n=0;n<sealed->size();++n){auto short_record=*sealed;short_record.resize(n);check(!portable::open_portable_credential(key,board,ftn,purpose,short_record),"every truncation refused");}
    auto suffix=*sealed;suffix.push_back(0);check(!portable::open_portable_credential(key,board,ftn,purpose,suffix),"extra suffix refused");
    std::set<std::vector<std::uint8_t>> unique;for(unsigned n=0;n<128;++n){auto next=portable::seal_portable_credential(key,board,ftn,purpose,text);check(next&&unique.insert(*next).second,"repeated saves use distinct nonces/envelopes");}
    for(const auto size:{0U,1U,128U}){const std::string input(size,'x');const auto packet=portable::seal_portable_credential(key,board,rlogin,"RLOGIN",input);check(packet&&portable::open_portable_credential(key,board,rlogin,"RLOGIN",*packet)->view()==input,"empty/boundary plaintext roundtrip");}
    check(!portable::seal_portable_credential(key,board,ftn,purpose,std::string(129,'x'))&&!portable::seal_portable_credential(key,board,ftn,"",text)&&!portable::seal_portable_credential(key,board,ftn,std::string(1025,'x'),text),"plaintext/purpose bounds enforced");
    auto moved=std::move(key);check(sodium_is_zero(key.bytes.data(),key.bytes.size())==1&&portable::open_portable_credential(moved,board,ftn,purpose,*sealed).has_value(),"move transfers key and wipes old storage");
    std::cout<<"PASS "<<checks<<" portable credential envelope checks; file storage/migration/replacement-PC qualification separate\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}
