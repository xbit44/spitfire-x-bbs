/* SFX packet sender additions October 13 2026, GPL-2.0-or-later, same grant
 * as SEXYZ. Separate executable only; never linked/copied into SFX.
 * One fixed host-created private payload.qwk. No INI/environment discovery,
 * caller path, resume, overwrite, serial purge or arbitrary post-transfer drain.
 */
#ifdef _WIN32
static BOOL sfx_packet_send_waiting(void* ctx,unsigned timeout)
{
    sfx_channel* ch=(sfx_channel*)ctx;
    ULONGLONG end=GetTickCount64()+(ULONGLONG)timeout*1000;
    for (;;) {
        fd_set set;
        ULONGLONG now,remaining;
        struct timeval wait;
        int ready;
        if (sfx_cancelled(ch))return FALSE;
        now=GetTickCount64();remaining=now<end?end-now:0;
        wait.tv_sec=0;wait.tv_usec=(long)(remaining>100?100000:remaining*1000);
        FD_ZERO(&set);FD_SET(ch->socket,&set);
        ready=select(0,&set,NULL,NULL,&wait);
        if (ready==SOCKET_ERROR) {ch->cancelled=TRUE;return FALSE;}
        if (ready)return TRUE;
        if (GetTickCount64()>=end)return FALSE;
        /* Honor the requested sender initialization/backchannel wait while
         * rechecking unchanged60s/300s cancellation at most100ms apart.
         * Timeout0 remains a single nonblocking poll. No bytes consumed,
         * input drain, handshake prefeed, renewed deadline or larger queue. */
    }
}
static FILE* sfx_packet_open_read(int64_t* size)
{
    HANDLE handle=CreateFileA("payload.qwk",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    BY_HANDLE_FILE_INFORMATION info;
    int fd;
    FILE* fp;
    if (handle==INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileInformationByHandle(handle,&info) || info.nNumberOfLinks!=1 || info.nFileSizeHigh
        || !info.nFileSizeLow || info.nFileSizeLow>SFX_MESSAGE_CAP
        || (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) {
        CloseHandle(handle); return NULL;
    }
    *size=info.nFileSizeLow;
    fd=_open_osfhandle((intptr_t)handle,_O_RDONLY|_O_BINARY);
    if (fd<0) {CloseHandle(handle);return NULL;}
    fp=_fdopen(fd,"rb");if (!fp)_close(fd);return fp;
}
static BOOL sfx_packet_send_finish(zmodem_t* strict)
{
    unsigned attempt;
    if (zmodem_send_zfin(strict)!=0) {
        sfx_fail(strict,SFX_COMPLETION);return FALSE;
    }
    for (attempt=0;attempt<8 && !sfx_cancelled(strict->cbdata);++attempt) {
        sfx_end_reader r={strict,0,0,NULL,0,0};
        int type;
        type=sfx_end_header(&r);
        /* One CRC-valid bounded ZRINIT can be a delayed ready/skip response.
         * It is not discarded noise or delivery proof. Keep waiting for the
         * acknowledgement of the ALREADY sent ZFIN; retransmitting here puts
         * a duplicate ZFIN ahead of OO at a receiver that already answered.
         * Unchanged deadlines and strict per-header byte/flow budgets apply. */
        if (type==ZRINIT && !strict->sfx_failure)continue;
        if (type==ZFIN && !strict->sfx_failure
            && sfx_send(strict->cbdata,'O',2)==0 && sfx_send(strict->cbdata,'O',2)==0
            && !sfx_cancelled(strict->cbdata))return TRUE;
        break;
    }
    sfx_fail(strict,SFX_COMPLETION);return FALSE;
}
static int sfx_packet_send_main(int argc,char** argv)
{
    sfx_channel ch={0};
    zmodem_t strict;
    WSADATA data;
    unsigned long long descriptor=0;
    size_t i,n;
    int peer_len;
    struct sockaddr_in peer;
    u_long nonblocking=1;
    FILE *payload=NULL,*result=NULL,*dsz=NULL;
    HANDLE directory;
    BY_HANDLE_FILE_INFORMATION dirinfo;
    int64_t offered=0,actual=0;
    uint64_t sent=0;
    BOOL completed=FALSE,batch=FALSE;
    char filename[13];
    if (argc!=4 || !argv[2][0])return SFX_LOCAL_IO;
    n=strlen(argv[3]);if (!n || n>8 || argv[3][0]<'A' || argv[3][0]>'Z')return SFX_LOCAL_IO;
    memcpy(filename,argv[3],n);memcpy(filename+n,".QWK",5);
    if (!sfx_filename((const unsigned char*)filename,n+4))return SFX_LOCAL_IO;
    for (i=0;argv[2][i];++i) {
        unsigned digit=(unsigned char)argv[2][i]-'0';
        if (digit>9 || i>=10 || descriptor>(UINT32_MAX-digit)/10)return SFX_LOCAL_IO;
        descriptor=descriptor*10+digit;
    }
    if (!descriptor || descriptor==INVALID_SOCKET || descriptor>UINT32_MAX || WSAStartup(MAKEWORD(2,2),&data))return SFX_LOCAL_IO;
    ch.socket=(SOCKET)descriptor;peer_len=sizeof(peer);
    if (getpeername(ch.socket,(struct sockaddr*)&peer,&peer_len) || peer.sin_family!=AF_INET
        || ntohl(peer.sin_addr.s_addr)!=INADDR_LOOPBACK || ioctlsocket(ch.socket,FIONBIO,&nonblocking))return SFX_LOCAL_IO;
    directory=CreateFileA(".",FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (directory==INVALID_HANDLE_VALUE)return SFX_LOCAL_IO;
    if (!GetFileInformationByHandle(directory,&dirinfo) || (dirinfo.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {CloseHandle(directory);return SFX_LOCAL_IO;}
    payload=sfx_packet_open_read(&offered);result=sfx_new_file("result.txt");dsz=sfx_new_file("dsz.log");
    if (!payload || !result || !dsz) {if(payload)fclose(payload);if(result)fclose(result);if(dsz)fclose(dsz);CloseHandle(directory);return SFX_LOCAL_IO;}
    ch.start=ch.inbound=GetTickCount64();
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
        if (!ok){CloseHandle(directory);return SFX_LOCAL_IO;}
    }
    CloseHandle(directory);closesocket(ch.socket);return strict.sfx_failure?(int)strict.sfx_failure:0;
}
#endif
