// Early adapter composition only: production listener/authentication/SQLite,
// fresh synthetic callers. Installed Runtime and protected IPC remain pending.
#include "sfx/application/chat.hpp"
#define SFX_CHAT_EARLY_FIXTURE 1
#define main retained_chat_fixture_main
#include "alpha_r1_journal_host.cpp"
#undef main
int main(int argc,char** argv){return retained_chat_fixture_main(argc,argv);}
