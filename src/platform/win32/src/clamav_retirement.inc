// Kept separate from installation to make the one-time migration removable
// after owner acceptance. This never selects or launches an AV executable.
namespace {
const std::vector<ClamAvPinnedMember>& retired_documents(){
    static const std::vector<ClamAvPinnedMember> values{
        {L"conf_examples/freshclam.conf.sample",7240,"7118283EADA094C7BB2CD250E1E60C48AD16CC5DCF22C9F39B9850C695E6BCC4"},
        {L"COPYING/COPYING.bzip2",2049,"D72A145C90918184A05EF65A04C9E6F7466FAA59BC1B82C8F6A8DDC7DDCB9BED"},
        {L"COPYING/COPYING.curl",1088,"8C8824F50E73A021F5DDE1FCCBF69685939247399A33A32ABAB1FA448C9DDABB"},
        {L"COPYING/COPYING.file",1872,"DFB818A0D41411C6FB1C193C68B73018CEADD1994BDA41AD541CBFF292894BC6"},
        {L"COPYING/COPYING.getopt",1125,"6DCE638B76399E7521AD8E182D3E33E4496C85B3B69B6FF434B53017101E82AD"},
        {L"COPYING/COPYING.LGPL",26527,"A9BDDE5616ECDD1E980B44F360600EE8783B1F99B8CC83A2BEB163A0A390E861"},
        {L"COPYING/COPYING.llvm",3249,"E3A9B913515A42F8FF3EF1551C3A2CDBA383C39ED959729E0E2911219496AD74"},
        {L"COPYING/COPYING.lzma",210,"C7CBCF00B59DC61165B655B45BF6C21BB685D593861E7ABBC78460C499167B42"},
        {L"COPYING/COPYING.pcre",2515,"ACCDCF2455C07B99ABEA59016B3663EAEF926A92092D103BFAA25FED27CF6B24"},
        {L"COPYING/COPYING.regex",1762,"E2C1395A3D9FEA6D5D25847C9D783DB6E2CC8B085B4025861F459139C5DFD90B"},
        {L"COPYING/COPYING.unrar",2403,"1FACCC6B5C7B958FB807A3F573D5BE9BF7889FE898F7E0617C544B05A81BFD00"},
        {L"COPYING/COPYING.YARA",11532,"A20D6317C5384E8D4C05F9C31097878675D9429EC46090656166039CC10BC957"},
        {L"COPYING/COPYING.zlib",885,"C2F77553F8D870C5635B0DACE0519253233F172B33CE1FDF6578610706294EEE"},
        {L"COPYING.txt",18738,"0C4FD2FA9733FC9122503797648710851E4EE6D9E4969DD33FCBD8C63CD2F584"},
        {L"README.md",5659,"9506726860097FCF9485AF4FCD899FCB6114DB780BE0DC3D99E0A3F3F27004C6"}
    };return values;
}
std::vector<std::uint8_t> retired_bytes(HANDLE h,std::uint64_t size){
    require(size<=32U*1024U*1024U,"setup_legacy_component_limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));std::size_t offset{};
    while(offset<bytes.size()){DWORD got{};require(ReadFile(h,bytes.data()+offset,static_cast<DWORD>(bytes.size()-offset),&got,nullptr)&&got,"setup_legacy_component_read");offset+=got;}
    return bytes;
}
}
struct ClamAvRetirement::Impl {
    struct Entry{ins::Handle handle;Path path;};
    std::vector<Entry> entries;std::vector<ins::Handle> parents;bool done{},legacy_engine{};
    static bool scratch_job(const Path& relative){
        if(relative.empty())return false;
        const auto name=relative.begin()->u8string();
        const auto job_prefix=name.rfind("update-",0)==0?7U:name.rfind("scan-",0)==0?5U:0U;
        return job_prefix&&name.size()==job_prefix+32&&name.substr(job_prefix).find_first_not_of("0123456789abcdefABCDEF")==std::string::npos;
    }
    void inventory(const Path& path,const std::wstring& owner,unsigned role,const Path& relative,const std::wstring& id,std::uint64_t& total){
        require(entries.size()<8192&&std::distance(relative.begin(),relative.end())<=16,"setup_legacy_component_limit");
        const auto attr=GetFileAttributesW(path.c_str());require(attr!=INVALID_FILE_ATTRIBUTES&&!(attr&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_READONLY)),"setup_legacy_component_redirect_or_attributes");
        const bool directory=(attr&FILE_ATTRIBUTE_DIRECTORY)!=0;
        if(directory){
            // Old Win7 Job assignment failure may leave a sealed SFX scratch
            // job. Admit only its exact generated prefix/identity underneath
            // this verified component role; never arbitrary Work directories.
            require(relative.empty()||(role==0&&(relative==L"COPYING"||relative==L"conf_examples"))||(role==4&&scratch_job(relative)),"setup_legacy_component_unknown_directory");
            auto h=ins::open_object(path,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE);
            require(role==0?ins::exact_security(h.get(),ins::structural_descriptor(owner).get()):WindowsStorageDaclPolicy(owner).assess(path,StorageDaclObjectKind::directory,role==4&&!relative.empty()).status.succeeded(),"setup_legacy_component_security");
            entries.push_back({std::move(h),path});WIN32_FIND_DATAW item{};const auto f=FindFirstFileW((path/L"*").c_str(),&item);require(f!=INVALID_HANDLE_VALUE,"setup_legacy_component_enumeration");
            struct Find{HANDLE h;~Find(){FindClose(h);}} close{f};
            do{const Path name=item.cFileName;if(name!=L"."&&name!=L"..")inventory(path/name,owner,role,relative/name,id,total);}while(FindNextFileW(f,&item));require(GetLastError()==ERROR_NO_MORE_FILES,"setup_legacy_component_enumeration");return;
        }
        // Request write access too for engine images, without writing: a live
        // executable/loaded image must refuse preflight, not a later deletion.
        auto h=ins::open_object(path,false,GENERIC_READ|READ_CONTROL|DELETE|(role==0?GENERIC_WRITE:0),FILE_SHARE_READ);
        BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(h.get(),&info)&&info.nNumberOfLinks==1&&!info.nFileSizeHigh,"setup_legacy_component_file_identity");
        total+=info.nFileSizeLow;require(total<=3ULL*1024*1024*1024,"setup_legacy_component_limit");
        const auto name=relative.generic_u8string();
        if(role==0){
            require(ins::exact_security(h.get(),ins::structural_descriptor(owner).get()),"setup_legacy_component_security");
            const auto& members=legacy_engine?legacy_clamav_program_members():clamav_program_members();
            const auto image=std::find_if(members.begin(),members.end(),[&](const auto& m){return Path(m.name).generic_u8string()==name;});
            const auto document=std::find_if(retired_documents().begin(),retired_documents().end(),[&](const auto& m){return Path(m.name).generic_u8string()==name;});
            if(name=="SFX-component.receipt")require(retired_bytes(h.get(),info.nFileSizeLow)==receipt(id,legacy_engine),"setup_legacy_component_receipt");
            else {const auto* pin=image!=members.end()?&*image:document!=retired_documents().end()?&*document:nullptr;require(pin&&info.nFileSizeLow==pin->bytes&&ins::sha256(retired_bytes(h.get(),info.nFileSizeLow))==pin->sha256,"setup_legacy_component_unknown_or_modified_file");}
        }else{
            // The native operation lock and daily guard intentionally use the
            // fixed mutable descriptor, not the non-executable payload policy.
            // The pre-release generated config may also carry that exact ACL.
            // Admit only these names; exact bytes/format are checked below.
            const bool generated_control=((role==4&&name=="SFX-ClamAV.operation.lock"&&info.nFileSizeLow==0)||
                (role==1&&((name=="update-state.dat"&&info.nFileSizeLow<=80)||name=="freshclam.conf")))&&
                ins::exact_security(h.get(),ins::mutable_descriptor(owner).get());
            require(generated_control||WindowsStorageDaclPolicy(owner).assess(path,StorageDaclObjectKind::file,true).status.succeeded()||(role==1&&name=="freshclam.conf"&&ins::exact_security(h.get(),ins::structural_descriptor(owner).get())),"setup_legacy_component_security");
            if(role==1){
                require(info.nFileSizeLow<=65536,"setup_legacy_component_limit");const auto raw=retired_bytes(h.get(),info.nFileSizeLow);
                if(name=="freshclam.conf"){const auto c=clamav_update_configuration(path.parent_path().parent_path().parent_path());require(raw==std::vector<std::uint8_t>(c.begin(),c.end()),"setup_legacy_component_configuration_changed");}
                else if(name=="SFX-update-status.dat")require(valid_clamav_update_status(raw),"setup_legacy_component_history_changed");
                else if(name=="update-state.dat"){const std::string t(raw.begin(),raw.end()),p="SFX-CLAMAV-UPDATE-1\n";require(t.rfind(p,0)==0&&t.size()>p.size()+1&&t.back()=='\n'&&t.substr(p.size(),t.size()-p.size()-1).find_first_not_of("0123456789")==std::string::npos,"setup_legacy_component_state_changed");}
                else require(false,"setup_legacy_component_unknown_file");
            }else if(role==2)require(name=="main.cvd"||name=="main.cld"||name=="daily.cvd"||name=="daily.cld"||name=="bytecode.cvd"||name=="bytecode.cld"||name=="freshclam.dat","setup_legacy_component_unknown_file");
            else if(role==3)require((name=="freshclam.log"||name=="freshclam.log.1")&&info.nFileSizeLow<=2U*1024*1024,"setup_legacy_component_unknown_file");
            else require((name=="SFX-ClamAV.operation.lock"&&info.nFileSizeLow==0)||(scratch_job(relative)&&std::distance(relative.begin(),relative.end())>1),"setup_legacy_component_busy_or_unknown_file");
        }
        entries.push_back({std::move(h),path});
    }
};
ClamAvRetirement::ClamAvRetirement():impl_(std::make_unique<Impl>()){}
ClamAvRetirement::~ClamAvRetirement()=default;
std::unique_ptr<ClamAvRetirement> ClamAvRetirement::prepare(const Path& root,const std::wstring& owner,const std::wstring& id){
    require(windows7_no_av()&&ins::elevated()&&owner==ins::operator_sid(),"setup_legacy_component_authority");
    require(ins::canonical_root(root)==root&&ins::read_receipt(root,owner).id==id,"setup_legacy_component_installation");
    auto result=std::unique_ptr<ClamAvRetirement>(new ClamAvRetirement);
    const std::array<Path,5> paths{root/L"Bin"/L"Tools"/L"ClamAV",root/L"Config"/L"ClamAV",root/L"Data"/L"ClamAV",root/L"Logs"/L"ClamAV",root/L"Work"/L"ClamAV"};
    const auto retained=paths[0].parent_path()/retained_engine_name;
    const bool active_present=!absent(paths[0]),retained_present=!absent(retained);
    if(active_present){
        VerifiedEngine engine(paths[0],owner);
        result->impl_->legacy_engine=engine.bytes==receipt(id,true);
        require(result->impl_->legacy_engine||engine.bytes==receipt(id),"setup_legacy_component_receipt");
        engine.verify_members({},result->impl_->legacy_engine);
    }
    std::uint64_t total{};
    for(unsigned n=0;n<paths.size();++n){if(absent(paths[n]))continue;require(active_present||retained_present,"setup_legacy_component_unrecognized_data");
        auto parents=pin_path(paths[n].parent_path(),true);for(auto& p:parents)result->impl_->parents.push_back(std::move(p));
        result->impl_->inventory(paths[n],owner,n,{},id,total);
    }
    if(retained_present){
        {
            VerifiedEngine engine(retained,owner);
            require(engine.bytes==receipt(id,true),"setup_legacy_component_receipt");
            engine.verify_members({},true);
        }
        auto parents=pin_path(retained.parent_path(),true);for(auto& p:parents)result->impl_->parents.push_back(std::move(p));
        result->impl_->legacy_engine=true;
        result->impl_->inventory(retained,owner,0,{},id,total);
    }
    return result;
}
void ClamAvRetirement::retire(){
    require(windows7_no_av()&&ins::elevated()&&!impl_->done,"setup_legacy_component_authority");
    for(auto i=impl_->entries.rbegin();i!=impl_->entries.rend();++i){FILE_DISPOSITION_INFO d{TRUE};require(SetFileInformationByHandle(i->handle.get(),FileDispositionInfo,&d,sizeof(d))!=FALSE,"setup_legacy_component_retirement_failed");i->handle.reset();}
    for(const auto& e:impl_->entries)require(absent(e.path),"setup_legacy_component_retirement_unconfirmed");impl_->done=true;
}
