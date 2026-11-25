/* Section10 strict X/YMODEM file-job coordinator, October21 2026.
 * GPL-2.0-or-later. Separate SEXYZ executable; never linked into SFX.
 * Uses the same inherited payload capabilities, bounded transport, free-space
 * checks and durable receipt as the accepted ZMODEM job. No filesystem names
 * from the peer are opened. No protocol substitution or partial-file resume.
 */
static BOOL sfx_xy_byte(sfx_channel* ch,int value)
{
    return sfx_file_send(ch,(BYTE)value,2)==0;
}
static BOOL sfx_xy_signal(sfx_channel* ch,int value)
{
    return sfx_xy_byte(ch,value)&&sfx_file_flush_bytes(ch,2);
}
static BOOL sfx_xy_frame(sfx_channel* ch,const BYTE* data,unsigned size,unsigned seq,BOOL crc)
{
    unsigned i;uint16_t sum=0;BYTE checksum=0;
    if(!sfx_xy_byte(ch,size==128?SOH:STX)||!sfx_xy_byte(ch,seq&255)||!sfx_xy_byte(ch,(seq&255)^255))return FALSE;
    for(i=0;i<size;++i){sum=ucrc16(data[i],sum);checksum+=data[i];if(!sfx_xy_byte(ch,data[i]))return FALSE;}
    if(crc){if(!sfx_xy_byte(ch,sum>>8)||!sfx_xy_byte(ch,sum&255))return FALSE;}
    else if(!sfx_xy_byte(ch,checksum))return FALSE;
    return sfx_file_flush_bytes(ch,2);
}
/* One full bounded frame. CRC failure permits retry only in ACKed modes.
 * No purge/drain; a truncated frame fails the job instead of guessing alignment.
 */
static int sfx_xy_read(sfx_channel* ch,BYTE* data,unsigned* size,unsigned* seq)
{
    int start=sfx_file_recv(ch,10),n,inv,c;unsigned i;uint16_t crc=0;
    if(start==EOT)return EOT;
    if(start==CAN){(void)sfx_file_recv(ch,2);return CAN;}
    if(start<0)return -1;
    if(start!=SOH&&start!=STX)return -2;
    *size=start==SOH?128:1024;
    n=sfx_file_recv(ch,2);inv=sfx_file_recv(ch,2);if(n<0||inv<0)return -2;
    *seq=(unsigned)n;
    for(i=0;i<*size;++i){c=sfx_file_recv(ch,2);if(c<0)return -2;data[i]=(BYTE)c;crc=ucrc16(data[i],crc);}
    n=sfx_file_recv(ch,2);c=sfx_file_recv(ch,2);if(n<0||c<0)return -2;
    if(((*seq)^(unsigned)inv)!=255||crc!=(uint16_t)((n<<8)|c))return -3;
    return 0;
}
static BOOL sfx_xy_mode(sfx_channel* ch,char protocol,BOOL* crc)
{
    unsigned tries;
    for(tries=0;tries<8&&!sfx_cancelled(ch);++tries){
        int c=sfx_file_recv(ch,10);
        if(c==CAN)return FALSE;
        if(protocol=='G'){if(c=='G'){*crc=TRUE;return TRUE;}}
        else if(c=='C'){*crc=TRUE;return TRUE;}
        else if(protocol=='X'&&c==NAK){*crc=FALSE;return TRUE;}
        else if(c>=0)return FALSE; /* Never silently change the chosen protocol. */
    }
    return FALSE;
}
static BOOL sfx_xy_ack_frame(sfx_channel* ch,const BYTE* data,unsigned size,unsigned seq,BOOL crc,BOOL streaming)
{
    unsigned tries;
    for(tries=0;tries<8&&!sfx_cancelled(ch);++tries){
        int reply;
        if(!sfx_xy_frame(ch,data,size,seq,crc))return FALSE;
        if(streaming)return TRUE;
        reply=sfx_file_recv(ch,10);if(reply==ACK)return TRUE;
        if(reply!=NAK&&reply!=-1)return FALSE;
    }
    return FALSE;
}
static BOOL sfx_xy_send_eot(sfx_channel* ch)
{
    unsigned tries;
    for(tries=0;tries<8&&!sfx_cancelled(ch);++tries){
        int reply;if(!sfx_xy_signal(ch,EOT))return FALSE;
        reply=sfx_file_recv(ch,10);if(reply==ACK)return TRUE;
        if(reply!=NAK&&reply!=-1)return FALSE;
    }
    return FALSE;
}
static BOOL sfx_xy_send_job(sfx_channel* ch,sfx_file_job* job,char protocol)
{
    unsigned i;BOOL streaming=protocol=='G',crc=TRUE;BYTE block[1024];
    for(i=0;i<job->count;++i){
        sfx_file_slot* slot=&job->slot[i];uint64_t length=0,offset=0;unsigned seq=1;BOOL ok=FALSE;
        FILE* file=sfx_file_open(slot,TRUE,&length);if(!file)return FALSE;
        slot->offered=1;++job->zm->sfx_attempted;
        do {
            if(!sfx_xy_mode(ch,protocol,&crc))break;
            if(protocol!='X'){
                unsigned name=(unsigned)strlen(slot->name);int used;
                memset(block,0,sizeof(block));memcpy(block,slot->name,name);
                used=snprintf((char*)block+name+1,sizeof(block)-name-1,"%" PRIu64,length);
                if(used<1||(unsigned)used>=sizeof(block)-name-1)break;
                if(!sfx_xy_ack_frame(ch,block,name+2+(unsigned)used<=128?128:1024,0,TRUE,streaming))break;
                if(!sfx_xy_mode(ch,protocol,&crc))break;
            }
            while(offset<length&&!sfx_cancelled(ch)){
                unsigned size=protocol=='X'?128:1024;
                unsigned count=(unsigned)((length-offset)<size?(length-offset):size);
                /* Streaming peers send no per-block ACK. Observe cancellation
                   (or any invalid early response) between bounded blocks. */
                if(streaming&&sfx_file_waiting(ch,0)){(void)sfx_file_recv(ch,0);break;}
                memset(block,CPMEOF,size);
                if(fread(block,1,count,file)!=count||!sfx_xy_ack_frame(ch,block,size,seq,crc,streaming))break;
                offset+=count;++seq;
            }
            if(offset!=length||ferror(file)||sfx_cancelled(ch)||!sfx_xy_send_eot(ch))break;
            ok=TRUE;
        }while(0);
        if(fclose(file)!=0)ok=FALSE;
        if(!ok)return FALSE;
        slot->completed=slot->zeof=1;slot->bytes=length;
    }
    if(protocol!='X'){
        if(!sfx_xy_mode(ch,protocol,&crc))return FALSE;
        memset(block,0,128);
        /* Batch termination is a full, CRC-protected empty header. */
        if(!sfx_xy_ack_frame(ch,block,128,0,TRUE,streaming))return FALSE;
    }
    return TRUE;
}
static BOOL sfx_xy_header(const BYTE* block,unsigned size,char* name,uint64_t* length,BOOL* empty)
{
    unsigned n=0,at,digits=0;uint64_t value=0;
    while(n<size&&block[n])++n;
    if(n==0){*empty=TRUE;return TRUE;}
    if(n>128||n>=size||!sfx_filename(block,n))return FALSE;
    memcpy(name,block,n);name[n]=0;at=n+1;
    while(at<size&&block[at]>='0'&&block[at]<='9'){
        unsigned digit=block[at++]-'0';if(++digits>20||value>(SFX_FILE_MAX-digit)/10)return FALSE;value=value*10+digit;
    }
    if(!digits||at>=size||(block[at]!=0&&block[at]!=' '))return FALSE;
    *length=value;*empty=FALSE;return TRUE;
}
static BOOL sfx_xy_receive_job(sfx_channel* ch,sfx_file_job* job,char protocol)
{
    BOOL streaming=protocol=='G';unsigned attempts=0;BYTE block[1024],previous[1024];
    for(;;){
        sfx_file_slot* slot=NULL;FILE* file;uint64_t length=0,offset=0,initial=0;
        unsigned i,size=0,seq=0,expected=1,errors=0,previous_size=0;BOOL ok=FALSE;
        if(protocol=='X'){if(attempts)return FALSE;slot=&job->slot[0];slot->offered=1;++attempts;++job->zm->sfx_attempted;}
        else {
            BOOL empty=FALSE;char name[129];int received;
            if(!sfx_xy_signal(ch,streaming?'G':'C'))return FALSE;
            do {received=sfx_xy_read(ch,block,&size,&seq);
                if(received==0)break;
                /* Before a header starts, repeat the selected mode request.
                   G has no retransmission after a frame begins, but the caller
                   may still be choosing a local file in the terminal. */
                if((streaming&&received!=-1)||(received!=-1&&received!=-3)||sfx_cancelled(ch)||++errors>=8||!sfx_xy_signal(ch,streaming?'G':'C'))return FALSE;
            }while(!sfx_cancelled(ch));
            if(received!=0||seq!=0||!sfx_xy_header(block,size,name,&length,&empty))return FALSE;
            if(empty)return streaming?TRUE:sfx_xy_signal(ch,ACK);
            if(++attempts>SFX_FILE_COUNT)return FALSE;
            ++job->zm->sfx_attempted;
            for(i=0;i<job->count;++i)if(sfx_file_same_name(name,job->slot[i].name)){slot=&job->slot[i];break;}
            if(!slot||slot->offered||length>slot->cap)return FALSE;
            slot->offered=1;
            if(!streaming&&!sfx_xy_signal(ch,ACK))return FALSE;
        }
        file=sfx_file_open(slot,FALSE,&initial);if(!file)return FALSE;
        do {
            if(!sfx_xy_signal(ch,streaming?'G':'C'))break;
            errors=0;
            while(!sfx_cancelled(ch)){
                int received=sfx_xy_read(ch,block,&size,&seq);
                if(received==EOT){
                    if(protocol!='X'&&offset!=length)break;
                    if(!streaming){
                        if(!sfx_xy_signal(ch,NAK)||sfx_file_recv(ch,10)!=EOT)break;
                    }
                    /* Commit bytes to disk before acknowledging completion. */
                    if(fflush(file)||fsync(fileno(file))||!sfx_xy_signal(ch,ACK))break;
                    ok=TRUE;break;
                }
                if(received!=0){
                    /* Synchronet's G-mode cancellation poll can consume the
                       post-header G before its sender waits for the data-mode
                       request. Retry only a completely idle first-block wait.
                       Partial frames, CRC errors and started streams still fail. */
                    if(streaming&&received==-1&&expected==1&&offset==0&&!sfx_cancelled(ch)&&++errors<8&&sfx_xy_signal(ch,'G'))continue;
                    if(streaming||(received!=-1&&received!=-3)||++errors>=8||!sfx_xy_signal(ch,expected==1?'C':NAK))break;
                    continue;
                }
                if(seq==((expected-1)&255)&&previous_size&&size==previous_size&&!memcmp(block,previous,size)){
                    if(streaming||!sfx_xy_signal(ch,ACK))break;
                    if(++errors>=8)break;continue;
                }
                if(seq!=(expected&255)||(protocol!='X'&&offset>=length))break;
                {
                    unsigned count=size;
                    if(protocol!='X'&&length-offset<count)count=(unsigned)(length-offset);
                    if(offset>slot->cap||count>slot->cap-offset||!sfx_file_space(ch,file,count)||fwrite(block,1,count,file)!=count)break;
                    offset+=count;memcpy(previous,block,size);previous_size=size;++expected;errors=0;
                    if(!streaming&&!sfx_xy_signal(ch,ACK))break;
                }
            }
        }while(0);
        if(!sfx_close_file(file))ok=FALSE;
        if(!ok)return FALSE;
        slot->completed=slot->zeof=1;slot->bytes=offset;
        /* XMODEM has no wire file length. Preserve every received byte; never
           guess that legitimate trailing Ctrl-Z/zero bytes are padding. */
        if(protocol=='X')return TRUE;
    }
}
