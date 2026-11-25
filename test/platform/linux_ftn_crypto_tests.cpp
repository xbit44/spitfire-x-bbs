#include "sfx/platform/linux/ftn_crypto.hpp"
#include <iostream>
#include <set>
#include <stdexcept>
namespace {unsigned checks{};void check(bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);}}
int main(){try{
    using namespace sfx::platform::linux_host;
    const auto response=ftn_cram_response("Jefe","7768617420646f2079612077616e7420666f72206e6f7468696e673f");
    check(response&&response->view()=="CRAM-MD5-750c783e6ab0b503eaa86e310a5db738","independent RFC2202 HMAC-MD5 vector with decoded BinkP challenge");
    check(verify_ftn_cram("Jefe","7768617420646f2079612077616e7420666f72206e6f7468696e673f","CRAM-MD5-750C783E6AB0B503EAA86E310A5DB738"),"uppercase digest accepted");
    check(!verify_ftn_cram("wrong","7768617420646f2079612077616e7420666f72206e6f7468696e673f",response->view()),"wrong shared secret rejected");
    const std::string challenge32="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    const std::string independent="CRAM-MD5-2e0f903f958ddc40eb4f9cba8d580ec8";
    check(check_ftn_cram("CaseNine9",challenge32,independent)==FtnCramCheck::accepted,"independent Python HMAC vector: nine character secret and full 32 byte challenge");
    check(verify_ftn_cram("fixture-session-key","0102030405060708090a0b0c0d0e0f10","CRAM-MD5-5eea2e79c267f0a08b6ec64bf06ad0c9"),"actual IREX and independent HMAC agree on complete 16 byte challenge");
    check(!verify_ftn_cram("fixture-session-key","0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20","CRAM-MD5-5eea2e79c267f0a08b6ec64bf06ad0c9"),"never accept a truncated-challenge response to a longer peer challenge");
    check(check_ftn_cram("CaseNine",challenge32,independent)==FtnCramCheck::digest_mismatch,"truncated password is distinguished from response format");
    check(check_ftn_cram("casenine9",challenge32,independent)==FtnCramCheck::digest_mismatch,"case remains significant");
    check(check_ftn_cram("CaseNine9",challenge32,independent+std::string(1,'\0'))==FtnCramCheck::trailing_nul,"trailing NUL identified without accepting response");
    check(check_ftn_cram("CaseNine9",challenge32,independent+" option")==FtnCramCheck::length,"extra response options remain rejected but distinguishable");
    check(check_ftn_cram("CaseNine9",challenge32,independent.substr(0,40))==FtnCramCheck::length,"short response classified");
    auto invalid_hex=independent;invalid_hex.back()='g';
    check(check_ftn_cram("CaseNine9",challenge32,invalid_hex)==FtnCramCheck::encoding,"nonhex digest classified");
    check(check_ftn_cram("CaseNine9","invalid",independent)==FtnCramCheck::unavailable,"local computation failure distinct from peer mismatch");
    for(const auto invalid:{"","0","00112233445566","001122334455667x"})check(!ftn_cram_response("key",invalid),"invalid/short challenge refused");
    check(!ftn_cram_response("",std::string(64,'0'))&&!ftn_cram_response(std::string(129,'x'),std::string(64,'0'))&&!ftn_cram_response("key",std::string(130,'0')),"credential/challenge bounds");
    std::set<std::string> seen;for(unsigned i=0;i<64;++i){const auto challenge=ftn_cram_challenge();check(challenge&&challenge->size()==32&&seen.insert(*challenge).second,"actual OpenSSL RAND fresh16-byte legacy-compatible challenge");
        const auto proof=ftn_cram_response("separate-session-secret",*challenge);check(proof&&verify_ftn_cram("separate-session-secret",*challenge,proof->view()),"actual crypto response validates");
        auto changed=*challenge;changed[0]=changed[0]=='0'?'1':'0';check(!verify_ftn_cram("separate-session-secret",changed,proof->view()),"old response cannot replay under a new challenge");}
    std::cout<<"PASS "<<checks<<" actual Linux FTN CRAM crypto checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
