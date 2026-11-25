#include "ftn_external_files.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include "launch.inc"

int main(int argc,char** argv){
    namespace app=sfx::application;namespace files=sfx::platform::external_ftn;
    try{
        const auto root=std::filesystem::current_path();
        if(argc==3&&std::string_view(argv[1])=="--prepare"){
            files::prepare_profile(root,argv[2]);std::cout<<"BinkD handoff folders prepared.\n";return 0;
        }
        if(argc==3&&std::string_view(argv[1])=="--prepare-run"){
            const auto recorder=std::filesystem::absolute(argv[0]).lexically_normal().lexically_relative(root).generic_u8string();
            const auto run=files::prepare_run(root,argv[2],recorder);std::cout<<run.id<<'\n'<<run.configuration_path<<'\n';return 0;
        }
        if((argc==4||(argc==5&&std::string_view(argv[4])=="--server"))&&std::string_view(argv[1])=="--launch"){
            const auto recorder=std::filesystem::absolute(argv[0]).lexically_normal().lexically_relative(root).generic_u8string();
            const auto run=files::prepare_run(root,argv[2],recorder);
#ifdef _WIN32
            constexpr const char* run_folder="FTN/External/Runs/";
#else
            constexpr const char* run_folder="ftn/external/runs/";
#endif
            std::cout<<"BinkD run "<<run.id<<" prepared.\nPrivate diagnostic log: "<<run_folder<<run.id<<"/binkd.log\nKeep this log private; it can contain received passwords.\n"<<std::flush;
            return launch_stock(std::filesystem::u8path(argv[3]),run.configuration_path,argc==5);
        }
        if(argc<6||argc>69||std::string_view(argv[1])!="--record-run"||(std::string_view(argv[4])!="0"&&std::string_view(argv[4])!="1")){
#ifdef _WIN32
            constexpr const char* program="SFXBinkD";
#else
            constexpr const char* program="sfxbinkd";
#endif
            std::cerr<<"Run from the SFX folder.\n"<<program<<" --prepare NETWORK\n"<<program<<" --launch PROFILE BINKD [--server]\n"<<program<<" --prepare-run PROFILE\nReceive recording requires the prepared run's own hook.\n";return 2;
        }
        std::string addresses;for(int i=5;i<argc;++i){if(i!=5)addresses+=' ';addresses+=argv[i];}
        std::istringstream input(addresses);std::string address;std::vector<app::FtnAddress> remote;
        while(input>>address){const auto parsed=app::parse_ftn_address(address);if(!parsed||remote.size()==64)throw std::runtime_error("ftn_external_hook_addresses_invalid");remote.push_back(*parsed);}
        std::string file=argv[3];
#ifdef _WIN32
        std::replace(file.begin(),file.end(),'\\','/');
#endif
        (void)files::record_run_completed(root,argv[2],std::move(file),std::string_view(argv[4])=="1",remote);
        std::cout<<"Received file recorded for SFX.\n";return 0;
    }catch(const std::exception&){
        // Never echo untrusted hook arguments, paths or a configuration secret.
        std::cerr<<"BinkD setup, launch or recording could not finish. Files retained; check the private SFX folder, supported profile and mailer path.\n";return 1;
    }
}
