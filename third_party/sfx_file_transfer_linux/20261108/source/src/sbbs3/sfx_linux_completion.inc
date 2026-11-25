/* GPL-2.0-or-later. Exact strict completion routines from the accepted
 * SFX SEXYZ fork; original notices retained in sibling source files. */
/* Strict refusal completion cannot use the ordinary resynchronizing scanner.
 * Read exactly one header, with no scan/retry after an unexpected byte. The
 * fixed frame is at most 24 raw bytes before optional flow control. Bound the
 * entire parser to 32 raw bytes and four XON/XOFF bytes (including the ZCRCW
 * metadata's trailing XON). Parity XON/XOFF is legal outside an escape pair;
 * it never resets this budget. This local TCP profile needs no arbitrary
 * serial-line padding/noise recovery. Existing 60s/300s cancellation remains.
 * Binary16/32, hex16, one/two ZPAD, legal binary escapes and hex LF/CRLF with
 * parity terminators are supported. No bytes after the header are consumed.
 */
typedef struct {
    zmodem_t* zm; unsigned raw, flow;
    const BYTE* memory; unsigned size, pos;
} sfx_end_reader;
static int sfx_end_raw(sfx_end_reader* r)
{
    int c;
    if (r->raw>=32 || r->zm->sfx_failure) return -1;
    ++r->raw;
    if (r->memory) {
        if (r->pos>=r->size) { sfx_fail(r->zm,SFX_PROTOCOL); return -1; }
        return r->memory[r->pos++];
    }
    if (sfx_cancelled(r->zm->cbdata)) return -1;
    c=r->zm->recv_byte(r->zm->cbdata,10);
    if (c<0) sfx_fail(r->zm,SFX_PROTOCOL);
    return c;
}
static int sfx_end_byte(sfx_end_reader* r)
{
    int c;
    for (;;) {
        c=sfx_end_raw(r);
        if (c!=XON && c!=(XON|0x80) && c!=XOFF && c!=(XOFF|0x80)) return c;
        if (++r->flow>4) { sfx_fail(r->zm,SFX_PROTOCOL); return -1; }
    }
}
static int sfx_end_binary(sfx_end_reader* r)
{
    int c=sfx_end_byte(r);
    if (c!=ZDLE) return c;
    c=sfx_end_raw(r);
    if (c==ZRUB0) return 0x7f;
    if (c==ZRUB1) return 0xff;
    if (c>=0 && (c&0x60)==0x40) return c^0x40;
    sfx_fail(r->zm,SFX_PROTOCOL); return -1;
}
static int sfx_end_nibble(sfx_end_reader* r)
{
    int c=sfx_end_byte(r);
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    sfx_fail(r->zm,SFX_PROTOCOL); return -1;
}
static int sfx_end_header(sfx_end_reader* r)
{
    zmodem_t* zm=r->zm;
    unsigned char data[9];
    unsigned i, count;
    int c,style;
    uint16_t crc16=0;
    uint32_t crc32=0xffffffffU,received;
    if (sfx_end_byte(r)!=ZPAD) goto bad;
    c=sfx_end_byte(r);
    if (c==ZPAD) c=sfx_end_byte(r);
    if (c!=ZDLE) goto bad;
    style=sfx_end_byte(r);
    if (style!=ZHEX && style!=ZBIN && style!=ZBIN32) goto bad;
    count=style==ZBIN32?9:7;
    for (i=0;i<count;++i) {
        if (style==ZHEX) {
            int high=sfx_end_nibble(r),low=sfx_end_nibble(r);
            if (high<0 || low<0) goto bad;
            c=(high<<4)|low;
        } else c=sfx_end_binary(r);
        if (c<0) goto bad;
        data[i]=(unsigned char)c;
        if (i<5) { crc16=(uint16_t)ucrc16(c,crc16); crc32=ucrc32(c,crc32); }
    }
    if (style==ZBIN32) {
        received=(uint32_t)data[5]|((uint32_t)data[6]<<8)|((uint32_t)data[7]<<16)|((uint32_t)data[8]<<24);
        if (received!=~crc32) goto bad;
    } else if (((unsigned)data[5]<<8|data[6])!=crc16) goto bad;
    if (style==ZHEX) {
        c=sfx_end_byte(r);
        if (c=='\r' || c==('\r'|0x80)) c=sfx_end_byte(r);
        if (c!='\n' && c!=('\n'|0x80)) goto bad;
    }
    if (zm->sfx_failure) goto bad;
    if ((data[1] || data[2] || data[3] || data[4])
        && !(zm->sfx_packet_send && data[0]==ZRINIT)) goto bad;
    return data[0];
bad:
    sfx_fail(zm,SFX_PROTOCOL); return -1;
}
static BOOL sfx_refusal_zfin(zmodem_t* zm)
{
    sfx_end_reader r={zm,0,0,NULL,0,0};
    if (sfx_end_header(&r)==ZFIN) return TRUE;
    sfx_fail(zm,SFX_PROTOCOL); return FALSE;
}

/* The upstream initial scanner can skip arbitrary noise. That scanner alone
 * cannot prove a safe empty transfer. Reparse every actually consumed initial
 * byte, without scanning or I/O: optional exact "rz" startup line, zero to
 * eight zero-field CRC-valid ZRQINITs, and one zero-field CRC-valid ZFIN.
 * The whole prefix is capped at 256 bytes; every header retains the fixed
 * 32-byte/four-flow-control bound. No malformed prefix can become an empty
 * completion by resynchronizing to a later valid end. No file path uses this.
 */
static BOOL sfx_empty_prefix(zmodem_t* zm,const sfx_channel* ch)
{
    unsigned pos=0, queries=0;
    if (ch->initial_overflow || !ch->initial_size || ch->initial_size>sizeof(ch->initial)) goto bad;
    if (ch->initial_size>=3 && ch->initial[0]=='r' && ch->initial[1]=='z' && ch->initial[2]=='\r') {
        pos=3;
        if (pos<ch->initial_size && ch->initial[pos]=='\n') ++pos;
    }
    for (;;) {
        sfx_end_reader r={zm,0,0,ch->initial+pos,ch->initial_size-pos,0};
        int type=sfx_end_header(&r);
        pos+=r.pos;
        if (type==ZFIN && pos==ch->initial_size && !zm->sfx_failure) return TRUE;
        if (type!=ZRQINIT || ++queries>8 || pos>=ch->initial_size) goto bad;
    }
bad:
    sfx_fail(zm,SFX_PROTOCOL); return FALSE;
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
