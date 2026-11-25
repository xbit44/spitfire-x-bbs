/* Compile the exact bound transport source, including its static KEX receive
 * function. I/O is an adversarial in-memory packet stream; waits use the real
 * Windows C11 shim. Full installed SSH/rekey/client checks remain separate. */
#include <windows.h>
#include <stdio.h>
#include "ssh-trans.c"
static unsigned checks;
static void check(bool value,const char* why){++checks;if(!value){fprintf(stderr,"FAIL %u: %s\n",checks,why);exit(1);}}
struct input {uint8_t* bytes;size_t count,at;};
static int send_bytes(uint8_t* b,size_t n,dssh_session s,void* p){(void)b;(void)n;(void)s;(void)p;return 0;}
static int receive_bytes(uint8_t* b,size_t n,dssh_session s,void* p){(void)s;struct input* in=p;if(n>in->count-in->at)return DSSH_ERROR_TERMINATED;memcpy(b,in->bytes+in->at,n);in->at+=n;return 0;}
static void append(struct input* in,size_t payload,uint8_t type){size_t padding=8-((5+payload)%8);if(padding<4)padding+=8;size_t packet=1+payload+padding,total=packet+4;in->bytes=realloc(in->bytes,in->count+total);check(in->bytes!=NULL,"synthetic packet buffer");uint8_t* b=in->bytes+in->count;b[0]=(uint8_t)(packet>>24);b[1]=(uint8_t)(packet>>16);b[2]=(uint8_t)(packet>>8);b[3]=(uint8_t)packet;b[4]=(uint8_t)padding;memset(b+5,0,payload+padding);b[5]=type;in->count+=total;}
static dssh_session session(struct input* in){dssh_session s=dssh_session_init(false,35000);check(s!=NULL,"actual transport initialization");s->rx_cbdata=in;s->trans.rekey_in_progress=true;return s;}
static void free_input(struct input* in){free(in->bytes);memset(in,0,sizeof(*in));}
static void queue_checks(void){
    struct input in={0};append(&in,1,SSH_MSG_USERAUTH_REQUEST);append(&in,1,SSH_MSG_KEXINIT);dssh_session s=session(&in);
    check(receive_peer_kexinit(s)==0&&s->trans.peer_kexinit_sz==1,"normal pre-peer-KEX packet retained until KEXINIT");s->trans.rekey_in_progress=false;uint8_t type,*payload;size_t len;
    check(recv_packet(s,&type,&payload,&len)==0&&type==SSH_MSG_USERAUTH_REQUEST&&len==1&&!s->trans.rekey_queue_head,"normal queued packet replays exactly once");dssh_session_cleanup(s);free_input(&in);
    for(unsigned n=0;n<1025;++n)append(&in,1,SSH_MSG_USERAUTH_REQUEST);s=session(&in);
    check(receive_peer_kexinit(s)==DSSH_ERROR_TOOLONG&&s->terminate,"many tiny pre-KEX packets terminate at actual count bound");size_t count=0;for(struct dssh_rekey_msg* p=s->trans.rekey_queue_head;p;p=p->next)++count;check(count==1024,"over-limit packet was not allocated or queued");dssh_session_cleanup(s);free_input(&in);
    for(unsigned n=0;n<513;++n)append(&in,32768,SSH_MSG_CHANNEL_DATA);s=session(&in);
    check(receive_peer_kexinit(s)==DSSH_ERROR_TOOLONG&&s->terminate,"pre-KEX retained payload bytes are bounded");size_t bytes=0;count=0;for(struct dssh_rekey_msg* p=s->trans.rekey_queue_head;p;p=p->next){++count;bytes+=p->len;}check(count==512&&bytes==16U*1024U*1024U,"accepted sixteen-MiB credit retained but overflowing allocation refused");dssh_session_cleanup(s);free_input(&in);
    append(&in,1,SSH_MSG_USERAUTH_REQUEST);append(&in,1,SSH_MSG_KEXINIT);s=session(&in);struct dssh_rekey_msg* prior=calloc(1,sizeof(*prior)+1);check(prior!=NULL,"retained prior-rekey fixture");prior->len=16U*1024U*1024U;s->trans.rekey_queue_head=s->trans.rekey_queue_tail=prior;
    check(receive_peer_kexinit(s)==DSSH_ERROR_TOOLONG&&s->terminate&&s->trans.rekey_queue_head==prior&&prior->next==NULL,"prior rekey queue counts toward the same ceiling before new allocation");dssh_session_cleanup(s);free_input(&in);
}
struct pending {dssh_session s;int result;};
static int reply_thread(void* value){struct pending* p=value;const uint8_t reply=SSH_MSG_REQUEST_FAILURE;p->result=send_to_slot(p->s,&p->s->trans.global_reply_slot,&reply,1);return 0;}
static dssh_session occupied(void){dssh_session s=dssh_session_init(false,35000);check(s!=NULL&&alloc_tx_slot(&s->trans.global_reply_slot,1,8,0)==0,"actual occupied global reply slot initialized");s->trans.tx_owner_active=true;s->trans.global_reply_slot.ready=true;return s;}
static void slot_checks(void){
    dssh_session s=occupied();s->timeout_ms=25;const uint8_t reply=SSH_MSG_REQUEST_FAILURE;const ULONGLONG start=GetTickCount64();
    check(send_to_slot(s,&s->trans.global_reply_slot,&reply,1)==DSSH_ERROR_TERMINATED&&s->terminate&&GetTickCount64()-start<1500,"occupied global reply wait has an actual finite deadline");check(s->trans.global_reply_slot.ready,"timed-out queued response not reported sent");s->trans.tx_owner_active=false;dssh_session_cleanup(s);
    s=occupied();s->timeout_ms=2000;struct pending wait={s,999};thrd_t thread;check(thrd_create(&thread,reply_thread,&wait)==thrd_success,"real waiting producer");Sleep(30);dssh_session_terminate(s);int code;thrd_join(thread,&code);check(wait.result==DSSH_ERROR_TERMINATED,"session cancellation wakes occupied reply waiter");s->trans.tx_owner_active=false;dssh_session_cleanup(s);
    s=occupied();s->timeout_ms=2000;wait.s=s;wait.result=999;check(thrd_create(&thread,reply_thread,&wait)==thrd_success,"normal slot waiter");Sleep(30);mtx_lock(&s->trans.tx_queue_mtx);s->trans.global_reply_slot.ready=false;cnd_broadcast(&s->trans.tx_slot_cnd);mtx_unlock(&s->trans.tx_queue_mtx);thrd_join(thread,&code);
    check(wait.result==0&&!s->terminate&&s->trans.global_reply_slot.ready&&s->trans.global_reply_slot.payload_len==1,"drained slot accepts next legitimate response before deadline");s->trans.tx_owner_active=false;dssh_session_cleanup(s);
}
int main(void){check(dssh_transport_set_callbacks(send_bytes,receive_bytes,NULL,NULL)==0,"test-only transport callbacks");queue_checks();slot_checks();printf("PASS %u exact-overlay SSH queue/deadline/replay/cancel checks; synthetic packet I/O, installed rekey separate\n",checks);return 0;}
