/* Strict one-file entry point for the separate SEXYZ GPL-2.0-or-later helper.
 * Separate SFX PACKET receive fork October 13 2026; all upstream notices retained.
 * The original accepted 256 KiB helper/source is unchanged. Not SFX-linked.
 * The host must create/secure a unique private cwd and supervise this process.
 * No shell, caller paths, INI discovery, getenv, resume, overwrite or purge.
 * Runtime is responsible for session deadlines/revocation and Job containment.
 */
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <inttypes.h>
#include <ctype.h>
#include "sfx_message_profile.h"
#include "crc16.h"
#include "crc32.h"
extern int zmodem_recv_header(zmodem_t*);

typedef struct {
    SOCKET socket;
    ULONGLONG start, inbound;
    uint64_t input_bytes;
    uint64_t output_bytes;
    BOOL cancelled;
    /* Diagnostic replay proof for an empty first batch only. This never
     * changes ordinary file/header processing or reads additional input. */
    BOOL record_initial, initial_overflow;
    unsigned initial_size;
    BYTE initial[256];
    void* files; /* Section9 private job state, not a caller pointer. */
} sfx_channel;

static BOOL sfx_cancelled(void* ctx)
{
    sfx_channel* ch=(sfx_channel*)ctx;
    ULONGLONG now=GetTickCount64();
    if (now-ch->start>=300000 || now-ch->inbound>=60000) ch->cancelled=TRUE;
    return ch->cancelled;
}

static BOOL sfx_connected(void* ctx) { return !sfx_cancelled(ctx); }

static int sfx_socket_byte(sfx_channel* ch, BYTE* byte, BOOL sending, unsigned seconds)
{
    ULONGLONG end=GetTickCount64()+(ULONGLONG)seconds*1000;
    for (;;) {
        fd_set set;
        struct timeval wait={0,100000};
        int ready, n;
        if (sfx_cancelled(ch)) return -1;
        if (!seconds) wait.tv_usec=0;
        FD_ZERO(&set); FD_SET(ch->socket,&set);
        ready=select(0,sending?NULL:&set,sending?&set:NULL,NULL,&wait);
        if (ready==SOCKET_ERROR) { ch->cancelled=TRUE; return -1; }
        if (ready) {
            n=sending?send(ch->socket,(const char*)byte,1,0):recv(ch->socket,(char*)byte,1,0);
            if (n==1) {
                if (sending) {
                    if (ch->output_bytes==UINT64_MAX) { ch->cancelled=TRUE; return -1; }
                    ++ch->output_bytes;
                }
                if (!sending) {
                    if (ch->input_bytes==UINT64_MAX) { ch->cancelled=TRUE; return -1; }
                    ++ch->input_bytes;
                    if (ch->record_initial) {
                        if (ch->initial_size<sizeof(ch->initial)) ch->initial[ch->initial_size++]=*byte;
                        else ch->initial_overflow=TRUE;
                    }
                    ch->inbound=GetTickCount64();
                }
                return sending?0:*byte;
            }
            if (n==0 || WSAGetLastError()!=WSAEWOULDBLOCK) { ch->cancelled=TRUE; return -1; }
        }
        if (GetTickCount64()>=end) return -1;
    }
}
static int sfx_send(void* ctx, BYTE byte, unsigned timeout)
{
    int result=sfx_socket_byte((sfx_channel*)ctx,&byte,TRUE,timeout);
    if (result<0) ((sfx_channel*)ctx)->cancelled=TRUE;
    return result;
}
static int sfx_recv(void* ctx, unsigned timeout)
{
    BYTE byte=0;
    return sfx_socket_byte((sfx_channel*)ctx,&byte,FALSE,timeout);
}
static BOOL sfx_waiting(void* ctx, unsigned timeout)
{
    sfx_channel* ch=(sfx_channel*)ctx;
    fd_set set; struct timeval wait={0,0};
    (void)timeout;
    FD_ZERO(&set); FD_SET(ch->socket,&set);
    return !sfx_cancelled(ctx) && select(0,&set,NULL,NULL,&wait)>0;
}

/* Strict refusal completion cannot use the ordinary resynchronizing scanner.
 * Read exactly one header, with no scan/retry after an unexpected byte. The
 * fixed frame is at most 24 raw bytes before optional flow control. Bound the
 * entire parser to 32 raw bytes and four XON/XOFF bytes (including the ZCRCW
 * metadata's trailing XON). Parity XON/XOFF is legal outside an escape pair;
 * it never resets this budget. This local TCP profile needs no arbitrary
 * serial-line padding/noise recovery. Existing 60s/300s cancellation remains.
 * Binary16/32, hex16, one/two ZPAD, legal binary escapes and hex LF/CRLF with
 * parity terminators are supported. No bytes after the header are consumed.
 */
typedef struct {
    zmodem_t* zm; unsigned raw, flow;
    const BYTE* memory; unsigned size, pos;
} sfx_end_reader;
static int sfx_end_raw(sfx_end_reader* r)
{
    int c;
    if (r->raw>=32 || r->zm->sfx_failure) return -1;
    ++r->raw;
    if (r->memory) {
        if (r->pos>=r->size) { sfx_fail(r->zm,SFX_PROTOCOL); return -1; }
        return r->memory[r->pos++];
    }
    if (sfx_cancelled(r->zm->cbdata)) return -1;
    c=r->zm->recv_byte(r->zm->cbdata,10);
    if (c<0) sfx_fail(r->zm,SFX_PROTOCOL);
    return c;
}
static int sfx_end_byte(sfx_end_reader* r)
{
    int c;
    for (;;) {
        c=sfx_end_raw(r);
        if (c!=XON && c!=(XON|0x80) && c!=XOFF && c!=(XOFF|0x80)) return c;
        if (++r->flow>4) { sfx_fail(r->zm,SFX_PROTOCOL); return -1; }
    }
}
static int sfx_end_binary(sfx_end_reader* r)
{
    int c=sfx_end_byte(r);
    if (c!=ZDLE) return c;
    c=sfx_end_raw(r);
    if (c==ZRUB0) return 0x7f;
    if (c==ZRUB1) return 0xff;
    if (c>=0 && (c&0x60)==0x40) return c^0x40;
    sfx_fail(r->zm,SFX_PROTOCOL); return -1;
}
static int sfx_end_nibble(sfx_end_reader* r)
{
    int c=sfx_end_byte(r);
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    sfx_fail(r->zm,SFX_PROTOCOL); return -1;
}
static int sfx_end_header(sfx_end_reader* r)
{
    zmodem_t* zm=r->zm;
    unsigned char data[9];
    unsigned i, count;
    int c,style;
    uint16_t crc16=0;
    uint32_t crc32=0xffffffffU,received;
    if (sfx_end_byte(r)!=ZPAD) goto bad;
    c=sfx_end_byte(r);
    if (c==ZPAD) c=sfx_end_byte(r);
    if (c!=ZDLE) goto bad;
    style=sfx_end_byte(r);
    if (style!=ZHEX && style!=ZBIN && style!=ZBIN32) goto bad;
    count=style==ZBIN32?9:7;
    for (i=0;i<count;++i) {
        if (style==ZHEX) {
            int high=sfx_end_nibble(r),low=sfx_end_nibble(r);
            if (high<0 || low<0) goto bad;
            c=(high<<4)|low;
        } else c=sfx_end_binary(r);
        if (c<0) goto bad;
        data[i]=(unsigned char)c;
        if (i<5) { crc16=(uint16_t)ucrc16(c,crc16); crc32=ucrc32(c,crc32); }
    }
    if (style==ZBIN32) {
        received=(uint32_t)data[5]|((uint32_t)data[6]<<8)|((uint32_t)data[7]<<16)|((uint32_t)data[8]<<24);
        if (received!=~crc32) goto bad;
    } else if (((unsigned)data[5]<<8|data[6])!=crc16) goto bad;
    if (style==ZHEX) {
        c=sfx_end_byte(r);
        if (c=='\r' || c==('\r'|0x80)) c=sfx_end_byte(r);
        if (c!='\n' && c!=('\n'|0x80)) goto bad;
    }
    if (zm->sfx_failure) goto bad;
    if ((data[1] || data[2] || data[3] || data[4])
        && !(zm->sfx_packet_send && data[0]==ZRINIT)) goto bad;
    return data[0];
bad:
    sfx_fail(zm,SFX_PROTOCOL); return -1;
}
static BOOL sfx_refusal_zfin(zmodem_t* zm)
{
    sfx_end_reader r={zm,0,0,NULL,0,0};
    if (sfx_end_header(&r)==ZFIN) return TRUE;
    sfx_fail(zm,SFX_PROTOCOL); return FALSE;
}

/* The upstream initial scanner can skip arbitrary noise. That scanner alone
 * cannot prove a safe empty transfer. Reparse every actually consumed initial
 * byte, without scanning or I/O: optional exact "rz" startup line, zero to
 * eight zero-field CRC-valid ZRQINITs, and one zero-field CRC-valid ZFIN.
 * The whole prefix is capped at 256 bytes; every header retains the fixed
 * 32-byte/four-flow-control bound. No malformed prefix can become an empty
 * completion by resynchronizing to a later valid end. No file path uses this.
 */
static BOOL sfx_empty_prefix(zmodem_t* zm,const sfx_channel* ch)
{
    unsigned pos=0, queries=0;
    if (ch->initial_overflow || !ch->initial_size || ch->initial_size>sizeof(ch->initial)) goto bad;
    if (ch->initial_size>=3 && ch->initial[0]=='r' && ch->initial[1]=='z' && ch->initial[2]=='\r') {
        pos=3;
        if (pos<ch->initial_size && ch->initial[pos]=='\n') ++pos;
    }
    for (;;) {
        sfx_end_reader r={zm,0,0,ch->initial+pos,ch->initial_size-pos,0};
        int type=sfx_end_header(&r);
        pos+=r.pos;
        if (type==ZFIN && pos==ch->initial_size && !zm->sfx_failure) return TRUE;
        if (type!=ZRQINIT || ++queries>8 || pos>=ch->initial_size) goto bad;
    }
bad:
    sfx_fail(zm,SFX_PROTOCOL); return FALSE;
}

/* Create-new, no sharing, no following an existing reparse target. All three
 * filenames below are compile-time constants in a host-owned private cwd. */
static FILE* sfx_new_file(const char* name)
{
    HANDLE handle=CreateFileA(name,GENERIC_WRITE,0,NULL,CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    BY_HANDLE_FILE_INFORMATION info;
    int fd;
    FILE* fp;
    if (handle==INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileInformationByHandle(handle,&info) || info.nNumberOfLinks!=1
        || (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) {
        CloseHandle(handle); return NULL;
    }
    fd=_open_osfhandle((intptr_t)handle,_O_WRONLY|_O_BINARY);
    if (fd<0) { CloseHandle(handle); return NULL; }
    fp=_fdopen(fd,"wb");
    if (!fp) _close(fd);
    return fp;
}
static BOOL sfx_close_file(FILE* fp)
{
    BOOL ok=!ferror(fp);
    if (fflush(fp)!=0) ok=FALSE;
    if (fclose(fp)!=0) ok=FALSE;
    return ok;
}

static int sfx_message_main(int argc, char** argv)
{
    sfx_channel ch={0};
    zmodem_t strict;
    WSADATA data;
    unsigned long long descriptor=0;
    size_t i;
    int type, peer_len;
    struct sockaddr_in peer;
    u_long nonblocking=1;
    FILE *payload=NULL, *result=NULL, *dsz=NULL;
    HANDLE directory;
    BY_HANDLE_FILE_INFORMATION dirinfo;
    BOOL completed=FALSE, batch=FALSE, empty=FALSE;
    int64_t actual=0;

    if (argc!=3 || !argv[2][0]) return SFX_LOCAL_IO;
    for (i=0; argv[2][i]; ++i) {
        unsigned digit=(unsigned char)argv[2][i]-'0';
        if (digit>9 || i>=10 || descriptor>(UINT32_MAX-digit)/10) return SFX_LOCAL_IO;
        descriptor=descriptor*10+digit;
    }
    if (!descriptor || descriptor==INVALID_SOCKET || descriptor>UINT32_MAX) return SFX_LOCAL_IO;
    if (WSAStartup(MAKEWORD(2,2),&data)) return SFX_LOCAL_IO;
    ch.socket=(SOCKET)descriptor;
    peer_len=sizeof(peer);
    if (getpeername(ch.socket,(struct sockaddr*)&peer,&peer_len) || peer.sin_family!=AF_INET
        || ntohl(peer.sin_addr.s_addr)!=INADDR_LOOPBACK
        || ioctlsocket(ch.socket,FIONBIO,&nonblocking)) return SFX_LOCAL_IO;
    directory=CreateFileA(".",FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (directory==INVALID_HANDLE_VALUE) return SFX_LOCAL_IO;
    if (!GetFileInformationByHandle(directory,&dirinfo)
        || (dirinfo.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(directory); return SFX_LOCAL_IO;
    }
    /* Holding the directory handle prevents ordinary rename/delete until exit. */
    result=sfx_new_file("result.txt");
    dsz=sfx_new_file("dsz.log");
    if (!result || !dsz) {
        if (result) fclose(result);
        if (dsz) fclose(dsz);
        CloseHandle(directory); return SFX_LOCAL_IO;
    }
    ch.start=ch.inbound=GetTickCount64();
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
            actual=_ftelli64(payload);
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
        CloseHandle(directory); return SFX_LOCAL_IO;
    }
    }
    CloseHandle(directory);
    closesocket(ch.socket);
    return strict.sfx_failure ? (int)strict.sfx_failure : 0;
}
#endif
