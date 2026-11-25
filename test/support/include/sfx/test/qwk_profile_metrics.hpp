#pragma once
// Diagnostic-only nested counters. No caller content or production log output.
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <windows.h>
namespace sfx::test::qwk_profile {
enum Key { total, setup, selection, references, parent_reference, rendition,
    single_encode, full_encode, zip_total, zip_header, zip_data, zip_finish,
    zip_close, zip_callback, authorization, account_read, policy_read,
    qwk_policy_read, observer, delivery_plan, digest, final_authority, key_count };
inline constexpr const char* names[]{"total","setup","selection","references","parent_reference","rendition","single_encode","full_encode","zip_total","zip_header","zip_data","zip_finish","zip_close","zip_callback","authorization","account_read","policy_read","qwk_policy_read","observer","delivery_plan","digest","final_authority"};
struct Sample {std::uint64_t wall{},thread{},process{};};
inline Sample now(){
    FILETIME created{},exited{},kernel{},user{};ULARGE_INTEGER k{},u{};Sample result;
    result.wall=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if(GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)){k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;result.thread=(k.QuadPart+u.QuadPart)*100;}
    if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)){k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;result.process=(k.QuadPart+u.QuadPart)*100;}
    return result;
}
struct Counter {std::uint64_t calls{},wall{},self_wall{},thread{},process{},maximum{};};
struct State {std::array<Counter,key_count> values{};};
inline thread_local State* active{};
struct Scope;
inline thread_local Scope* top{};
struct Scope {
    Key key;State* state;Scope* parent;Sample began{};std::uint64_t children{};
    explicit Scope(Key value):key(value),state(active),parent(top){if(state){began=now();top=this;}}
    ~Scope(){if(!state)return;const auto end=now();const auto elapsed=end.wall-began.wall;auto& c=state->values[key];++c.calls;c.wall+=elapsed;c.self_wall+=elapsed>=children?elapsed-children:0;c.thread+=end.thread-began.thread;c.process+=end.process-began.process;if(elapsed>c.maximum)c.maximum=elapsed;top=parent;if(parent)parent->children+=elapsed;}
};
template<class F> decltype(auto) measured(Key key,F&& fn){Scope scope(key);return std::forward<F>(fn)();}
inline void write(std::ostream& out,const State& s){
    out<<"{";for(unsigned i=0;i<key_count;++i){if(i)out<<',';const auto& c=s.values[i];out<<'"'<<names[i]<<"\":{\"calls\":"<<c.calls<<",\"wall_us\":"<<c.wall/1000<<",\"self_wall_us\":"<<c.self_wall/1000<<",\"thread_cpu_us\":"<<c.thread/1000<<",\"process_cpu_us\":"<<c.process/1000<<",\"max_us\":"<<c.maximum/1000<<'}';}out<<"}";
}
}
