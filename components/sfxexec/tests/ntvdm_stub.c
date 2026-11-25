/* Engineering fixture only: no CPU/DOS emulation, never distributed as runtime. */
#include <windows.h>
static BYTE arena[65536];
static USHORT ax,cx,es,di;static BYTE bh,bl;
__declspec(dllexport) void TestRegisters(USHORT a,BYTE n,BYTE op,USHORT size,USHORT offset)
{ax=a;bh=n;bl=op;cx=size;es=0;di=offset;}
__declspec(dllexport) BYTE* TestBuffer(void){return arena;}
__declspec(dllexport) USHORT TestResult(void){return ax;}
__declspec(dllexport) USHORT getAX(void){return ax;}
__declspec(dllexport) BYTE getBH(void){return bh;}
__declspec(dllexport) BYTE getBL(void){return bl;}
__declspec(dllexport) USHORT getCX(void){return cx;}
__declspec(dllexport) USHORT getES(void){return es;}
__declspec(dllexport) USHORT getDI(void){return di;}
__declspec(dllexport) void setAX(USHORT value){ax=value;}
__declspec(dllexport) BYTE* MGetVdmPointer(ULONG address,ULONG size,UCHAR mode)
{(void)mode;if(address>sizeof(arena)||size>sizeof(arena)-address)return NULL;return arena+address;}
__declspec(dllexport) void call_ica_hw_interrupt(int ms,BYTE line,int count)
{(void)ms;(void)line;(void)count;}
__declspec(dllexport) BOOL VDDInstallIOHook(HANDLE handle,WORD count,void* range,void* handlers)
{(void)handle;(void)count;(void)range;(void)handlers;return TRUE;}
__declspec(dllexport) void VDDDeInstallIOHook(HANDLE handle,WORD count,void* range)
{(void)handle;(void)count;(void)range;}
