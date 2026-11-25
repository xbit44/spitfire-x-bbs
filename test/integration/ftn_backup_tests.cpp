#include "sfx/archive/backup_service.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
namespace arc=sfx::archive;
namespace ins=sfx::platform::win32::installation;
namespace {
unsigned checks{};
void check(bool yes,const char* reason){++checks;if(!yes)throw std::runtime_error(reason);}
void write(const std::filesystem::path& path,std::string_view bytes){std::filesystem::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out.write(bytes.data(),bytes.size());check(bool(out),"fixture write");}
std::string read(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);check(bool(in),"fixture read");return {std::istreambuf_iterator<char>(in),{}};}
arc::Timestamp stamp(unsigned n){return {"20261029-12000"+std::to_string(n),"2026-10-29T12:00:0"+std::to_string(n),"-07:00","2026-10-29T19:00:0"+std::to_string(n)+"Z"};}
bool has(const arc::BackupInspection& value,std::string_view path){return std::any_of(value.files.begin(),value.files.end(),[&](const auto& f){return f.path==path;});}
}
int main(int argc,char** argv){try{
    check(argc==2,"new isolated backup root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve prior backup evidence");
    for(const auto* role:{"Config","Data","Display","Doors","Logs","BotGate","Backup/Upgrades","Backup/Scheduled","Work","FTN/Inbound","FTN/Outbound","FTN/Partials","FTN/Receipts"})std::filesystem::create_directories(root/role);
    write(root/"Data/SFX.DB","journal-A");write(root/"FTN/Inbound/received.bin","received-A");write(root/"FTN/Outbound/queued.pkt","queued-A");write(root/"FTN/Partials/resume.part","partial-A");write(root/"FTN/Receipts/ack.txt","receipt-A");write(root/"Work/FTN/scratch","disposable");
    arc::ManagedBackupRequest backup;backup.installation_root=root;backup.transaction_id="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";backup.created=stamp(0);backup.ftn_layout=false;
    const auto legacy=arc::create_managed_backup(backup);check(legacy.result.ok(),"old capability backup remains valid despite untrusted FTN pathname");const auto old=arc::inspect_backup(legacy.archive_path);check(!has(old,"FTN/Inbound/received.bin"),"pathname alone does not expand old backup contract");
    backup.created=stamp(1);backup.transaction_id="BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";backup.ftn_layout=true;
    const auto saved=arc::create_managed_backup(backup);check(saved.result.ok(),"capable backup includes durable FTN scope");const auto contents=arc::inspect_backup(saved.archive_path);check(contents.result.ok(),"full FTN backup verified");
    for(const auto* path:{"FTN/Inbound/received.bin","FTN/Outbound/queued.pkt","FTN/Partials/resume.part","FTN/Receipts/ack.txt","Data/SFX.DB"})check(has(contents,path),"exact durable mail/journal member retained");check(!has(contents,"Work/FTN/scratch"),"temporary FTN work excluded");
    auto directory=ins::open_object(root/L"FTN",true);const auto identity=ins::file_id(directory.get());directory.reset();
    write(root/"Data/SFX.DB","journal-B");write(root/"FTN/Outbound/queued.pkt","queued-B");
    arc::RestoreRequest restore;restore.installation_root=root;restore.archive_path=saved.archive_path;restore.transaction_id="CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC";restore.pre_restore_created=stamp(2);
    // Core transaction qualification uses explicit synthetic host callbacks;
    // actual installation ACL/publication is qualified by the native Setup run.
    restore.secure_live_directory=[](const auto&){return arc::Result{};};
    restore.secure_live_file=[](const auto&){return arc::Result{};};
    const auto refused=arc::restore_backup(restore);check(!refused.result.ok()&&!refused.live_state_changed&&read(root/"FTN/Outbound/queued.pkt")=="queued-B","old receipt cannot acquire FTN capability by restoring an archive");
    restore.ftn_layout=true;const auto restored=arc::restore_backup(restore);if(!restored.result.ok())std::cerr<<"RESTORE: "<<restored.result.detail<<'\n';check(restored.result.ok(),"capable core restore includes spool and journal in one recovery transaction");check(read(root/"Data/SFX.DB")=="journal-A"&&read(root/"FTN/Outbound/queued.pkt")=="queued-A","matched journal and queued mail restored");
    directory=ins::open_object(root/L"FTN",true);check(ins::file_id(directory.get())==identity,"restore preserves receipt-bound FTN root object identity");directory.reset();check(read(restored.retained_previous_state/"FTN/Outbound/queued.pkt")=="queued-B","prior queued bytes retained in rollback recovery state");
    write(root/"Data/SFX.DB","journal-C");write(root/"FTN/Outbound/queued.pkt","queued-C");restore.transaction_id="DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD";restore.pre_restore_created=stamp(3);restore.verify_live_publication=[] {return arc::Result{arc::Error::publication_failed,"injected final verification refusal"};};
    const auto failed=arc::restore_backup(restore);check(!failed.result.ok()&&failed.rollback_attempted&&failed.rollback_succeeded,"failed final verification rolls back all roles including FTN");check(read(root/"Data/SFX.DB")=="journal-C"&&read(root/"FTN/Outbound/queued.pkt")=="queued-C","rollback retains matching current queue and journal");
    std::cout<<"PASS "<<checks<<" capability-aware FTN backup/restore/preservation/rollback checks; isolated synthetic files, no owner recovery backup\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
