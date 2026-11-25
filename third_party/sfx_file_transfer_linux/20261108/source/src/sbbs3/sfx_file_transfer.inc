/* Section9 general-file executable only, October17 2026, GPL-2.0-or-later.
 * Derived from the separately distributed accepted packet helper; no SFX link.
 * No payload paths, generic CLI, INI, environment discovery, resume or overwrite.
 * The trusted host journals/creates/locks every payload BEFORE launch and
 * explicitly inherits only these handles. This profile never creates payloads.
 * Fixed job.cfg/result.txt are confined, host-owned per-job control material.
 */
#ifdef __linux__
#define SFX_FILE_COUNT 256u
#define SFX_FILE_MAX 1073741824ULL
#define SFX_FILE_RESERVE (64ULL*1024*1024)
typedef struct {
    int handle;
    uint64_t cap, bytes;
    char name[129];
    unsigned offered, completed, skipped, zeof;
} sfx_file_slot;
typedef struct {
    zmodem_t* zm;
    sfx_file_slot slot[SFX_FILE_COUNT];
    unsigned count, refusals;
    int selected;
    /* Fixed PRIVATE IPC buffers; not ZMODEM credit, payload storage or a new
       transfer queue. Wire protocol/caps/receipt grammar remain unchanged. */
    BYTE input[4096], output[4096];
    unsigned input_at, input_size, output_size;
    BOOL sending;
} sfx_file_job;

static BOOL sfx_file_flush_bytes(sfx_channel* ch,unsigned timeout)
{
    sfx_file_job* job=(sfx_file_job*)ch->files;
    uint64_t end=sfx_milliseconds()+(uint64_t)timeout*1000;
    unsigned at=0;
    while(at<job->output_size){
        int ready;ssize_t n;
        if(sfx_cancelled(ch))return FALSE;
        ready=sfx_poll(ch,TRUE,timeout?100:0);
        if(ready<0){ch->cancelled=TRUE;return FALSE;}
        if(ready){
            n=send(ch->socket,(const char*)job->output+at,(int)(job->output_size-at),MSG_NOSIGNAL);
            if(n>0){
                if(ch->output_bytes>UINT64_MAX-(unsigned)n){ch->cancelled=TRUE;return FALSE;}
                ch->output_bytes+=(unsigned)n;at+=(unsigned)n;continue;
            }
            if(n==0||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)){ch->cancelled=TRUE;return FALSE;}
        }
        if(sfx_milliseconds()>=end){ch->cancelled=TRUE;return FALSE;}
    }
    job->output_size=0;return TRUE;
}
static void sfx_file_flush(void* argument)
{
    zmodem_t* zm=(zmodem_t*)argument;
    (void)sfx_file_flush_bytes((sfx_channel*)zm->cbdata,zm->send_timeout);
}
static int sfx_file_send(void* context,BYTE byte,unsigned timeout)
{
    sfx_channel* ch=(sfx_channel*)context;sfx_file_job* job=(sfx_file_job*)ch->files;
    if(sfx_cancelled(ch))return -1;
    if(job->output_size==sizeof(job->output)&&!sfx_file_flush_bytes(ch,timeout))return -1;
    job->output[job->output_size++]=byte;return 0;
}
static int sfx_file_recv(void* context,unsigned timeout)
{
    sfx_channel* ch=(sfx_channel*)context;sfx_file_job* job=(sfx_file_job*)ch->files;
    uint64_t end=sfx_milliseconds()+(uint64_t)timeout*1000;BYTE byte;
    /* Flush before ANY read/wait, including header/ACK boundaries. Prefetched
       bytes count only when consumed; the host's forwarded-input/tail proof
       still rejects unexpected trailing data. Never drain or discard it. */
    if(sfx_cancelled(ch)||!sfx_file_flush_bytes(ch,job->zm->send_timeout))return -1;
    while(job->input_at==job->input_size){
        int ready;ssize_t n;
        if(sfx_cancelled(ch))return -1;
        ready=sfx_poll(ch,FALSE,timeout?100:0);
        if(ready<0){ch->cancelled=TRUE;return -1;}
        if(ready){
            n=recv(ch->socket,(char*)job->input,sizeof(job->input),0);
            if(n>0){job->input_at=0;job->input_size=(unsigned)n;break;}
            if(n==0||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)){ch->cancelled=TRUE;return -1;}
        }
        if(sfx_milliseconds()>=end)return -1;
    }
    byte=job->input[job->input_at++];
    if(ch->input_bytes==UINT64_MAX){ch->cancelled=TRUE;return -1;}
    ++ch->input_bytes;
    if(ch->record_initial){
        if(ch->initial_size<sizeof(ch->initial))ch->initial[ch->initial_size++]=byte;
        else ch->initial_overflow=TRUE;
    }
    ch->inbound=sfx_milliseconds();return byte;
}
static BOOL sfx_file_waiting(void* context,unsigned timeout)
{
    sfx_channel* ch=(sfx_channel*)context;sfx_file_job* job=(sfx_file_job*)ch->files;
    if(sfx_cancelled(ch)||!sfx_file_flush_bytes(ch,job->zm->send_timeout))return FALSE;
    if(job->input_at<job->input_size)return TRUE;
    return job->sending?sfx_packet_send_waiting(context,timeout):sfx_waiting(context,timeout);
}

static int sfx_file_number(const char** cursor,uint64_t* value)
{
    const char* p=*cursor;unsigned digits=0;uint64_t n=0;
    while(*p>='0'&&*p<='9'){
        unsigned d=(unsigned)(*p++-'0');if(++digits>20||n>(UINT64_MAX-d)/10)return 0;
        n=n*10+d;
    }
    if(!digits)return 0;*cursor=p;*value=n;return 1;
}
static int sfx_file_line(FILE* fp,char* line,size_t capacity)
{
    size_t n;
    if(!fgets(line,(int)capacity,fp))return 0;
    n=strlen(line);if(!n||line[n-1]!='\n')return 0;line[--n]=0;
    if(n&&line[n-1]=='\r')line[--n]=0;
    return n>0;
}
static int sfx_file_hex(char ch)
{
    return ch>='0'&&ch<='9'?ch-'0':ch>='A'&&ch<='F'?ch-'A'+10:-1;
}
static int sfx_file_same_name(const char* left,const char* right)
{
    while(*left&&*right){
        unsigned a=(unsigned char)*left++,b=(unsigned char)*right++;
        if(a>='a'&&a<='z')a-=32;if(b>='a'&&b<='z')b-=32;
        if(a!=b)return 0;
    }return !*left&&!*right;
}
static int sfx_file_load(sfx_file_job* job,BOOL sending)
{
    struct stat info;
    int fd;FILE* fp;char line[512];const char* p;uint64_t value;unsigned i,j;BOOL ok=FALSE;
    fd=open("job.cfg",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return 0;
    if(!sfx_private_file(fd,&info)||info.st_size<1||info.st_size>131072){close(fd);return 0;}
    fp=fdopen(fd,"rb");if(!fp){close(fd);return 0;}
    if(!sfx_file_line(fp,line,sizeof(line))||strcmp(line,sending?"SFX-FILE-SEND 1":"SFX-FILE-RECEIVE 1"))goto done;
    if(!sfx_file_line(fp,line,sizeof(line)))goto done;p=line;
    if(!sfx_file_number(&p,&value)||*p||!value||value>SFX_FILE_COUNT)goto done;
    job->count=(unsigned)value;
    for(i=0;i<job->count;++i){
        sfx_file_slot* slot=&job->slot[i];uint64_t handle;size_t n=0;
        if(!sfx_file_line(fp,line,sizeof(line)))goto done;p=line;
        if(!sfx_file_number(&p,&handle)||handle<3||handle>INT_MAX||*p++!=' ')goto done;
        if(!sfx_file_number(&p,&slot->cap)||!slot->cap||slot->cap>SFX_FILE_MAX||*p++!=' ')goto done;
        while(*p){
            int a,b;if(n>=128||!p[1]||(a=sfx_file_hex(p[0]))<0||(b=sfx_file_hex(p[1]))<0)goto done;
            slot->name[n++]=(char)((a<<4)|b);p+=2;
        }
        if(!sfx_filename((const unsigned char*)slot->name,n))goto done;
        slot->name[n]=0;slot->handle=(int)handle;
        for(j=0;j<i;++j)if(job->slot[j].handle==slot->handle||sfx_file_same_name(job->slot[j].name,slot->name))goto done;
    }
    if(fgetc(fp)!=EOF||ferror(fp))goto done;ok=TRUE;
done:
    if(fclose(fp)!=0)ok=FALSE;return ok;
}
static BOOL sfx_file_descriptor(sfx_file_slot* slot,BOOL sending,struct stat* info)
{
    int flags=fcntl(slot->handle,F_GETFL);
    return flags>=0&&(flags&O_ACCMODE)==(sending?O_RDONLY:O_WRONLY)&&!(flags&O_APPEND)&&
       fstat(slot->handle,info)==0&&S_ISREG(info->st_mode)&&info->st_uid==geteuid()&&
       info->st_nlink==1&&((info->st_mode&0777)==0600||(sending&&(info->st_mode&0777)==0400))&&info->st_size>=0&&
       (uint64_t)info->st_size<=slot->cap&&(sending||info->st_size==0);
}
static FILE* sfx_file_open(sfx_file_slot* slot,BOOL sending,uint64_t* length)
{
    struct stat info;FILE* file;
    if(!sfx_file_descriptor(slot,sending,&info)||
       lseek(slot->handle,0,SEEK_SET)!=0)return NULL;
    *length=(uint64_t)info.st_size;
    file=fdopen(slot->handle,sending?"rb":"wb");
    if(!file)return NULL;
    slot->handle=-1;
    if(setvbuf(file,NULL,_IONBF,0)){fclose(file);return NULL;}
    return file;
}
static BOOL sfx_file_space(void* context,FILE* file,unsigned count)
{
    struct statvfs space;uint64_t available;(void)context;
    if(fstatvfs(fileno(file),&space)||!space.f_frsize||
       space.f_bavail>UINT64_MAX/space.f_frsize)return FALSE;
    available=(uint64_t)space.f_bavail*space.f_frsize;
    return available>=SFX_FILE_RESERVE&&(uint64_t)count<=available-SFX_FILE_RESERVE;
}
static unsigned sfx_file_offer(void* context,const char* name,uint64_t bytes)
{
    sfx_channel* channel=(sfx_channel*)context;sfx_file_job* job=(sfx_file_job*)channel->files;
    unsigned i;job->selected=-1;
    for(i=0;i<job->count;++i){
        sfx_file_slot* slot=&job->slot[i];
        if(!sfx_file_same_name(name,slot->name))continue;
        if(slot->offered){++job->refusals;return 3;}
        slot->offered=1;job->selected=(int)i;job->zm->sfx_file_cap=slot->cap;
        if(bytes>slot->cap){slot->skipped=2;++job->refusals;return 2;}
        return 0;
    }
    ++job->refusals;return 3; /* no host-created destination for this name */
}
#include "sfx_xy_transfer.inc"
static int sfx_file_transfer_main(int argc,char** argv,BOOL sending)
{
    sfx_channel channel={0};zmodem_t strict;sfx_file_job job={0};
    int type,directory;struct stat directory_info;
    const char* p;uint64_t descriptor;FILE* result=NULL;unsigned i;
    BOOL batch=FALSE;char protocol='Z';
    if(argc==4&&argv[3][0]&&!argv[3][1]&&(argv[3][0]=='X'||argv[3][0]=='Y'||argv[3][0]=='G'))protocol=argv[3][0];
    else if(argc!=3)return SFX_LOCAL_IO;
    p=argv[2];
    if(!sfx_file_number(&p,&descriptor)||*p||descriptor<3||descriptor>INT_MAX)return SFX_LOCAL_IO;
    channel.socket=(int)descriptor;
    if(!sfx_private_channel(channel.socket))return SFX_LOCAL_IO;
    directory=open(".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(directory<0)return SFX_LOCAL_IO;
    if(fstat(directory,&directory_info)||!S_ISDIR(directory_info.st_mode)||
       directory_info.st_uid!=geteuid()||(directory_info.st_mode&0777)!=0700){close(directory);return SFX_LOCAL_IO;}
    if(!sfx_file_load(&job,sending)||(protocol=='X'&&job.count!=1)){close(directory);return SFX_LOCAL_IO;}
    /* Validate inherited capabilities BEFORE opening result.txt. Otherwise a
     * nonexistent numeric descriptor could alias that newly opened receipt.
     * Distinct descriptors must also name distinct payload objects. */
    {
        struct stat objects[SFX_FILE_COUNT];unsigned j;
        for(i=0;i<job.count;++i){
            if(job.slot[i].handle==channel.socket||job.slot[i].handle==directory||
               !sfx_file_descriptor(&job.slot[i],sending,&objects[i])){close(directory);return SFX_LOCAL_IO;}
            for(j=0;j<i;++j)if(objects[i].st_dev==objects[j].st_dev&&objects[i].st_ino==objects[j].st_ino){close(directory);return SFX_LOCAL_IO;}
        }
    }
    result=sfx_new_file("result.txt");if(!result){close(directory);return SFX_LOCAL_IO;}
    channel.start=channel.inbound=sfx_milliseconds();channel.files=&job;job.zm=&strict;job.selected=-1;job.sending=sending;
    zmodem_init(&strict,&channel,NULL,NULL,sfx_file_send,sfx_file_recv,sfx_connected,sfx_cancelled,sfx_file_waiting,sfx_file_flush);
    strict.sfx_message=TRUE;strict.sfx_files=TRUE;strict.sfx_packet_send=sending;
    strict.sfx_offer_limit=SFX_FILE_COUNT;strict.sfx_file_offer=sfx_file_offer;strict.sfx_write_allowed=sfx_file_space;
    strict.max_file_size=SFX_FILE_MAX;strict.max_errors=8;strict.recv_timeout=10;strict.send_timeout=2;strict.escape_telnet_iac=FALSE;
    strict.max_window_size=16384;strict.target_window_size=0;
    /* A finite receive frame lets senders respect the bounded decoded/SSH
       pipeline instead of interpreting transient SSH window pressure as EOF.
       8 KiB payload remains below half the 64 KiB SSH window even with every
       octet escaped. This is standard ZRINIT credit, not a larger host buffer.
       Send behavior and all receipt/size/cancellation checks are unchanged. */
    if(!sending)strict.recv_bufsize=8192;
    for(i=0;i<job.count;++i)if(job.slot[i].cap>strict.sfx_file_cap)strict.sfx_file_cap=job.slot[i].cap;
    if(protocol!='Z'){
        batch=sending?sfx_xy_send_job(&channel,&job,protocol):sfx_xy_receive_job(&channel,&job,protocol);
        if(!batch)sfx_fail(&strict,SFX_COMPLETION);
    }else if(sending){
        for(i=0;i<job.count&&!strict.sfx_failure&&!sfx_cancelled(&channel);++i){
            sfx_file_slot* slot=&job.slot[i];uint64_t length=0,sent=0;FILE* file=sfx_file_open(slot,TRUE,&length);
            if(!file){sfx_fail(&strict,SFX_LOCAL_IO);break;}
            slot->offered=1;++strict.sfx_attempted;strict.sfx_sent_zeof=FALSE;strict.file_skipped=FALSE;
            strict.files_remaining=job.count-i;strict.total_files=job.count;
            strict.bytes_remaining=strict.total_bytes=length;strict.sfx_file_cap=slot->cap;
            if(zmodem_send_file(&strict,slot->name,file,i==0,NULL,&sent)&&!strict.sfx_failure&&!sfx_cancelled(&channel)){
                if(strict.file_skipped)slot->skipped=1;
                else if(strict.sfx_sent_zeof&&strict.current_file_pos==(int64_t)length&&!ferror(file)){
                    slot->completed=slot->zeof=1;slot->bytes=length;
                }else sfx_fail(&strict,SFX_COMPLETION);
            }else sfx_fail(&strict,SFX_COMPLETION);
            if(fclose(file)!=0)sfx_fail(&strict,SFX_DISK);
        }
        if(!strict.sfx_failure)batch=sfx_packet_send_finish(&strict);
    }else{
        channel.record_initial=TRUE;type=zmodem_recv_init(&strict);channel.record_initial=FALSE;
        if(type==ZFIN&&!strict.sfx_attempted&&!sfx_empty_prefix(&strict,&channel))sfx_fail(&strict,SFX_COMPLETION);
        while(!strict.sfx_failure&&!sfx_cancelled(&channel)){
            if(type==ZFIN){
                int first=sfx_file_recv(&channel,2),second=sfx_file_recv(&channel,2);
                batch=first=='O'&&second=='O'&&!sfx_cancelled(&channel);break;
            }
            if(type!=ZFILE){sfx_fail(&strict,SFX_PROTOCOL);break;}
            if(strict.sfx_refusal){
                if(zmodem_send_zskip(&strict)!=0){sfx_fail(&strict,SFX_PROTOCOL);break;}
            }else{
                sfx_file_slot* slot;FILE* file;uint64_t length=0;unsigned errors;int64_t actual;
                if(job.selected<0||(unsigned)job.selected>=job.count){sfx_fail(&strict,SFX_LOCAL_IO);break;}
                slot=&job.slot[job.selected];file=sfx_file_open(slot,FALSE,&length);strict.sfx_zeof=FALSE;
                if(!file){sfx_fail(&strict,SFX_DISK);break;}
                errors=zmodem_recv_file_data(&strict,file,0);actual=ftello(file);
                if(errors<=strict.max_errors&&strict.sfx_zeof&&!strict.sfx_failure&&actual>=0&&(uint64_t)actual<=slot->cap){
                    slot->completed=slot->zeof=1;slot->bytes=(uint64_t)actual;
                }else sfx_fail(&strict,SFX_COMPLETION);
                if(!sfx_close_file(file))sfx_fail(&strict,SFX_DISK);
            }
            if(strict.sfx_failure)break;
            strict.sfx_refusal=0;strict.sfx_zeof=FALSE;type=zmodem_recv_init(&strict);
        }
    }
    if(!sfx_file_flush_bytes(&channel,strict.send_timeout))sfx_fail(&strict,SFX_LOCAL_IO);
    if(sfx_cancelled(&channel)||strict.cancelled||strict.local_abort)sfx_fail(&strict,SFX_CANCEL);
    if(!batch)sfx_fail(&strict,SFX_COMPLETION);
    if(strict.sfx_failure&&!channel.cancelled){
        if(protocol=='Z')zmodem_send_zabort(&strict);
        else for(i=0;i<8;++i)(void)sfx_xy_byte(&channel,CAN);
    }
    (void)sfx_file_flush_bytes(&channel,strict.send_timeout);
    {
        BOOL ok=TRUE;
        if(protocol=='Z'){if(fputs("SFX-FILE-TRANSFER 1\n",result)<0)ok=FALSE;}
        else if(fprintf(result,"SFX-FILE-TRANSFER 2\nprotocol %c\n",protocol)<0)ok=FALSE;
        if(fprintf(result,"direction %c\ncount %u\nattempted %u\nrefused %u\nbatch %u\nfailure %u\ninput %" PRIu64 "\noutput %" PRIu64 "\n",
            sending?'S':'R',job.count,strict.sfx_attempted,job.refusals,batch?1:0,strict.sfx_failure,channel.input_bytes,channel.output_bytes)<0)ok=FALSE;
        for(i=0;i<job.count;++i){sfx_file_slot* slot=&job.slot[i];
            if(fprintf(result,"file %u %u %u %u %" PRIu64 " %u\n",i,slot->offered,slot->completed,slot->skipped,slot->bytes,slot->zeof)<0)ok=FALSE;
        }
        if(!sfx_close_file(result))ok=FALSE;if(!ok)sfx_fail(&strict,SFX_LOCAL_IO);
    }
    if(fsync(directory))sfx_fail(&strict,SFX_LOCAL_IO);
    close(directory);close(channel.socket);
    return strict.sfx_failure?(int)strict.sfx_failure:0;
}
#endif
