/* Linux process/descriptor transport for the separate SFX SEXYZ helper.
 * November 8 2026. GPL-2.0-or-later; inherited notices remain in this tree.
 * No SFX libraries are linked into this executable. */
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <sys/un.h>
#include <unistd.h>
#include <limits.h>
#include "sfx_message_profile.h"
#include "crc16.h"
#include "crc32.h"
extern int zmodem_recv_header(zmodem_t*);

typedef struct {
    int socket;
    uint64_t start, inbound, input_bytes, output_bytes;
    BOOL cancelled, record_initial, initial_overflow;
    unsigned initial_size;
    BYTE initial[256];
    void* files;
} sfx_channel;

static uint64_t sfx_milliseconds(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC,&value))return UINT64_MAX;
    return (uint64_t)value.tv_sec*1000+(uint64_t)value.tv_nsec/1000000;
}
static BOOL sfx_cancelled(void* context)
{
    sfx_channel* ch=context;
    uint64_t now=sfx_milliseconds();
    if(now==UINT64_MAX||now<ch->start||now<ch->inbound||
       now-ch->start>=300000||now-ch->inbound>=60000)ch->cancelled=TRUE;
    return ch->cancelled;
}
static BOOL sfx_connected(void* context){return !sfx_cancelled(context);}
/* poll works for inherited descriptors above FD_SETSIZE; EINTR never renews
 * either deadline. HUP may still carry the peer's final buffered bytes. */
static int sfx_poll(sfx_channel* ch,BOOL sending,int milliseconds)
{
    struct pollfd item={ch->socket,sending?POLLOUT:POLLIN,0};
    int status=poll(&item,1,milliseconds);
    if(status<0&&errno==EINTR)return 0;
    if(status<0||(item.revents&(POLLERR|POLLNVAL))){ch->cancelled=TRUE;return -1;}
    return status>0&&(item.revents&(item.events|POLLHUP))?1:0;
}
static int sfx_socket_byte(sfx_channel* ch,BYTE* byte,BOOL sending,unsigned seconds)
{
    uint64_t end=sfx_milliseconds()+(uint64_t)seconds*1000;
    for(;;){
        int ready;ssize_t n;
        if(sfx_cancelled(ch))return -1;
        ready=sfx_poll(ch,sending,seconds?100:0);
        if(ready<0)return -1;
        if(ready){
            n=sending?send(ch->socket,byte,1,MSG_NOSIGNAL):recv(ch->socket,byte,1,0);
            if(n==1){
                uint64_t* counter=sending?&ch->output_bytes:&ch->input_bytes;
                if(*counter==UINT64_MAX){ch->cancelled=TRUE;return -1;}
                ++*counter;
                if(!sending){
                    if(ch->record_initial){
                        if(ch->initial_size<sizeof(ch->initial))ch->initial[ch->initial_size++]=*byte;
                        else ch->initial_overflow=TRUE;
                    }
                    ch->inbound=sfx_milliseconds();
                }
                return sending?0:*byte;
            }
            if(n==0||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)){
                ch->cancelled=TRUE;return -1;
            }
        }
        if(sfx_milliseconds()>=end)return -1;
    }
}
static int sfx_send(void* context,BYTE byte,unsigned timeout)
{
    sfx_channel* ch=context;int status=sfx_socket_byte(ch,&byte,TRUE,timeout);
    if(status<0)ch->cancelled=TRUE;
    return status;
}
static int sfx_recv(void* context,unsigned timeout)
{BYTE byte=0;return sfx_socket_byte(context,&byte,FALSE,timeout);}
static BOOL sfx_waiting(void* context,unsigned timeout)
{sfx_channel* ch=context;(void)timeout;return !sfx_cancelled(ch)&&sfx_poll(ch,FALSE,0)>0;}
static BOOL sfx_packet_send_waiting(void* context,unsigned timeout)
{
    sfx_channel* ch=context;uint64_t end=sfx_milliseconds()+(uint64_t)timeout*1000;
    for(;;){
        uint64_t now,remaining;int status;
        if(sfx_cancelled(ch))return FALSE;
        now=sfx_milliseconds();remaining=now<end?end-now:0;
        status=sfx_poll(ch,FALSE,(int)(remaining>100?100:remaining));
        if(status)return status>0;
        if(sfx_milliseconds()>=end)return FALSE;
    }
}
static BOOL sfx_private_file(int fd,struct stat* info)
{
    return fstat(fd,info)==0&&S_ISREG(info->st_mode)&&info->st_uid==geteuid()&&
        info->st_nlink==1&&(info->st_mode&0777)==0600;
}
static FILE* sfx_new_file(const char* name)
{
    int fd=open(name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    struct stat info;FILE* file;
    if(fd<0)return NULL;
    if(!sfx_private_file(fd,&info)){close(fd);return NULL;}
    file=fdopen(fd,"wb");if(!file)close(fd);return file;
}
static BOOL sfx_close_file(FILE* file)
{
    BOOL ok=!ferror(file);
    if(fflush(file)||fsync(fileno(file)))ok=FALSE;
    if(fclose(file))ok=FALSE;
    return ok;
}
static int sfx_private_channel(int descriptor)
{
    struct sockaddr_un peer;struct ucred owner;
    socklen_t size=sizeof(peer),owner_size=sizeof(owner),type_size=sizeof(int);
    int type=0,flags;
    if(getpeername(descriptor,(struct sockaddr*)&peer,&size)||peer.sun_family!=AF_UNIX||
       getsockopt(descriptor,SOL_SOCKET,SO_TYPE,&type,&type_size)||type!=SOCK_STREAM||
       getsockopt(descriptor,SOL_SOCKET,SO_PEERCRED,&owner,&owner_size)||
       owner_size!=sizeof(owner)||owner.uid!=geteuid()||owner.pid!=getppid())return 0;
    flags=fcntl(descriptor,F_GETFL);
    return flags>=0&&fcntl(descriptor,F_SETFL,flags|O_NONBLOCK)==0;
}
static int sfx_private_process(void)
{
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct caps[2]={{0}};
    /* The supervisor must launch with no capability. Refuse accidental
     * privileged inheritance instead of silently exercising that authority. */
    if(geteuid()==0||getuid()!=geteuid()||getgid()!=getegid()||
       syscall(SYS_capget,&header,caps)||caps[0].effective||caps[1].effective||
       caps[0].permitted||caps[1].permitted||caps[0].inheritable||caps[1].inheritable||
       prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))return 0;
    return signal(SIGPIPE,SIG_IGN)!=SIG_ERR;
}
