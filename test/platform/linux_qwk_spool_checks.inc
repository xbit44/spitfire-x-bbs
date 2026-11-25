    // Same-account changes are observable refusals, not Windows sharing locks.
    const auto unchanged=[&]{return scalar(database,"SELECT count(*) FROM sfx_local_messages")==2&&scalar(database,"SELECT count(*) FROM sfx_qwk_rep_receipts")==1;};
    const auto reject_changed=[&](const auto& path,const std::shared_ptr<app::IQwkReplySource>& value){
        check(!value->verify_identity()&&!value->rewind()&&!value->next(),"changed spool refuses all reader entry points");
        check(!storage.import_qwk_reply(bob,value)&&unchanged(),"changed spool cannot publish any records");
        value->confirm_completed();check(std::filesystem::exists(path),"uncertain completion never erases work");
    };
    {
        const auto path=job(L"changed-mode");auto value=sfx::apps::runtime::seal_linux_qwk_reply_spool(path,packet,512,4096);
        check(static_cast<bool>(value),"permission mutation source");check(chmod((path/"reply.spool").c_str(),0600)==0,"owner changes permissions");
        reject_changed(path,*value.value);value.value.reset();check(std::filesystem::exists(path/"reply.spool"),"changed mode retained");
    }
    {
        const auto path=job(L"changed-content");auto value=sfx::apps::runtime::seal_linux_qwk_reply_spool(path,packet,512,4096);
        check(static_cast<bool>(value),"content mutation source");struct stat before{};check(stat((path/"reply.spool").c_str(),&before)==0,"original write timestamp");
        check(chmod((path/"reply.spool").c_str(),0600)==0,"open deliberate content mutation");
        host::FileDescriptor edit(open((path/"reply.spool").c_str(),O_WRONLY|O_CLOEXEC));const char bad='X';
        check(edit&&pwrite(edit.get(),&bad,1,0)==1&&fsync(edit.get())==0&&fchmod(edit.get(),0400)==0,"mutate same length and restore mode");
        const struct timespec times[2]={before.st_atim,before.st_mtim};check(futimens(edit.get(),times)==0,"restore old mtime does not restore ctime");
        reject_changed(path,*value.value);
    }
    {
        const auto path=job(L"replaced-file");auto value=sfx::apps::runtime::seal_linux_qwk_reply_spool(path,packet,512,4096);
        check(static_cast<bool>(value),"replacement source");std::filesystem::rename(path/"reply.spool",path/"old.spool");
        host::publish_private_file(host::open_owned_directory(path).get(),"reply.spool",{'K','E','E','P'},false);
        reject_changed(path,*value.value);value.value.reset();
        check(host::read_private_file(host::open_owned_directory(path).get(),"reply.spool",4)==std::vector<std::uint8_t>({'K','E','E','P'}),"replacement preserved on destruction");
    }
    {
        const auto path=job(L"moved-job");auto value=sfx::apps::runtime::seal_linux_qwk_reply_spool(path,packet,512,4096);
        check(static_cast<bool>(value),"directory mutation source");const auto moved=root/"retained-moved-job";std::filesystem::rename(path,moved);
        check(std::filesystem::create_directory(path)&&chmod(path.c_str(),0700)==0,"replacement directory");
        reject_changed(moved,*value.value);value.value.reset();check(std::filesystem::exists(moved/"reply.spool"),"moved spool retained");
    }
    {
        const auto path=job(L"linked-file");auto value=sfx::apps::runtime::seal_linux_qwk_reply_spool(path,packet,512,4096);
        check(static_cast<bool>(value)&&link((path/"reply.spool").c_str(),(path/"second-name").c_str())==0,"extra hardlink source");reject_changed(path,*value.value);
    }
    {
        const auto path=job(L"symlink-file");const auto target=existing_job/"reply.spool";
        check(symlink(target.c_str(),(path/"reply.spool").c_str())==0,"synthetic symlink");
        check(!sfx::apps::runtime::seal_linux_qwk_reply_spool(path,packet,512,4096),"factory refuses a symlink without following it");
        const auto alias=root/"symlink-job";check(symlink(existing_job.c_str(),alias.c_str())==0,"directory symlink");
        check(!sfx::apps::runtime::seal_linux_qwk_reply_spool(alias,packet,512,4096),"factory refuses redirected parent");
    }
    {
        const auto path=job(L"fifo-file");check(mkfifo((path/"reply.spool").c_str(),0600)==0,"synthetic fifo");
        check(!sfx::apps::runtime::seal_linux_qwk_reply_spool(path,packet,512,4096),"factory refuses fifo without blocking");
    }
    {
        const auto path=job(L"public-job");check(chmod(path.c_str(),0755)==0,"exposed fixture directory");
        check(!sfx::apps::runtime::seal_linux_qwk_reply_spool(path,packet,512,4096)&&!std::filesystem::exists(path/"reply.spool"),"shared directory refused unchanged");
    }
    {
        const auto path=job(L"callback-once");unsigned callbacks{};auto lifetime=std::make_shared<int>(1);std::weak_ptr<int> weak=lifetime;
        auto value=sfx::apps::runtime::seal_linux_qwk_reply_spool(path,packet,512,4096,{},lifetime,-1,[&]{++callbacks;});
        check(static_cast<bool>(value),"confirmed cleanup source");lifetime.reset();check(!weak.expired(),"reader holds job lifetime");
        (*value.value)->confirm_completed();(*value.value)->confirm_completed();check(callbacks==1,"confirmation callback once");
        value.value.reset();check(weak.expired()&&!std::filesystem::exists(path/"reply.spool"),"confirmed file removed before final job lifetime release");
    }
    {
        const auto path=job(L"throwing-cancel");check(!sfx::apps::runtime::seal_linux_qwk_reply_spool(path,packet,512,4096,[]{throw std::runtime_error("cancelled");return false;})&&!std::filesystem::exists(path/"reply.spool"),"throwing cancellation fails closed before creation");
        const auto bounds=job(L"field-bound");auto invalid=packet;invalid.records[0].message.body.assign(app::kMessageBodyBytes+1,'x');
        check(!sfx::apps::runtime::seal_linux_qwk_reply_spool(bounds,invalid,512,app::kQwkExpandedBytes)&&!std::filesystem::exists(bounds/"reply.spool"),"individual field bounded before allocation and write");
    }
