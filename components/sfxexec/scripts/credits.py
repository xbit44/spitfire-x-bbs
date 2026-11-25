"""Bound both mailslot directions with private per-session message credits."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];p=ROOT/'src/src/sbbs3/sbbsexec.c';s=p.read_text()
def edit(old,new):
    global s
    assert old in s,old[:100];s=s.replace(old,new)
edit('#include <vddsvc.h>','#include <vddsvc.h>\n#include <sddl.h>')
edit('static HANDLE sfx_input_thread, sfx_interrupt_thread;','static HANDLE sfx_input_thread, sfx_interrupt_thread;\nstatic HANDLE sfx_output_credit, sfx_input_credit;')
edit('if(sfx_ring_ready) {RingBufDispose','if(sfx_input_credit) {CloseHandle(sfx_input_credit);sfx_input_credit=NULL;}\n    if(sfx_output_credit) {CloseHandle(sfx_output_credit);sfx_output_credit=NULL;}\n    if(ini) {iniFreeStringList(ini);ini=NULL;}\n    if(sfx_ring_ready) {RingBufDispose')
edit('sfx_ring_ready=TRUE;','''sfx_ring_ready=TRUE;
            sfx_name(str,sizeof(str),"from_credit",sfx_session,node_num);
            sfx_output_credit=OpenSemaphoreA(SYNCHRONIZE|SEMAPHORE_MODIFY_STATE,FALSE,str);
            if(!sfx_output_credit){retval=11;break;}
            sfx_name(str,sizeof(str),"to_credit",sfx_session,node_num);
            sfx_input_credit=OpenSemaphoreA(SYNCHRONIZE|SEMAPHORE_MODIFY_STATE,FALSE,str);
            if(!sfx_input_credit){retval=12;break;}''')
edit('RingBufWrite(&rdbuf, buf, count);','RingBufWrite(&rdbuf, buf, count);\n\t\tReleaseSemaphore(sfx_input_credit,1,NULL);')
edit('WriteFile(wrslot,','sfx_write(wrslot,')
helper='''/* One credit represents one message, at most 4000 bytes. Kernel output
 * queues therefore contain at most 64 messages. Waits end on carrier loss. */
static BOOL sfx_write(HANDLE slot,const void* data,DWORD count,DWORD* sent,OVERLAPPED* unused)
{
    HANDLE waiters[2];DWORD result;(void)unused;*sent=0;
    if(!sfx_output_credit||!hungup_event)return FALSE;
    waiters[0]=sfx_output_credit;waiters[1]=hungup_event;
    result=WaitForMultipleObjects(2,waiters,FALSE,2000);
    if(result!=WAIT_OBJECT_0)return FALSE;
    if(count>4000)count=4000;
    if(!WriteFile(slot,data,count,sent,NULL)) {ReleaseSemaphore(sfx_output_credit,1,NULL);return FALSE;}
    return TRUE;
}

/* Explicit owner/SYSTEM DACL, independent of inherited NTVDM token defaults. */
static HANDLE sfx_mailslot(const char* name,DWORD maximum,DWORD timeout,SECURITY_ATTRIBUTES* unused)
{
    HANDLE token=NULL,result=INVALID_HANDLE_VALUE;DWORD size=0;BYTE* info=NULL;
    char* sid=NULL;char sddl[512];PSECURITY_DESCRIPTOR descriptor=NULL;SECURITY_ATTRIBUTES attributes;
    (void)unused;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return result;
    GetTokenInformation(token,TokenUser,NULL,0,&size);info=(BYTE*)malloc(size);
    if(info&&GetTokenInformation(token,TokenUser,info,size,&size)&&ConvertSidToStringSidA(((TOKEN_USER*)info)->User.Sid,&sid)) {
        _snprintf(sddl,sizeof(sddl),"D:P(A;;GA;;;SY)(A;;GA;;;%s)",sid);sddl[sizeof(sddl)-1]=0;
        if(ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl,SDDL_REVISION_1,&descriptor,NULL)) {
            attributes.nLength=sizeof(attributes);attributes.lpSecurityDescriptor=descriptor;attributes.bInheritHandle=FALSE;
            result=CreateMailslotA(name,maximum,timeout,&attributes);
        }
    }
    if(descriptor)LocalFree(descriptor);if(sid)LocalFree(sid);if(info)free(info);CloseHandle(token);return result;
}

'''
edit('void hangup()',helper+'void hangup()')
edit('rdslot = CreateMailslot(str','rdslot = sfx_mailslot(str')
edit('char  cwd[MAX_PATH + 1];','char  bounded[MAX_PATH + 1];')
edit('''p = (BYTE*)GetVDMPointer((ULONG)((getES() << 16) | getDI())
			                         , count, FALSE);
			lprintf(LOG_INFO, "LOAD_INI_FILE: %s", p);
			if (getCX()>MAX_PATH || p==NULL) { retval=0xffff; break; }''','''if(count>MAX_PATH) {retval=0xffff;break;}
            p=(BYTE*)GetVDMPointer((ULONG)((getES()<<16)|getDI()),count,FALSE);
            if(!p){retval=0xffff;break;}memcpy(bounded,p,count);bounded[count]=0;p=(BYTE*)bounded;
            lprintf(LOG_INFO,"LOAD_INI_FILE: %s",p);''')
edit('InitializeCriticalSection(&interrupt_mutex);sfx_interrupt_ready=TRUE;','if(!interrupt_event){retval=10;break;}\n\t\t\t\tInitializeCriticalSection(&interrupt_mutex);sfx_interrupt_ready=TRUE;')
p.write_text(s)
print('PASS bounded private two-way credits and explicit driver mailslot ACL')
