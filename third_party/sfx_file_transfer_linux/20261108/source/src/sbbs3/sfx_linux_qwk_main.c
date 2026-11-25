/* SFX Linux QWK packet helper, GPL-2.0-or-later.
 * Separate executable adapted from the accepted SFX packet SEXYZ fork.
 * Copyright Rob Swindell and contributors. Retain all inherited notices.
 * Fixed host-created private cwd; caller filenames remain metadata only. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "genwrap.h"
#include "dirwrap.h"
#include "sexyz.h"
#include "git_hash.h"
#include "sfx_linux_platform.inc"
#include "sfx_linux_completion.inc"
static int packet_directory(void)
{
    int fd=open(".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat info;
    if(fd<0)return -1;
    if(fstat(fd,&info)||!S_ISDIR(info.st_mode)||info.st_uid!=geteuid()||(info.st_mode&07777)!=0700){close(fd);return -1;}
    return fd;
}
static int packet_channel(const char* text)
{
    unsigned value=0;size_t i;
    if(!text[0])return -1;
    for(i=0;text[i];++i){unsigned digit=(unsigned char)text[i]-'0';if(digit>9||i>=10||value>((unsigned)INT_MAX-digit)/10)return -1;value=value*10+digit;}
    return value>=3&&sfx_private_channel((int)value)?(int)value:-1;
}
static FILE* packet_payload(int64_t* size)
{
    int fd=open("payload.qwk",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    struct stat info;FILE* fp;
    if(fd<0)return NULL;
    if(fstat(fd,&info)||!S_ISREG(info.st_mode)||info.st_uid!=geteuid()||info.st_nlink!=1||
       ((info.st_mode&07777)!=0400&&(info.st_mode&07777)!=0600)||info.st_size<1||info.st_size>SFX_MESSAGE_CAP){close(fd);return NULL;}
    *size=info.st_size;fp=fdopen(fd,"rb");if(!fp)close(fd);return fp;
}
static int packet_receive(int argc,char** argv)
{
    sfx_channel ch={0};zmodem_t strict;
    int directory,type;FILE *payload=NULL,*result=NULL,*dsz=NULL;
    BOOL completed=FALSE,batch=FALSE,empty=FALSE;int64_t actual=0;
    if(argc!=3||(ch.socket=packet_channel(argv[2]))<0)return SFX_LOCAL_IO;
    directory=packet_directory();if(directory<0)return SFX_LOCAL_IO;
    result=sfx_new_file("result.txt");dsz=sfx_new_file("dsz.log");
    if(!result||!dsz){if(result)fclose(result);if(dsz)fclose(dsz);close(directory);return SFX_LOCAL_IO;}
    ch.start=ch.inbound=sfx_milliseconds();
    zmodem_init(&strict,&ch,NULL,NULL,sfx_send,sfx_recv,sfx_connected,sfx_cancelled,sfx_waiting,NULL);
    strict.sfx_message=TRUE;
    strict.max_file_size=SFX_MESSAGE_CAP;
    strict.max_errors=8;
    strict.recv_timeout=10;
    strict.send_timeout=2;
    strict.escape_telnet_iac=FALSE;
    ch.record_initial=TRUE;
    type=zmodem_recv_init(&strict);
    ch.record_initial=FALSE;
    if (type==ZFIN && !strict.sfx_failure && !strict.sfx_attempted && !strict.sfx_refusal
        && !strict.sfx_zeof && !sfx_cancelled(&ch) && sfx_empty_prefix(&strict,&ch)) {
        /* recv_init has already emitted the full ZFIN acknowledgement. Only
         * exact literal OO can complete this no-file cancellation; the host
         * must verify output/input ledgers, files, authority and deadlines. */
        int first=sfx_recv(&ch,2), second=sfx_recv(&ch,2);
        if (first=='O' && second=='O' && !sfx_cancelled(&ch)) batch=empty=TRUE;
    } else if (type==ZFILE && !strict.sfx_failure && strict.sfx_refusal && !sfx_cancelled(&ch)) {
        /* Decline the one offered file before ZRPOS or payload creation. A
         * cooperative sender ends the batch with ZFIN/OO after ZSKIP. No
         * success on cancellation, extra files, incoming data or silence. */
        if (zmodem_send_zskip(&strict)==0) {
            if (sfx_refusal_zfin(&strict) && zmodem_send_zfin(&strict)==0) {
                int first=sfx_recv(&ch,2), second=sfx_recv(&ch,2);
                if (first=='O' && second=='O' && !sfx_cancelled(&ch)) batch=TRUE;
            }
        }
    } else if (type==ZFILE && !strict.sfx_failure && !sfx_cancelled(&ch)) {
        payload=sfx_new_file("payload.rep");
        if (!payload) sfx_fail(&strict,SFX_DISK);
        else {
            unsigned errors=zmodem_recv_file_data(&strict,payload,0);
            actual=ftello(payload);
            completed=errors<=strict.max_errors && strict.sfx_zeof && !strict.sfx_failure;
            if (!sfx_close_file(payload)) { completed=FALSE; sfx_fail(&strict,SFX_DISK); }
            payload=NULL;
            if (completed && !sfx_cancelled(&ch)) {
                type=zmodem_recv_init(&strict);
                if (type==ZFIN && !strict.sfx_failure) {
                    /* The normal ZFIN handshake ends with literal "OO". No
                     * success on ZFIN in place of ZEOF, missing batch end,
                     * disconnect, missing/partial OO or second ZFILE. */
                    int first=sfx_recv(&ch,2), second=sfx_recv(&ch,2);
                    if (first=='O' && second=='O' && !sfx_cancelled(&ch)) batch=TRUE;
                }
            }
        }
    }
    if (sfx_cancelled(&ch) || strict.cancelled || strict.local_abort) sfx_fail(&strict,SFX_CANCEL);
    if (empty) {
        if (!batch || completed || actual || strict.sfx_zeof || strict.sfx_attempted || strict.sfx_refusal)
            sfx_fail(&strict,SFX_COMPLETION);
    } else if (strict.sfx_refusal) {
        if (!batch || completed || actual || strict.sfx_zeof || strict.sfx_attempted!=1)
            sfx_fail(&strict,SFX_COMPLETION);
    } else if (!completed || !batch || actual<1 || actual>SFX_MESSAGE_CAP) sfx_fail(&strict,SFX_COMPLETION);
    if (strict.sfx_failure && !ch.cancelled) zmodem_send_zabort(&strict);
    if (fprintf(dsz,"%c %" PRId64 " 0 0 0 0 0 payload.rep\n",
                strict.sfx_failure?'E':((strict.sfx_refusal || empty)?'S':'Z'),actual)<0) sfx_fail(&strict,SFX_LOCAL_IO);
    if (!sfx_close_file(dsz)) sfx_fail(&strict,SFX_LOCAL_IO);
    {
    BOOL result_ok=TRUE;
    /* Version4 additionally distinguishes a completed empty batch (reason3,
     * attempted0) from a policy refusal or a completed file. It accounts
     * for bytes consumed through the strict OO
     * boundary, not bytes merely buffered in TCP. The host must bind this to
     * its own forwarding ledger before classifying any close/reset as safe.
     * No tail is read, purged or interpreted here; all strict guards remain. */
    if (fprintf(result,"SFX-PACKET-RECEIVE 1\nattempted %u\ncompleted %u\nbytes %" PRId64
                       "\nzeof %u\nbatch %u\nfailure %u\nrefusal %u\ninput %" PRIu64 "\noutput %" PRIu64 "\n",
                strict.sfx_attempted,completed?1:0,actual,strict.sfx_zeof?1:0,batch?1:0,
                strict.sfx_failure,empty?3:strict.sfx_refusal,ch.input_bytes,ch.output_bytes)<0) result_ok=FALSE;
    if (!sfx_close_file(result)) result_ok=FALSE;
    if (!result_ok) {
        close(directory); return SFX_LOCAL_IO;
    }
    }
    close(directory);
    close(ch.socket);
    return strict.sfx_failure ? (int)strict.sfx_failure : 0;
}
static int packet_send(int argc,char** argv)
{
    sfx_channel ch={0};zmodem_t strict;size_t n;int directory;
    FILE *payload=NULL,*result=NULL,*dsz=NULL;int64_t offered=0,actual=0;uint64_t sent=0;
    BOOL completed=FALSE,batch=FALSE;char filename[13];
    if(argc!=4||(ch.socket=packet_channel(argv[2]))<0)return SFX_LOCAL_IO;
    n=strlen(argv[3]);if(!n||n>8||argv[3][0]<'A'||argv[3][0]>'Z')return SFX_LOCAL_IO;
    memcpy(filename,argv[3],n);memcpy(filename+n,".QWK",5);if(!sfx_filename((const unsigned char*)filename,n+4))return SFX_LOCAL_IO;
    directory=packet_directory();if(directory<0)return SFX_LOCAL_IO;
    payload=packet_payload(&offered);result=sfx_new_file("result.txt");dsz=sfx_new_file("dsz.log");
    if(!payload||!result||!dsz){if(payload)fclose(payload);if(result)fclose(result);if(dsz)fclose(dsz);close(directory);return SFX_LOCAL_IO;}
    ch.start=ch.inbound=sfx_milliseconds();
    zmodem_init(&strict,&ch,NULL,NULL,sfx_send,sfx_recv,sfx_connected,sfx_cancelled,sfx_packet_send_waiting,NULL);
    strict.sfx_message=TRUE;strict.sfx_packet_send=TRUE;strict.sfx_attempted=1;
    strict.max_file_size=SFX_MESSAGE_CAP;strict.max_errors=8;strict.recv_timeout=10;strict.send_timeout=2;strict.escape_telnet_iac=FALSE;
    strict.max_window_size=16384;strict.target_window_size=0; /* fixed bounded ACK window, no dynamic growth */
    strict.files_remaining=strict.total_files=1;strict.bytes_remaining=strict.total_bytes=offered;
    if (zmodem_send_file(&strict,filename,payload,TRUE,NULL,&sent) && !strict.sfx_failure && !sfx_cancelled(&ch)) {
        if (strict.file_skipped)strict.sfx_refusal=4; /* explicitly no delivery/progress */
        else if (strict.sfx_sent_zeof && strict.current_file_pos==offered && !ferror(payload))completed=TRUE;
        else sfx_fail(&strict,SFX_COMPLETION);
        if (!strict.sfx_failure)batch=sfx_packet_send_finish(&strict);
    } else sfx_fail(&strict,SFX_COMPLETION);
    if (fclose(payload)!=0)sfx_fail(&strict,SFX_DISK);
    if (sfx_cancelled(&ch) || strict.cancelled || strict.local_abort)sfx_fail(&strict,SFX_CANCEL);
    if (!batch || (!completed && strict.sfx_refusal!=4))sfx_fail(&strict,SFX_COMPLETION);
    if (completed && !strict.sfx_failure)actual=offered;
    if (strict.sfx_failure && !ch.cancelled)zmodem_send_zabort(&strict);
    if (fprintf(dsz,"%c %" PRId64 " 0 0 0 0 0 payload.qwk\n",strict.sfx_failure?'E':(completed?'z':'S'),actual)<0)sfx_fail(&strict,SFX_LOCAL_IO);
    if (!sfx_close_file(dsz))sfx_fail(&strict,SFX_LOCAL_IO);
    {
        BOOL ok=TRUE;
        if (fprintf(result,"SFX-PACKET-SEND 1\nattempted 1\ncompleted %u\nbytes %" PRId64 "\nzeof %u\nbatch %u\nfailure %u\nrefusal %u\ninput %" PRIu64 "\noutput %" PRIu64 "\n",
            completed?1:0,actual,strict.sfx_sent_zeof?1:0,batch?1:0,strict.sfx_failure,strict.sfx_refusal,ch.input_bytes,ch.output_bytes)<0)ok=FALSE;
        if (!sfx_close_file(result))ok=FALSE;
        if (!ok){close(directory);return SFX_LOCAL_IO;}
    }
    close(directory);close(ch.socket);return strict.sfx_failure?(int)strict.sfx_failure:0;
}
int main(int argc,char** argv)
{
    if(argc==2&&!strcmp(argv[1],"--version")){printf("SFX Linux QWK packet SEXYZ fork: packet-v1 ZMODEM, upstream %s, GPL-2.0-or-later; Copyright Rob Swindell and contributors\n",GIT_HASH);return 0;}
    if(!sfx_private_process())return SFX_LOCAL_IO;
    if(argc==3&&!strcmp(argv[1],"--sfx-packet-receive-v1"))return packet_receive(argc,argv);
    if(argc==4&&!strcmp(argv[1],"--sfx-packet-send-v1"))return packet_send(argc,argv);
    return SFX_LOCAL_IO;
}
