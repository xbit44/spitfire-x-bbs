#include "sfx/application/session_revocation.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    namespace app=sfx::application;
    unsigned checks{};
    const auto require=[&](bool ok,const char* label) { ++checks; if(!ok) throw std::runtime_error(label); };
    try {
        app::SessionRevocationLedger ledger;
        app::Identifier128 first,second; first.bytes[0]=1; second.bytes[0]=2;
        const auto accepted=ledger.checkpoint();
        require(ledger.permits(accepted,1,false,first),"fresh handoff allowed");
        require(ledger.record({app::SessionRevocationKind::account_all,1,{}}),"record account reset");
        require(!ledger.permits(accepted,1,false,first),"delayed authenticated handoff cannot escape reset");
        require(ledger.permits(accepted,2,false,second),"other account unaffected");
        auto later=ledger.checkpoint();
        require(ledger.permits(later,1,false,first),"new connection after reset can authenticate normally");
        require(ledger.record({app::SessionRevocationKind::sysop_level_all,{},{}}),"record permanent SY replacement");
        require(!ledger.permits(later,1,true,first) && ledger.permits(later,1,false,first),
            "SY invalidates only Sysop handoffs, never ordinary callers");
        later=ledger.checkpoint();
        require(ledger.record({app::SessionRevocationKind::account_except_current,1,first}),"record voluntary change");
        require(ledger.permits(later,1,true,first) && !ledger.permits(later,1,true,second),
            "exact current session survives; another session does not");
        const auto unchanged=ledger.checkpoint();
        require(!ledger.record({app::SessionRevocationKind::account_all,{},{}}) &&
            ledger.checkpoint()==unchanged,"malformed command has no mutation");
        for(std::size_t i=0;i<app::SessionRevocationLedger::capacity;++i)
            require(ledger.record({app::SessionRevocationKind::account_all,99,{}}),"bounded history fills");
        require(ledger.permits(unchanged,1,false,first),"exact history capacity remains usable");
        require(!ledger.permits(accepted,1,false,first),"overrun proof fails closed without unbounded history");
        require(!ledger.permits(ledger.checkpoint()+1,1,false,first) &&
            !ledger.permits(ledger.checkpoint(),0,false,first),"future proof and invalid identity rejected");
        std::cout<<"PASS session revocation: "<<checks<<" checks\n"; return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL session revocation: "<<error.what()<<'\n'; return 1; }
}
