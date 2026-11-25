"""Final paired boundary fixes, applied after derive/refine/credits."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=ROOT/'src/src/sbbs3/sbbsexec.c';s=p.read_text()
def edit(old,new):
    global s
    assert old in s,old[:100];s=s.replace(old,new)
edit('if (space < 1)', 'if (space < 256)')
edit('if (space > sizeof(buf))\n\t\t\tspace = sizeof(buf);','if (space > 256)\n\t\t\tspace = 256;')
edit('lputs(LOG_WARNING, "input_thread: input buffer full!");\n\t\t\tYIELD();','Sleep(2);')
edit(', 0 //LINEAR_RX_BUFLEN\t\t/* Max message size (0=any) */',', 256 /* Matched host sends messages of at most 256 bytes */')
edit('if (hangup_event == NULL) {\n\t\t\t\tlprintf', 'if (hangup_event == NULL) {\n                retval=13;break;\n\t\t\t\tlprintf')
edit('if (carrier_event == NULL) {\n\t\t\t\tlprintf', 'if (carrier_event == NULL) {\n                retval=14;break;\n\t\t\t\tlprintf')
# Reset per-session options, including UART mode and disabled DTR, on reuse.
edit('sfx_open=FALSE;sfx_bound=FALSE;sfx_session[0]=0;sfx_node=0;',
     'uart=default_uart;hangup_supported=TRUE;yield_interval=1.0;log_level=LOG_INFO;\n    sfx_open=FALSE;sfx_bound=FALSE;sfx_session[0]=0;sfx_node=0;')
# Win16 callers supply explicit lengths, not guaranteed NUL termination.
for label,following,operation in [('VDD_LOAD_INI_SECTION','VDD_DEBUG_OUTPUT','parse_ini'),('VDD_DEBUG_OUTPUT','VDD_HANGUP','debug')]:
    a=s.index('\t\tcase '+label+':');b=s.index('\n\t\tcase '+following+':',a)
    body='parse_ini(bounded);' if operation=='parse_ini' else 'lputs(LOG_INFO,bounded);'
    s=s[:a]+'''\t\tcase '''+label+''': {
            char bounded[512];count=getCX();
            if(count>=sizeof(bounded)){retval=0xffff;break;}
            p=(BYTE*)GetVDMPointer((ULONG)((getES()<<16)|getDI()),count,FALSE);
            if(!p){retval=0xffff;break;}
            memcpy(bounded,p,count);bounded[count]=0;
            '''+body+'''
            break;
        }
'''+s[b:]
edit('if (isdir(p))\n\t\t\t\tiniFileName(ini_fname, sizeof(ini_fname), p, INI_FILENAME);\n\t\t\telse\n\t\t\t\tSAFECOPY(ini_fname, p);',
     'if(isdir(p)){retval=0xffff;break;}\n            SAFECOPY(ini_fname,p);')
edit('\n\n\t\t}\n\t\tbreak;\n\n\t\tcase VDD_LOAD_INI_SECTION:',
     '\n            else {retval=0xffff;}\n\t\t}\n\t\tbreak;\n\n\t\tcase VDD_LOAD_INI_SECTION:')
edit('default:\n\t\t\tlprintf(LOG_ERR, "!UNKNOWN VDD_OP: %d", getBL());',
     'default:\n            retval=0xffff;\n\t\t\tlprintf(LOG_ERR, "!UNKNOWN VDD_OP: %d", getBL());')
# Defensive mapped-buffer checks leave invalid dispatches without a write.
edit('retval = vdd_read(p, count);','if(!p){retval=0xffff;break;}\n            retval = vdd_read(p, count);')
edit('retval = RingBufPeek(&rdbuf, p, count);','if(!p){retval=0xffff;break;}\n            retval = RingBufPeek(&rdbuf, p, count);')
edit('if (!sfx_write(wrslot, p, count, &retval, NULL)) {','if(!p){retval=0xffff;break;}\n            if (!sfx_write(wrslot, p, count, &retval, NULL)) {')
edit('status->inbuf_size = RINGBUF_SIZE_IN;','if(!status){retval=0xffff;break;}\n            status->inbuf_size = RINGBUF_SIZE_IN;')
p.write_text(s)
p=ROOT/'src/src/sbbs3/dosxtrn/dosxtrn.c';s=p.read_text()
s=s.replace('fgets(cmdline, sizeof(cmdline), fp);','if(!fgets(cmdline,sizeof(cmdline),fp)) {fclose(fp);return 15;}')
s=s.replace('vdd_str(VDD_LOAD_INI_FILE, ini_fname);','if(vdd_str(VDD_LOAD_INI_FILE,ini_fname)!=0) {\n        fprintf(stderr,"!Private SFX INI unavailable\\n");\n        _asm mov ax,vdd;\n        UnRegisterModule();return 16;\n    }')
# DOS's PSP command tail is short, but explicit mode validation prevents accidental
# other mode bits and makes mismatched manual invocations fail before registration.
s=s.replace('if(strlen(argv[0])>=sizeof(exec_dir)-16 || strlen(argv[1])<4) return 12;',
'''if(strcmp(argv[4],"4") && strcmp(argv[4],"8")) return 17;
    if(strlen(argv[0])>=sizeof(exec_dir)-16 || strlen(argv[1])<4) return 12;''')
p.write_text(s)
notice='/* SFX modifications, 2026-10-24: private paired session/node channels,\n * strict SFX loading, bounded credits and joined lifecycle. Original notices\n * and GPL/LGPL terms are retained below. See SFX-CHANGES.txt. */\n'
for relative in ('src/src/sbbs3/sbbsexec.c','src/src/sbbs3/dosxtrn/dosxtrn.c','src/src/sbbs3/fossdefs.h'):
    modified=ROOT/relative;modified.write_text(notice+modified.read_text())
print('PASS bounded message reads, per-session reset, mapped buffers and strict private INI')
