/* Protocol-only loopback test peer. No terminal emulator, UI or shipped image.
 * Reuses the pinned DeuceSSH engine family used by current SyncTERM; this is
 * not a claim to drive SyncTERM itself. Credentials/pin arrive only on stdin. */
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "deucessh.h"
#include "deucessh-auth.h"
#include "deucessh-conn.h"
#include "deucessh-algorithms.h"
static SOCKET wire=INVALID_SOCKET;
static dssh_session session;
static dssh_channel channel;
static atomic_bool done;
static ULONGLONG end;
static uint8_t pin[32];
static int syncterm_write;
static uint64_t sent_bytes;
static int io(uint8_t*b,size_t n,dssh_session s,void*d,int writing){
    (void)d;size_t at=0;
    while(at<n){
        if(atomic_load(&done)||GetTickCount64()>=end||dssh_session_is_terminated(s))return DSSH_ERROR_TERMINATED;
        int used=writing?send(wire,(char*)b+at,(int)(n-at),0):recv(wire,(char*)b+at,(int)(n-at),0);
        if(used>0){at+=used;continue;}
        if(used==SOCKET_ERROR&&WSAGetLastError()==WSAETIMEDOUT)continue;
        fprintf(stderr,"peer %s error %d\n",writing?"tx":"rx",used?WSAGetLastError():0);return DSSH_ERROR_TERMINATED;
    }return 0;
}
static int tx(uint8_t*b,size_t n,dssh_session s,void*d){return io(b,n,s,d,1);}
static int rx(uint8_t*b,size_t n,dssh_session s,void*d){return io(b,n,s,d,0);}
static void terminated(dssh_session s,void*d){(void)s;(void)d;shutdown(wire,SD_BOTH);}
static dssh_hostkey_decision verify(const char*a,unsigned bits,const uint8_t*sha,const uint8_t*blob,size_t n,void*d){
    (void)a;(void)bits;(void)blob;(void)n;(void)d;return memcmp(sha,pin,32)?DSSH_HOSTKEY_REJECT:DSSH_HOSTKEY_ACCEPT;
}
static DWORD WINAPI writer(void*unused){
    (void)unused;uint8_t bytes[32768];
    for(;;){
        uint32_t length;if(fread(&length,4,1,stdin)!=1)break;
        if(!length||length>sizeof(bytes)||fread(bytes,1,length,stdin)!=length)break;
        size_t at=0;
        while(at<length&&!atomic_load(&done)){
            int64_t n=dssh_chan_write(channel,0,bytes+at,length-at);
            if(n>0){at+=(size_t)n;sent_bytes+=(uint64_t)n;continue;}
            if(n==DSSH_ERROR_NOMORE&&!syncterm_write){if(dssh_chan_poll(channel,DSSH_POLL_WRITE,100)<0)break;continue;}
            fprintf(stderr,"peer write %lld accepted_bytes=%llu\n",n,(unsigned long long)sent_bytes);atomic_store(&done,true);break;
        }
    }
    atomic_store(&done,true);shutdown(wire,SD_BOTH);return 0;
}
static int line(char*b,size_t n){if(!fgets(b,(int)n,stdin))return 0;size_t k=strlen(b);if(!k||b[k-1]!='\n')return 0;b[--k]=0;return 1;}
int main(int argc,char**argv){
    if(argc!=3)return 2;int port=atoi(argv[1]);if(port!=26021)return 2;
    if(strcmp(argv[2],"syncterm")&&strcmp(argv[2],"wait"))return 2;syncterm_write=!strcmp(argv[2],"syncterm");
    char user[129],password[129],hex[66];if(!line(user,sizeof(user))||!line(password,sizeof(password))||!line(hex,sizeof(hex))||strlen(hex)!=64)return 2;
    for(int i=0;i<32;++i){unsigned v;if(sscanf(hex+2*i,"%2x",&v)!=1)return 2;pin[i]=(uint8_t)v;}
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);setvbuf(stdout,NULL,_IONBF,0);
    WSADATA ws;if(WSAStartup(MAKEWORD(2,2),&ws))return 2;
    end=GetTickCount64()+600000;atomic_init(&done,false);wire=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    struct sockaddr_in target={0};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons((u_short)port);
    if(connect(wire,(struct sockaddr*)&target,sizeof(target)))return 2;
    DWORD timeout=200;setsockopt(wire,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(timeout));setsockopt(wire,SOL_SOCKET,SO_SNDTIMEO,(char*)&timeout,sizeof(timeout));
    if(dssh_register_curve25519_sha256()||dssh_register_ssh_ed25519()||dssh_register_aes256_ctr()||dssh_register_hmac_sha2_256()||dssh_register_none_comp()||dssh_transport_set_callbacks(tx,rx,NULL,NULL))return 2;
    session=dssh_session_init(true,0);dssh_session_set_hostkey_verify_cb(session,verify,NULL);dssh_session_set_terminate_cb(session,terminated,NULL);
    int status=dssh_transport_handshake(session);if(status<0){fprintf(stderr,"handshake %d\n",status);return 3;}
    status=dssh_auth_password(session,user,password,NULL,NULL);SecureZeroMemory(password,sizeof(password));if(status<0){fprintf(stderr,"auth %d\n",status);return 3;}
    if(dssh_session_start(session)<0)return 3;
    struct dssh_chan_params params;dssh_chan_params_init(&params,DSSH_CHAN_SHELL);dssh_chan_params_set_pty(&params,true);dssh_chan_params_set_term(&params,"ansi");dssh_chan_params_set_size(&params,80,25,0,0);
    channel=dssh_chan_open(session,&params);dssh_chan_params_free(&params);if(!channel){fprintf(stderr,"channel failed\n");return 3;}
    fprintf(stderr,"READY native pinned SSH peer mode=%s\n",argv[2]);
    HANDLE thread=CreateThread(NULL,0,writer,NULL,0,NULL);if(!thread)return 3;
    while(!atomic_load(&done)&&GetTickCount64()<end){
        int ready=dssh_chan_poll(channel,DSSH_POLL_READ|DSSH_POLL_EVENT,100);if(ready<0){fprintf(stderr,"peer poll %d\n",ready);break;}
        if(ready&DSSH_POLL_EVENT){struct dssh_chan_event event;while(dssh_chan_read_event(channel,&event)==0){} }
        if(ready&DSSH_POLL_READ){uint8_t bytes[32768];int64_t n=dssh_chan_read(channel,0,bytes,sizeof(bytes));if(n==DSSH_ERROR_NOMORE)continue;if(n<=0)break;if(fwrite(bytes,1,(size_t)n,stdout)!=(size_t)n)break;}
    }
    atomic_store(&done,true);shutdown(wire,SD_BOTH);CancelSynchronousIo(thread);WaitForSingleObject(thread,3000);CloseHandle(thread);
    dssh_session_terminate(session);dssh_session_cleanup(session);closesocket(wire);WSACleanup();return 0;
}
