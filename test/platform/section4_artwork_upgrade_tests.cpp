// Exact production version/provenance decision; build-only copies, no installation mutation.
#include "../../src/apps/setup/setup_engine.cpp"
#include "sfx/application/botgate.hpp"
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    namespace setup=sfx::apps::setup;namespace ins=sfx::platform::win32::installation;
    try {
        ins::require(argc==4,"old/current Runtime and stock artwork paths required");
        const auto base=std::filesystem::current_path();
        ins::require(base.parent_path().filename()=="build","build-only fixture");
        const auto root=base/L"section4-artwork-fixtures"/ins::unique_id();
        ins::require(std::filesystem::create_directories(root/L"Bin"),"fresh fixture");
        setup::Plan plan;plan.root=root;
        ins::require(!setup::introducing_botgate_art(plan),"fresh installation uses its own artwork seed");
        plan.previous=ins::Receipt{};
        for(int i=1;i<=2;++i) {
            const auto source=std::filesystem::canonical(argv[i]);
            ins::require(source.filename()==L"SFXRuntime.exe" && source.native().find(L"\\build\\")!=std::wstring::npos,"build Runtime source only");
            const auto copy=root/L"Bin"/L"SFXRuntime.exe";
            std::filesystem::copy_file(source,copy,std::filesystem::copy_options::overwrite_existing);
            plan.previous->program_hashes[ins::runtime_program_index]=ins::file_sha256(copy);
            ins::require(setup::introducing_botgate_art(plan)==(i==1),"only pre-v003 first introduction seeds missing art");
            plan.previous->program_hashes[ins::runtime_program_index]=std::string(64,'0');
            bool refused=false;try {(void)setup::introducing_botgate_art(plan);}catch(...) {refused=true;}
            ins::require(refused,"changed old Runtime rejected before version decision");
        }
        const auto artwork=ins::read_file(argv[3],4096);
        ins::require(ins::sha256(artwork)=="F4536F582FAE940C589078AEAA37F229B33EEF0A96D5375346E9E13588294F79","exact preserved owner art");
        const std::string source(artwork.begin(),artwork.end());
        const auto ten=sfx::application::render_botgate_art(source,10,true);
        for(unsigned seconds:{1U,4U,9U,20U}) {
            const auto frame=sfx::application::render_botgate_art(source,seconds,true);
            ins::require(frame.size()==ten.size(),"exact stock frame dimensions unchanged");
            unsigned differences=0;
            for(std::size_t i=0;i<ten.size();++i) if(frame[i]!=ten[i]) ++differences;
            ins::require(differences<=2 && differences>0,"only countdown cells change in exact owner artwork");
        }
        std::cout<<"PASS artwork first-introduction/version/hash boundary: 5 decisions; exact stock countdown: 9 checks\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
