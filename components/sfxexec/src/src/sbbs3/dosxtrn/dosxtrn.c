/* SFX modifications, 2026-10-24: private paired session/node channels,
 * strict SFX loading, bounded credits and joined lifecycle. Original notices
 * and GPL/LGPL terms are retained below. See SFX-CHANGES.txt. */
/* Synchronet External DOS Program Launcher (16-bit MSVC 1.52c project) */

/****************************************************************************
 * @format.tab-size 4		(Plain Text/Source Code File Header)			*
 * @format.use-tabs true	(see http://www.synchro.net/ptsc_hdr.html)		*
 *																			*
 * Copyright Rob Swindell - http://www.synchro.net/copyright.html			*
 *																			*
 * This program is free software; you can redistribute it and/or			*
 * modify it under the terms of the GNU General Public License				*
 * as published by the Free Software Foundation; either version 2			*
 * of the License, or (at your option) any later version.					*
 * See the GNU General Public License for more details: gpl.txt or			*
 * http://www.fsf.org/copyleft/gpl.html										*
 *																			*
 * For Synchronet coding style and modification guidelines, see				*
 * http://www.synchro.net/source.html										*
 *																			*
 * Note: If this box doesn't appear square, then you need to fix your tabs.	*
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <dos.h>			/* _dos_set/getvect() */
#include <windows.h>		/* BOOL, etc. */
#include "vdd_func.h"
#include "sfx_pair.h"
#include "sfx_gate.h"
#include <time.h>
#include "isvbop.h"			/* ddk\inc */
#include "fossdefs.h"
#include "../git_branch.h"
#include "../git_hash.h"

typedef char sfx_status_size[(sizeof(vdd_status_t)==20)?1:-1];
typedef char sfx_fossil_size[(sizeof(fossil_info_t)==19)?1:-1];
typedef char sfx_word_size[(sizeof(int)==2 && sizeof(DWORD)==4)?1:-1];

#define DOSXTRN_REVISION	26
#define VDD_FILENAME	"SFXEXEC.DLL"

#if 0
#define DEBUG_INT_CALLS
#define DEBUG_DOS_CALLS
#define DEBUG_FOSSIL_CALLS
#endif

/****************************************************************************/
/* Truncates white-space chars off end of 'str' and terminates at first tab */
/****************************************************************************/
static void truncsp(char *str)
{
	size_t c;

	str[strcspn(str,"\t")]=0;
	c=strlen(str);
	while(c && (unsigned char)str[c-1]<=' ') c--;
	str[c]=0;
}
short	vdd=0;
BYTE	node_num=0;
char	id_string[128];
#ifdef DEBUG_INT_CALLS
ulong	int14calls=0;
ulong	int16calls=0;
ulong	int21calls=0;
ulong	int29calls=0;
#endif

void (interrupt *oldint14)();
void (interrupt *oldint16)();
void (interrupt *oldint21)();
void (interrupt *oldint29)();


static int vdd_buf(BYTE op, int count, WORD buf_seg, WORD buf_off)
{
	int retval;

	_asm {
		push	bx
		push	cx
		push	es
		push	di
		mov		ax,	vdd
		mov		bh,	node_num
		mov		bl,	op
		mov		cx,	count
		mov		es, buf_seg
		mov		di, buf_off
	}
	DispatchCall();
	_asm {
		mov		retval, ax
		pop		di
		pop		es
		pop		cx
		pop		bx
	}
	return(retval);
}

static int vdd_str(BYTE op, char* str)
{
	WORD			buf_seg;

	_asm mov buf_seg, ss;
	return vdd_buf(op, strlen(str), buf_seg, (WORD)str);
}

static int vdd_op(BYTE op)
{
	int retval;

#if FALSE	/* disable yield? */
	if(op==VDD_YIELD)
		return(0);
#endif
	_asm {
		push	bx
		mov		ax,	vdd
		mov		bh,	node_num
		mov		bl,	op
	}
	DispatchCall();
	_asm {
		mov		retval, ax
		pop		bx
	}
	return(retval);
}


void vdd_getstatus(vdd_status_t* status)
{
	WORD			buf_seg;

	_asm mov buf_seg, ss;
	if(vdd_buf(VDD_STATUS, sizeof(vdd_status_t), buf_seg, (WORD)status)!=0)
		memset(status,0,sizeof(vdd_status_t));
}

#define HAPPY_PORT_STATUS FOSSIL_MDM_STATUS_DCD_CHNG | FOSSIL_MDM_STATUS_DCD | FOSSIL_MDM_STATUS_CTS | FOSSIL_LINE_STATUS_THRE | FOSSIL_LINE_STATUS_TSRE
WORD PortStatus()
{
	WORD			status=FOSSIL_MDM_STATUS_DCD_CHNG; /* AL bit 3 (change in DCD) always set */
	vdd_status_t	vdd_status;

	vdd_getstatus(&vdd_status);

	if(vdd_status.online)			/* carrier detect */
		status|=FOSSIL_MDM_STATUS_DCD;

	if(vdd_status.inbuf_full)		/* receive data ready  */
		status|=FOSSIL_LINE_STATUS_RDA;

/*	if(vm->overrun)					/* overrun error detected */
/*		status|=0x0200;				/* OVRN */

	if(vdd_status.outbuf_full
		<vdd_status.outbuf_size/2)	/* room available in output buffer */
		status|=FOSSIL_LINE_STATUS_THRE;

	if(!vdd_status.outbuf_full)		/* output buffer is empty */
		status|=FOSSIL_LINE_STATUS_TSRE;

	if(vdd_status.outbuf_full < vdd_status.outbuf_size)
		status|=FOSSIL_MDM_STATUS_CTS;

	return(status);
}

#ifdef DEBUG_FOSSIL_CALLS
	DWORD fossil_calls[0x100];

const char* fossil_func(int func)
{
	switch(func) {
		case FOSSIL_FUNC_SET_RATE:		return "set rate";
		case FOSSIL_FUNC_PUT_CHAR:		return "put char";
		case FOSSIL_FUNC_GET_CHAR:		return "get char";
		case FOSSIL_FUNC_GET_STATUS:	return "get status";
		case FOSSIL_FUNC_INIT:			return "init";
		case FOSSIL_FUNC_UNINIT:		return "uninit";
		case FOSSIL_FUNC_DTR:			return "dtr";
		case FOSSIL_FUNC_GET_TIMER:		return "get timer";
		case FOSSIL_FUNC_FLUSH_OUT:		return "flush out";
		case FOSSIL_FUNC_PURGE_OUT:		return "purge out";
		case FOSSIL_FUNC_PURGE_IN:		return "purge in";
		case FOSSIL_FUNC_WRITE_CHAR:	return "write char";
		case FOSSIL_FUNC_PEEK:			return "peek";
		case FOSSIL_FUNC_GET_KB:		return "get kb";
		case FOSSIL_FUNC_GET_KB_WAIT:	return "get kb wait";
		case FOSSIL_FUNC_FLOW_CTRL:		return "flow_ctrl";
		case FOSSIL_FUNC_CTRL_C:		return "ctrl_c";
		case FOSSIL_FUNC_BREAK:			return "break";
		case FOSSIL_FUNC_GET_INFO:		return "get info";
		default: return "unknown";
	}
}
#endif

void interrupt winNTint14(
	unsigned _es, unsigned _ds,
	unsigned _di, unsigned _si,
	unsigned _bp, unsigned _sp,
	unsigned _bx, unsigned _dx,
	unsigned _cx, unsigned _ax,
	)
{
	BYTE			ch;
	BYTE far*		p;
	WORD			buf_seg;
	int				rd;
	int				wr;
	vdd_status_t	vdd_status;
    fossil_info_t info = { 
		 sizeof(info)
		,FOSSIL_REVISION
		,DOSXTRN_REVISION	/* driver revision */
		,0			/* ID string pointer */	
		,0,0		/* receive buffer size/free (overwritten later) */
		,0,0		/* transmit buffer size/free (overwritten later) */
        ,80,25		/* screen dimensions (cols, rows) */
					/* port settings (i.e. 38400 N-8-1): */
        ,FOSSIL_BAUD_RATE_38400
		|FOSSIL_PARITY_NONE
		|FOSSIL_DATA_BITS_8
		|FOSSIL_STOP_BITS_1
	};

#ifdef DEBUG_INT_CALLS
	int14calls++;
#endif

#ifdef DEBUG_FOSSIL_CALLS
	fossil_calls[_ax>>8]++;
#endif

	switch(_ax>>8) {
		case FOSSIL_FUNC_SET_RATE:	/* Initialize/Set baud rate */
			_ax = PortStatus();
			break;
		case FOSSIL_FUNC_PUT_CHAR: /* write char to com port, with wait */
			ch=_ax&0xff;
			_asm mov buf_seg, ss;
			wr = vdd_buf(VDD_WRITE, 1, buf_seg, (WORD)&ch);
			_ax = PortStatus();
			if(wr != 1)
				_ax |= FOSSIL_LINE_STATUS_TIMEOUT;
			break;
		case FOSSIL_FUNC_GET_CHAR: /* read char from com port, with wait */
			_asm mov buf_seg, ss;
			rd = vdd_buf(VDD_READ, 1, buf_seg, (WORD)&ch);
			_ax = ch;
			if(rd != 1) {
				vdd_op(VDD_YIELD);
				_ax = FOSSIL_LINE_STATUS_TIMEOUT;
			}
			break;
		case FOSSIL_FUNC_GET_STATUS:	/* request status */
			_ax=PortStatus();
			if(_ax == HAPPY_PORT_STATUS)
				vdd_op(VDD_MAYBE_YIELD);
			break;
		case FOSSIL_FUNC_INIT:	/* initialize */
			_ax=FOSSIL_SIGNATURE;	/* magic number = success */
			_bx=FOSSIL_REVISION<<8 | FOSSIL_FUNC_HIGHEST;	/* FOSSIL rev/maximum FOSSIL func supported */
			break;
		case FOSSIL_FUNC_DTR:
			if((_ax&0xff)==0)	/* Lower DTR */
				vdd_op(VDD_HANGUP);
			break;
        case FOSSIL_FUNC_FLUSH_OUT:	/* flush output buffer	*/
			break;
        case FOSSIL_FUNC_PURGE_OUT:	/* purge output buffer	*/
			vdd_op(VDD_OUTBUF_PURGE);
			break;
        case FOSSIL_FUNC_PURGE_IN:	/* purge input buffer	*/
			vdd_op(VDD_INBUF_PURGE);
			break;
		case FOSSIL_FUNC_WRITE_CHAR:	/* write char to com port, no wait */
			ch=_ax&0xff;
			_asm mov buf_seg, ss;
			_ax = vdd_buf(VDD_WRITE, 1, buf_seg, (WORD)&ch);
			if(_ax != 1)
				vdd_op(VDD_YIELD);
			break;
        case FOSSIL_FUNC_PEEK:	/* non-destructive read-ahead */
			_asm mov buf_seg, ss;
			rd = vdd_buf(VDD_PEEK, 1, buf_seg, (WORD)&ch);
			_ax = ch;
			if(rd == 0) {
				vdd_op(VDD_YIELD);
				_ax = FOSSIL_CHAR_NOT_AVAILABLE;
			}
			break;
        case FOSSIL_FUNC_READ_BLOCK:	/* read block, no wait */
			_ax = vdd_buf(VDD_READ, _cx, _es, _di);
			if(_ax == 0)
				vdd_op(VDD_YIELD);
			break;
        case FOSSIL_FUNC_WRITE_BLOCK:	/* write block, no wait */
			_ax = vdd_buf(VDD_WRITE, _cx, _es, _di);
			break;
        case FOSSIL_FUNC_GET_INFO:	/* driver info */
			vdd_getstatus(&vdd_status);
			info.inbuf_size=vdd_status.inbuf_size;
			info.inbuf_free=info.inbuf_size-vdd_status.inbuf_full;
			info.outbuf_size=vdd_status.outbuf_size;
			info.outbuf_free=info.outbuf_size-vdd_status.outbuf_full;
			info.id_string = (DWORD)(char far*)id_string;

			if(vdd_status.inbuf_full==vdd_status.outbuf_full==0)
				vdd_op(VDD_MAYBE_YIELD);

			p = _MK_FP(_es,_di);
            wr=sizeof(info);
            if(wr>_cx)
            	wr=_cx;
            _fmemcpy(p, &info, wr);
        	_ax=wr;
            break;
		case FOSSIL_FUNC_GET_KB:
			_ax = FOSSIL_CHAR_NOT_AVAILABLE;
			break;
	}
}

void int14stub(void)
{
	/* This function will be overwritten later (during runtime) with FOSSIL signature */
}

void interrupt winNTint16(
	unsigned _es, unsigned _ds,
	unsigned _di, unsigned _si,
	unsigned _bp, unsigned _sp,
	unsigned _bx, unsigned _dx,
	unsigned _cx, unsigned _ax,
	unsigned _ip, unsigned _cs,
    unsigned flags )
{
	BYTE			ch;
	WORD			buf_seg;
	vdd_status_t	status;

#ifdef DEBUG_INT_CALLS
	int16calls++;
#endif

	vdd_getstatus(&status);
 	switch(_ax>>8) {
    	case 0x00:	/* Read char from keyboard */
        case 0x10:	/* Read char from enhanced keyboard */
			if(status.inbuf_full) {
				_asm mov buf_seg, ss;
				vdd_buf(VDD_READ, 1, buf_seg, (WORD)&ch);
				_ax=ch;
				return;
			} 
			vdd_op(VDD_MAYBE_YIELD);
			break;
    	case 0x01:	/* Get keyboard status */
        case 0x11:	/* Get enhanced keyboard status */
			if(status.inbuf_full) {
				_asm mov buf_seg, ss;
				vdd_buf(VDD_PEEK, 1, buf_seg, (WORD)&ch);
                flags&=~(1<<6);	/* clear zero flag */
                _ax=ch;
				return;
			}
			vdd_op(VDD_MAYBE_YIELD);
	        break;
	}

	_chain_intr(oldint16);		
}

#ifdef DEBUG_DOS_CALLS
	DWORD dos_calls[0x100];
#endif

void interrupt winNTint21(
	unsigned _es, unsigned _ds,
	unsigned _di, unsigned _si,
	unsigned _bp, unsigned _sp,
	unsigned _bx, unsigned _dx,
	unsigned _cx, unsigned _ax,
	)
{
#ifdef DEBUG_INT_CALLS
	int21calls++;
#endif
	if(_ax>>8 == 0x2c)	/* GET_SYSTEM_TIME */
		vdd_op(VDD_MAYBE_YIELD);
#ifdef DEBUG_DOS_CALLS
	dos_calls[_ax>>8]++;
#endif
	_chain_intr(oldint21);
}

void interrupt winNTint29(
	unsigned _es, unsigned _ds,
	unsigned _di, unsigned _si,
	unsigned _bp, unsigned _sp,
	unsigned _bx, unsigned _dx,
	unsigned _cx, unsigned _ax,
	)
{
	char	ch;
	WORD	buf_seg;
#ifdef DEBUG_INT_CALLS
	int29calls++;
#endif

	ch=_ax&0xff;
	_asm mov buf_seg, ss
	vdd_buf(VDD_WRITE, 1, buf_seg, (WORD)&ch);

	_chain_intr(oldint29);
}

/****************************************************************************/
/* Return the filename portion of a full pathname							*/
/****************************************************************************/
char* getfname(const char* path)
{
	const char* fname;
	const char* bslash;

	fname=strrchr(path,'/');
	bslash=strrchr(path,'\\');
	if(bslash>fname)
		fname=bslash;
	if(fname!=NULL) 
		fname++;
	else 
		fname=(char*)path;
	return((char*)fname);
}

char *	DllName		=VDD_FILENAME;
char *	InitFunc	="VDDInitialize";
char *	DispFunc	="VDDDispatch";
#define MAX_ENVVARS 32
#define MAX_ARGS	32

/* Wait before installing DOS handlers/spawning a game. The host releases this
 * gate only after verifying the pinned System32 file and actual VDM module.
 * VDD_PEEK/READ/WRITE use unchanged bounded private session channels. */
static int sfx_wait_start(const char* session) {
    char expected[SFX_GATE_BOUND],received[SFX_GATE_BOUND],ack[SFX_GATE_BOUND];
    WORD segment;int available,got;time_t begin=time(NULL),now;
    sprintf(expected,SFX_GATE_PREFIX "%s|%u",session,node_num);
    _asm mov segment,ss;
    printf("SFXLOAD core open; awaiting host System32 identity approval\n");fflush(stdout);
    do {
        if(vdd_op(VDD_ONLINE)!=1) return 0;
        available=vdd_op(VDD_INBUF_FULL);
        if(available<0 || available>SFX_GATE_BOUND-1) return 0;
        if(available>=(int)strlen(expected)) {
            got=vdd_buf(VDD_READ,available,segment,(WORD)received);
            if(!sfx_gate_matches(received,(unsigned)got,expected)) return 0;
            sprintf(ack,SFX_GATE_ACK "%s|%u",session,node_num);
            if(vdd_buf(VDD_WRITE,strlen(ack),segment,(WORD)ack)!=(int)strlen(ack)) return 0;
            printf("SFXLOAD host identity approval accepted; game may start\n");fflush(stdout);
            return 1;
        }
        vdd_op(VDD_YIELD);now=time(NULL);
    } while(now>=begin && now-begin<10);
    return 0;
}

/* Split outside quotes, retaining argument quotes for the DOS command tail
 * reconstructed by _spawnvp. The game's parser consumes those quotes. */
static int sfx_arguments(char* line,char** args,int capacity)
{
    char* p=line;
    int count=0,quoted;
    while(*p) {
        while(*p==' ' || *p=='\t')p++;
        if(!*p)break;
        if(count>=capacity-1)return 0;
        args[count++]=p;quoted=0;
        while(*p) {
            if(*p=='"')quoted=!quoted;
            if(!quoted && (*p==' ' || *p=='\t'))break;
            p++;
        }
        if(quoted)return 0;
        if(*p)*p++=0;
    }
    if(!count)return 0;
    if(args[0][0]=='"') {
        size_t length=strlen(args[0]);
        if(length<3 || args[0][length-1]!='"')return 0;
        args[0][length-1]=0;args[0]++;
    }
    args[count]=0;return count;
}
int main(int argc, char **argv)
{
	char	str[128];
	char	cmdline[128],*p;
	char	exec_dir[128];
	char*	envvar[MAX_ENVVARS];
	char*	arg[MAX_ARGS];
	char*	ini_fname = exec_dir;
	int		i,envnum=0;
	int		mode = SBBSEXEC_MODE_UNSPECIFIED;
	int		argn;
	FILE*	fp;
	BOOL	x64=FALSE;
	BOOL	success=FALSE;
	WORD	buf_seg;
	WORD	w;
    WORD load_ds,load_ss;unsigned readback_ok=1,j;

	sprintf(id_string,"SFX FOSSIL Driver (SFXDOOR) revision %u %s/%s", DOSXTRN_REVISION, GIT_BRANCH, GIT_HASH);
	if(argc!=7) {
		fprintf(stderr
			,"%s - Copyright %s Rob Swindell\n"
			,id_string, __DATE__+7);
		fprintf(stderr
			,"usage: SFXDOOR <private.env> NT <node1-128> <mode> <private.ini> <session32hex>\n");
		return(1);
	}

	if(strcmp(argv[2],"NT") || !sfx_valid_session(argv[6])) return 10;
	{
		char* end;long node=strtol(argv[3],&end,10);
		if(!*argv[3] || *end || node<1 || node>SFX_NODE_MAX) return 11;
	}
	if(strcmp(argv[4],"3") && strcmp(argv[4],"4") && strcmp(argv[4],"8") && strcmp(argv[4],"12")) return 17;
    if(strlen(argv[0])>=sizeof(exec_dir)-16 || strlen(argv[1])<4) return 12;
	sprintf(exec_dir,"%.*s",sizeof(exec_dir)-1,argv[0]);
	p=getfname(exec_dir);
	*p=0;
    /* Native System32 convention: one exact basename, no alternate attempts.
     * Host pins the resolved System32 target and verifies actual load identity
     * before sending SFXSTART2. Registration alone never permits a game. */
    DllName=VDD_FILENAME;

	argn = 2;
	if(argn < argc) {
		if(strcmp(argv[argn],"x64") == 0)
			x64=TRUE;
		argn++;
	}
	if(argn < argc && IS_DIGIT(argv[argn][0])) {
		node_num=atoi(argv[argn]);
		argn++;
	}
	if(argn < argc && IS_DIGIT(argv[argn][0])) {
		mode = atoi(argv[argn]);
		argn++;
	}
	if(argn < argc) {
		ini_fname = argv[argn];
		argn++;
	}

	if(mode == SBBSEXEC_MODE_UNSPECIFIED)
		mode = SBBSEXEC_MODE_DEFAULT;

	if((fp=fopen(argv[1],"r"))==NULL) {
		fprintf(stderr,"!Error opening %s\n",argv[1]);
		return(2);
	}

	if(!fgets(cmdline,sizeof(cmdline),fp)) {fclose(fp);return 15;}
	truncsp(cmdline);

	if(!sfx_arguments(cmdline,arg,MAX_ARGS)) {fclose(fp);return 20;}

	while(!feof(fp) && envnum < MAX_ENVVARS) {
		if(!fgets(str, sizeof(str), fp))
			break;
		truncsp(str);
		if((envvar[envnum]=strdup(str))==NULL) {
			fprintf(stderr,"!MALLOC ERROR\n");
			return(4);
		}
		_putenv(envvar[envnum++]);
	}
	fclose(fp);

	/* Save int14 handler */
	oldint14=_dos_getvect(0x14);

	/* Overwrite stub function */
	((BYTE*)int14stub)[0] = 0xe9;	/* jump (relative) */
	((BYTE*)int14stub)[3] = 0x90;	/* NOP */
	((BYTE*)int14stub)[4] = 0x90;	/* NOP */
	((BYTE*)int14stub)[5] = 0x90;	/* NOP */
	((BYTE*)int14stub)[6] = FOSSIL_SIGNATURE&0xff;	/* FOSSIL sig (LSB) */
	((BYTE*)int14stub)[7] = FOSSIL_SIGNATURE>>8;	/* FOSSIL sig (MSB) */
	((BYTE*)int14stub)[8] = FOSSIL_FUNC_HIGHEST;	/* FOSSIL highest func supported */

	for(i=0;i<1;i++) {

        _asm mov load_ds,ds;
        _asm mov load_ss,ss;
        for(j=0;j<=strlen(DllName);++j)if(((char far*)_MK_FP(load_ds,(WORD)DllName))[j]!=DllName[j])readback_ok=0;
        printf("SFXLOAD System32-contract registration name=%s node=%u DS=%04x SS=%04x SI=%04x readback=%u init=0000:0000\n",DllName,node_num,load_ds,load_ss,(unsigned)DllName,readback_ok);fflush(stdout);
        if(!readback_ok){printf("SFXLOAD DS:SI readback mismatch; registration/game refused\n");return 18;}
        /* Register VDD */
       	_asm {
			push	es
			push	ds
			pop		es
			mov     si, DllName		; ds:si = dll name
			mov     di, InitFunc    ; es:di = init routine
			mov     bx, DispFunc    ; ds:bx = dispatch routine
		};
		if(!x64) {	// NTVDMx64 (based on an older Windows NTVDM) requires an init routine
			_asm {	/* Vista work-around, apparently doesn't support an InitFunc (RegisterModule fails with AX=1) */
				xor		di,di
				mov		es,di
			};
		}
		RegisterModule();
		_asm {
			mov		vdd, ax
			jc		err
			mov		success, TRUE
			err:
			pop		es
		}
		if(success)
			break;
		/* Exactly one approved basename registration; no fallback driver/probe. */
	}
	if(!success) {
		printf("SFXLOAD System32 RegisterModule FAILED BOP AX=%u path=%s; not a Win32 error\n",(unsigned)vdd,DllName);fflush(stdout);
        return 18;
	}

#if 0
	fprintf(stderr,"vdd handle=%d\n",vdd);
	fprintf(stderr,"mode=%d\n",mode);
#endif
	if(vdd_op(SFX_VDD_PAIR)!=SFX_PAIR_WORD || vdd_str(SFX_VDD_BIND,argv[6])!=SFX_PAIR_WORD) {
		printf("SFXLOAD FAILED stage=pair_or_bind; game not started\n");fflush(stdout);
		_asm mov ax,vdd;
		UnRegisterModule();return 13;
	}
	if(vdd_str(VDD_LOAD_INI_FILE,ini_fname)!=0) {
        fprintf(stderr,"!Private SFX INI unavailable\n");
        _asm mov ax,vdd;
        UnRegisterModule();return 16;
    }

	printf("SFXLOAD pair accepted; private INI accepted; opening core VDD\n");fflush(stdout);
	vdd_str(VDD_LOAD_INI_SECTION, getfname(arg[0]));

	sprintf(str,"%s, rev %u %s/%s, %s %s mode=%u", __FILE__, DOSXTRN_REVISION, GIT_BRANCH, GIT_HASH, __DATE__, __TIME__, mode);
	vdd_str(VDD_DEBUG_OUTPUT, str);

	if(mode & SBBSEXEC_MODE_UART)
		vdd_op(VDD_VIRTUALIZE_UART);
	i=vdd_op(VDD_OPEN);
	if(i) {
		printf("SFXLOAD FAILED stage=VDD_OPEN code=%d; game not started\n",i);fflush(stdout);
		_asm mov ax,vdd;
		UnRegisterModule();
		return(-1);
	}
    if(!sfx_wait_start(argv[6])) {
        printf("SFXLOAD FAILED stage=host_identity_gate; game not started\n");fflush(stdout);
        vdd_op(VDD_CLOSE);
        _asm mov ax,vdd;
        UnRegisterModule();return 19;
    }
	oldint16=_dos_getvect(0x16);
	oldint21=_dos_getvect(0x21);
	oldint29=_dos_getvect(0x29);
	if(mode & SBBSEXEC_MODE_FOSSIL) {
		*(WORD*)((BYTE*)int14stub+1) = (WORD)winNTint14 - (WORD)&int14stub - 3;	/* jmp offset */
		_dos_setvect(0x14,(void(interrupt *)())int14stub); 
	}
	_dos_setvect(0x21,winNTint21); 
	if(mode&SBBSEXEC_MODE_DOS_IN)
		_dos_setvect(0x16,winNTint16); 
	if(mode&SBBSEXEC_MODE_DOS_OUT) 
		_dos_setvect(0x29,winNTint29); 

	_heapmin();
	i=_spawnvp(_P_WAIT, arg[0], arg);

	p=argv[1]+(strlen(argv[1])-3);
	strcpy(p,"RET");
	if((fp=fopen(argv[1],"w+"))==NULL) {
		fprintf(stderr,"!Error opening %s\n",argv[1]);
	} else {
		fprintf(fp,"%d",i);
		fclose(fp);
	}
	strcpy(p,"ERR");
	if(i == -1) {
		if((fp=fopen(argv[1],"w+"))==NULL) {
			fprintf(stderr,"!Error opening %s\n",argv[1]);
		} else {
			fprintf(fp,"%d\n%s\n", errno, strerror(errno));
			fclose(fp);
		}
	} else
		remove(argv[1]);

	/* Restore original ISRs */
	_dos_setvect(0x14,oldint14);

	if(vdd_op(VDD_CLOSE)!=0) {fprintf(stderr,"!SFX close failed; host must end private VDM\n");return 14;}

	_dos_setvect(0x16,oldint16);
	_dos_setvect(0x21,oldint21);
	_dos_setvect(0x29,oldint29);

	sprintf(str,"%s returned %d", arg[0], i);
	vdd_str(VDD_DEBUG_OUTPUT, str);

#ifdef DEBUG_INT_CALLS
	sprintf(str,"int14h calls: %lu", int14calls);	vdd_str(VDD_DEBUG_OUTPUT, str);
	sprintf(str,"int16h calls: %lu", int16calls);	vdd_str(VDD_DEBUG_OUTPUT, str);
	sprintf(str,"int21h calls: %lu", int21calls);	vdd_str(VDD_DEBUG_OUTPUT, str);
	sprintf(str,"int29h calls: %lu", int29calls);	vdd_str(VDD_DEBUG_OUTPUT, str);
#endif
#ifdef DEBUG_DOS_CALLS
	for(i=0;i<0x100;i++) {
		if(dos_calls[i]>100) {
			sprintf(str,"int21h function %02X calls: %u"
				,i, dos_calls[i]);
			vdd_str(VDD_DEBUG_OUTPUT, str);
		}
	}
#endif
#ifdef DEBUG_FOSSIL_CALLS
	for(i=0;i<0x100;i++) {
		if(fossil_calls[i]>0) {
			sprintf(str,"int14h function %02X (%-10s) calls: %lu"
				,i, fossil_func(i), fossil_calls[i]);
			vdd_str(VDD_DEBUG_OUTPUT, str);
		}
	}
#endif

	/* Unregister VDD */
	_asm mov ax, vdd;
	UnRegisterModule();

	return(i);
}
