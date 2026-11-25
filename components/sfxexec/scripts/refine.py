"""Paired range/ABI checks and bounded VDD thread shutdown on derived source."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def edit(text,old,new):assert old in text,old[:100];return text.replace(old,new)
p=ROOT/'src/src/sbbs3/sbbsexec.c';s=p.read_text()
s=edit(s,'static BOOL sfx_bound;','''static BOOL sfx_bound, sfx_open, sfx_ring_ready, sfx_interrupt_ready, sfx_io_ready;
static BYTE sfx_node;
static HANDLE sfx_input_thread, sfx_interrupt_thread;
static volatile LONG sfx_closing;
static WORD sfx_vdd_handle;''')
s=edit(s,'&& !sfx_bound)', '&& (!sfx_bound || node_num!=sfx_node))')
s=edit(s,'sfx_bound=TRUE;retval=SFX_PAIR_WORD;', 'sfx_node=(BYTE)node_num;sfx_bound=TRUE;retval=SFX_PAIR_WORD;')
s=edit(s,'void _cdecl interrupt_thread(void *arg)','unsigned __stdcall interrupt_thread(void *arg)')
s=edit(s,'void _cdecl input_thread(void* arg)','unsigned __stdcall input_thread(void* arg)')
s=s.replace('while (1) {','while (!InterlockedCompareExchange(&sfx_closing,0,0)) {')
s=s.replace('WaitForMultipleObjects(handle_count, handles, FALSE, INFINITE)','WaitForMultipleObjects(handle_count, handles, FALSE, 100)')
s=edit(s,'\n}\n\nunsigned __stdcall input_thread','\n\treturn 0;\n}\n\nunsigned __stdcall input_thread')
s=edit(s,'lputs(LOG_DEBUG, "input_thread: terminated");','lputs(LOG_DEBUG, "input_thread: terminated");\n\treturn 0;')
s=edit(s,'if (GetLastError() == ERROR_HANDLE_EOF)','if (GetLastError() == ERROR_SEM_TIMEOUT) continue;\n\t\t\tif (GetLastError() == ERROR_HANDLE_EOF)')
s=edit(s,', MAILSLOT_WAIT_FOREVER /* Read timeout */',', 100 /* Bounded read permits joined close */')
s=edit(s,'\t\tcase VDD_OPEN:\n','\t\tcase VDD_OPEN:\n\t\t\tif(sfx_open) {retval=0xffff;break;}\n\t\t\tsfx_vdd_handle=getAX();InterlockedExchange(&sfx_closing,0);\n')
s=edit(s,'sfx_name(str,sizeof(str),"hungup",sfx_session,node_num);','sfx_ring_ready=TRUE;\n\t\t\tsfx_name(str,sizeof(str),"hungup",sfx_session,node_num);')
s=edit(s,'VDDInstallIOHook((HANDLE)getAX(), 1, &PortRange, &IOHandlers);',
    'if(!VDDInstallIOHook((HANDLE)getAX(),1,&PortRange,&IOHandlers)) {retval=7;break;}\n\t\t\t\tsfx_io_ready=TRUE;')
s=edit(s,'InitializeCriticalSection(&interrupt_mutex);','InitializeCriticalSection(&interrupt_mutex);sfx_interrupt_ready=TRUE;')
s=edit(s,'_beginthread(interrupt_thread, 0, NULL);','sfx_interrupt_thread=(HANDLE)_beginthreadex(NULL,0,interrupt_thread,NULL,0,NULL);\n\t\t\t\tif(!sfx_interrupt_thread){retval=8;break;}')
s=edit(s,'_beginthread(input_thread, 0, NULL);','sfx_input_thread=(HANDLE)_beginthreadex(NULL,0,input_thread,NULL,0,NULL);\n\t\t\tif(!sfx_input_thread){retval=9;break;}\n\t\t\tsfx_open=TRUE;')
a=s.index('\t\tcase VDD_CLOSE:');b=s.index('\n\t\tcase VDD_READ:',a)
s=s[:a]+'''\t\tcase VDD_CLOSE:
            retval=sfx_cleanup()?0:15;
            break;
'''+s[b:]
cleanup='''static BOOL sfx_cleanup(void)
{
    InterlockedExchange(&sfx_closing,1);
    if(interrupt_event) SetEvent(interrupt_event);
    if(sfx_input_thread && WaitForSingleObject(sfx_input_thread,2000)!=WAIT_OBJECT_0) return FALSE;
    if(sfx_interrupt_thread && WaitForSingleObject(sfx_interrupt_thread,2000)!=WAIT_OBJECT_0) return FALSE;
    if(sfx_input_thread) {CloseHandle(sfx_input_thread);sfx_input_thread=NULL;}
    if(sfx_interrupt_thread) {CloseHandle(sfx_interrupt_thread);sfx_interrupt_thread=NULL;}
    if(sfx_io_ready) {VDD_IO_PORTRANGE range;range.First=uart.io_base;range.Last=uart.io_base+UART_IO_RANGE;
        VDDDeInstallIOHook((HANDLE)(ULONG_PTR)sfx_vdd_handle,1,&range);sfx_io_ready=FALSE;}
    if(sfx_interrupt_ready) {DeleteCriticalSection(&interrupt_mutex);sfx_interrupt_ready=FALSE;}
    if(rdslot!=INVALID_HANDLE_VALUE) {CloseHandle(rdslot);rdslot=INVALID_HANDLE_VALUE;}
    if(wrslot!=INVALID_HANDLE_VALUE) {CloseHandle(wrslot);wrslot=INVALID_HANDLE_VALUE;}
    if(hungup_event) {CloseHandle(hungup_event);hungup_event=NULL;}
    if(hangup_event) {CloseHandle(hangup_event);hangup_event=NULL;}
    if(carrier_event) {CloseHandle(carrier_event);carrier_event=NULL;}
    if(interrupt_event) {CloseHandle(interrupt_event);interrupt_event=NULL;}
    if(sfx_ring_ready) {RingBufDispose(&rdbuf);sfx_ring_ready=FALSE;}
    sfx_open=FALSE;sfx_bound=FALSE;sfx_session[0]=0;sfx_node=0;
    return TRUE;
}

'''
s=edit(s,'__declspec(dllexport) void __cdecl VDDDispatch(void)',cleanup+'__declspec(dllexport) void __cdecl VDDDispatch(void)')
s=edit(s,'\tsetAX((WORD)retval);','\tif(getBL()==VDD_OPEN && retval) (void)sfx_cleanup();\n\tsetAX((WORD)retval);')
s=edit(s,'\tswitch (getBL()) {','\tif(!sfx_open && getBL()<=VDD_MAYBE_YIELD && getBL()!=VDD_OPEN && getBL()!=VDD_CLOSE) {setAX(0xffff);return;}\n\tswitch (getBL()) {')
p.write_text(s)
p=ROOT/'src/src/sbbs3/dosxtrn/dosxtrn.c';s=p.read_text()
s=edit(s,'#define DOSXTRN_REVISION','typedef char sfx_status_size[(sizeof(vdd_status_t)==20)?1:-1];\ntypedef char sfx_fossil_size[(sizeof(fossil_info_t)==19)?1:-1];\ntypedef char sfx_word_size[(sizeof(int)==2 && sizeof(DWORD)==4)?1:-1];\n\n#define DOSXTRN_REVISION')
s=edit(s,'info.id_string = id_string;','info.id_string = (DWORD)(char far*)id_string;')
s=edit(s,'\tvdd_op(VDD_CLOSE);','\tif(vdd_op(VDD_CLOSE)!=0) {fprintf(stderr,"!SFX close failed; host must end private VDM\\n");return 14;}')
p.write_text(s)
p=ROOT/'src/src/sbbs3/fossdefs.h';s=(ROOT/'upstream/src/sbbs3/fossdefs.h').read_text()
s=s.replace('#pragma pack(push)','#if !defined(_MSC_VER) || _MSC_VER>800\n\t#pragma pack(push)\n\t#endif')
s=s.replace('#pragma pack(pop)','#if defined(_MSC_VER) && _MSC_VER<=800\n\t#pragma pack()\n\t#else\n\t#pragma pack(pop)\n\t#endif')
p.write_text(s)
print('PASS bounded VDD close and strict legacy ABI/far-pointer checks applied')
