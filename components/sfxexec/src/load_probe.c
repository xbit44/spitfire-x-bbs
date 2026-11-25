/* SFX native-VDM loader diagnostic, 2026-10-24. GPL-2.0-or-later.
 * No CRT, no static NTVDM imports, no entry-point code, no game/session hooks.
 * Only called after direct SFXEXEC RegisterModule failure. A successful Win32
 * LoadLibrary here is diagnostic evidence, not successful BOP registration.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "sfx_load.h"
typedef USHORT (WINAPI *getword_fn)(void);
typedef BYTE (WINAPI *getbyte_fn)(void);
typedef void (WINAPI *setword_fn)(USHORT);
typedef BYTE* (WINAPI *map_fn)(ULONG,ULONG,UCHAR);
static void add(char* out,unsigned* at,const char* text) {
    while(*text&&*at<SFX_LOAD_REPORT-1)out[(*at)++]=*text++;
    out[*at]=0;
}
static void decimal(char* out,unsigned* at,DWORD n) {
    char digits[11];unsigned count=0;do{digits[count++]=(char)('0'+n%10);n/=10;}while(n&&count<10);
    while(count&&*at<SFX_LOAD_REPORT-1)out[(*at)++]=digits[--count];out[*at]=0;
}
static int valid_path(const char* path) {
    const char tail[]="SFXEXEC.DLL";unsigned n=0,i;
    while(n<SFX_LOAD_PATH&&path[n]){if(path[n]<32||path[n]>=127||path[n]=='"')return 0;++n;}
    if(n>=SFX_LOAD_PATH||n<14||!((path[0]>='A'&&path[0]<='Z')||(path[0]>='a'&&path[0]<='z'))||path[1]!=':'||path[2]!='\\'||path[n-12]!='\\')return 0;
    for(i=0;i<11;i++){char c=path[n-11+i];if(c>='a'&&c<='z')c-=32;if(c!=tail[i])return 0;}
    return 1;
}
__declspec(dllexport) void __cdecl VDDDispatch(void) {
    HMODULE vdm=GetModuleHandleA("ntvdm.exe");setword_fn setax;getword_fn cx,es,di;getbyte_fn bl;map_fn map;
    sfx_load_request* request;unsigned at=0,i,prefix;DWORD attributes,attribute_error,error;HMODULE target;char image[261];
    const char* names[]={"VDDDeInstallIOHook","setAX","getES","getDI","getCX","getBH","getBL","getAX","MGetVdmPointer","call_ica_hw_interrupt","VDDInstallIOHook"};
    if(!vdm)return;
    setax=(setword_fn)GetProcAddress(vdm,"setAX");if(!setax)return;
    bl=(getbyte_fn)GetProcAddress(vdm,"getBL");if(!bl){setax(0xf001);return;}
    if(bl()==21){setax(SFX_LOAD_PAIR);return;}
    if(bl()!=0){setax(0xf002);return;}
    cx=(getword_fn)GetProcAddress(vdm,"getCX");es=(getword_fn)GetProcAddress(vdm,"getES");di=(getword_fn)GetProcAddress(vdm,"getDI");map=(map_fn)GetProcAddress(vdm,"MGetVdmPointer");
    if(!cx||!es||!di||!map){setax(0xf001);return;}
    if(cx()!=sizeof(*request)){setax(0xf003);return;}
    request=(sfx_load_request*)map(((ULONG)es()<<16)|di(),sizeof(*request),FALSE);
    if(!request||!valid_path(request->path)){setax(0xf004);return;}
    image[260]=0;if(!GetModuleFileNameA(GetModuleHandleA("SFXLDR.DLL"),image,260)){setax(0xf005);return;}
    prefix=0;for(i=0;image[i]&&i<260;i++)if(image[i]=='\\')prefix=i+1;
    for(i=0;i<prefix;++i){char a=image[i],b=request->path[i];if(a>='a'&&a<='z')a-=32;if(b>='a'&&b<='z')b-=32;if(!b||a!=b){setax(0xf005);return;}}
    if(!prefix||request->path[prefix]!='S'&&request->path[prefix]!='s'){setax(0xf005);return;}
    request->report[0]=0;
    SetLastError(0);attributes=GetFileAttributesA(request->path);attribute_error=attributes==INVALID_FILE_ATTRIBUTES?GetLastError():0;
    SetLastError(0);target=LoadLibraryExA(request->path,NULL,0);error=target?0:GetLastError();
    add(request->report,&at,"SFXLOAD native-VDM LoadLibraryExA(full_path,flags=0): loaded=");decimal(request->report,&at,target!=NULL);
    add(request->report,&at," win32_error=");decimal(request->report,&at,error);
    add(request->report,&at," file_attributes_error=");decimal(request->report,&at,attribute_error);add(request->report,&at,"\n");
    if(target){add(request->report,&at,"SFXLOAD target exports: VDDDispatch=");decimal(request->report,&at,GetProcAddress(target,"VDDDispatch")!=NULL);
        add(request->report,&at," VDDInitialize=");decimal(request->report,&at,GetProcAddress(target,"VDDInitialize")!=NULL);add(request->report,&at,"\n");FreeLibrary(target);}
    image[0]=0;image[260]=0;GetModuleFileNameA(vdm,image,260);add(request->report,&at,"SFXLOAD VDM module=");add(request->report,&at,image);add(request->report,&at,"\n");
    image[0]=0;GetModuleFileNameA(NULL,image,260);add(request->report,&at,"SFXLOAD process image=");add(request->report,&at,image);add(request->report,&at,"\nSFXLOAD VDM exports missing:");
    for(i=0;i<sizeof(names)/sizeof(names[0]);++i)if(!GetProcAddress(vdm,names[i])){add(request->report,&at," ");add(request->report,&at,names[i]);}
    add(request->report,&at," [end]\nSFXLOAD diagnostic only; failed BOP registration remains failed; game not started.\n");setax(0);
}
__declspec(dllexport) BOOL __cdecl VDDInitialize(PVOID module,ULONG reason,PCONTEXT context) {
    (void)module;(void)reason;(void)context;return TRUE;
}
