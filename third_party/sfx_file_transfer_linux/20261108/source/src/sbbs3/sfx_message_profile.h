/* SFX general-file receiver fork, October 17 2026. Separate purpose-only exe.
 * GPL-2.0-or-later: same grant as the complete SEXYZ helper. No SFX linkage.
 * Names are metadata only. General-file payloads use host-inherited handles.
 * Historical packet branch is not exposed by this executable's CLI.
 * Original packet and 256 KiB text/art helpers remain unchanged elsewhere. */
#ifndef SFX_MESSAGE_PROFILE_H
#define SFX_MESSAGE_PROFILE_H
#define SFX_MESSAGE_CAP 16777216u
enum { SFX_METADATA=1, SFX_EXTRA_FILE, SFX_SIZE, SFX_COMPLETION,
       SFX_DISK, SFX_PROTOCOL, SFX_CANCEL, SFX_LOCAL_IO };
enum { SFX_REFUSE_NONE=0, SFX_REFUSE_EXTENSION=1, SFX_REFUSE_SIZE=2 };

static int sfx_fail(zmodem_t* zm, unsigned why)
{
    if (!zm->sfx_failure) zm->sfx_failure = why;
    return 0;
}

static int sfx_filename(const unsigned char* name, size_t n)
{
    char stem[129];
    size_t i, end;
    if (!n || n > 128 || name[0] == '.' || name[n-1] == '.' || name[n-1] == ' ')
        return 0;
    for (i=0; i<n; ++i) {
        unsigned c = name[i];
        if (c < 32 || c == 127 || strchr("/\\:<>\"|?*", c)) return 0;
        stem[i] = (char)toupper(c);
    }
    stem[n] = 0;
    for (end=0; end<n && stem[end]!='.'; ++end) {}
    /* Windows device aliases also include an extension or spaces before it. */
    while (end && stem[end-1]==' ') --end;
    stem[end] = 0;
    if (!strcmp(stem,"CON") || !strcmp(stem,"PRN") || !strcmp(stem,"AUX")
        || !strcmp(stem,"NUL") || !strcmp(stem,"CONIN$") || !strcmp(stem,"CONOUT$")) return 0;
    if (end==4 && (!strncmp(stem,"COM",3) || !strncmp(stem,"LPT",3))
        && stem[3]>='0' && stem[3]<='9') return 0;
    return 1;
}

/* Policy is separate from structural validity. Match only the final extension;
 * the host still validates received text/ANSI content before any post. */
static int sfx_message_extension(const unsigned char* name, size_t n)
{
    unsigned char a,b,c;
    if (n<5 || name[n-4]!='.') return 0;
    a=(unsigned char)tolower(name[n-3]);
    b=(unsigned char)tolower(name[n-2]);
    c=(unsigned char)tolower(name[n-1]);
    return (a=='r' && b=='e' && c=='p') || (a=='z' && b=='i' && c=='p');
}

/* Accept 1..7 bounded unsigned numeric metadata fields: size decimal,
 * timestamp/mode octal, serial/files/remaining bytes/trailing serial decimal.
 * Missing optional fields are permitted, malformed/overflow/extra data is not.
 * Padding after the metadata terminator must be zero. No strlen or sscanf on
 * an untrusted unterminated subpacket. */
static int sfx_zfile(zmodem_t* zm)
{
    const unsigned char* p=zm->rx_data_subpacket;
    size_t n=zm->rx_data_subpacket_len, name=0, pos, end, i;
    uint64_t fields[7]={0};
    unsigned count=0;
    while (name<n && p[name]) ++name;
    if (name==n || !sfx_filename(p,name)) return sfx_fail(zm,SFX_METADATA);
    pos=name+1; end=pos;
    while (end<n && p[end]) ++end;
    if (end==n || end-pos>128) return sfx_fail(zm,SFX_METADATA);
    for (i=end+1; i<n; ++i) if (p[i]) return sfx_fail(zm,SFX_METADATA);
    while (pos<end) {
        unsigned digits=0, base;
        uint64_t value=0;
        if (count>=7) return sfx_fail(zm,SFX_METADATA);
        base=(count==1 || count==2)?8:10;
        while (pos<end && p[pos]!=' ') {
            unsigned digit=p[pos++]-'0';
            if (digit>=base || ++digits>20 || value>(UINT64_MAX-digit)/base)
                return sfx_fail(zm,SFX_METADATA);
            value=value*base+digit;
        }
        if (!digits) return sfx_fail(zm,SFX_METADATA);
        fields[count++]=value;
        while (pos<end && p[pos]==' ') ++pos;
    }
    if (!count || (!fields[0]&&!zm->sfx_files)) return sfx_fail(zm,SFX_SIZE);
    if (count>4 && fields[4]>(zm->sfx_files?zm->sfx_offer_limit:1)) return sfx_fail(zm,SFX_EXTRA_FILE);
    if (!zm->sfx_files && count>5 && fields[5] && fields[5]!=fields[0]) return sfx_fail(zm,SFX_METADATA);
    if ((count>1 && fields[1]>UINT32_MAX) || (count>2 && fields[2]>UINT32_MAX)
        || (count>3 && fields[3]>UINT32_MAX) || (count>6 && fields[6]>UINT32_MAX))
        return sfx_fail(zm,SFX_METADATA);
    memcpy(zm->current_file_name,p,name);
    zm->current_file_name[name]=0;
    /* A well-formed but disallowed offer can be skipped without requesting
     * data. Oversize takes precedence if both policy checks fail. Never cast
     * an unbounded uint64 announcement to the signed file-size fields. */
    if(zm->sfx_files){
        if(!zm->sfx_file_offer)return sfx_fail(zm,SFX_LOCAL_IO);
        zm->sfx_refusal=zm->sfx_file_offer(zm->cbdata,zm->current_file_name,fields[0]);
        if(!zm->sfx_refusal&&(fields[0]>zm->sfx_file_cap||zm->sfx_file_cap>1073741824ULL))return sfx_fail(zm,SFX_SIZE);
    }else zm->sfx_refusal=fields[0]>SFX_MESSAGE_CAP?SFX_REFUSE_SIZE:
        (sfx_message_extension(p,name)?SFX_REFUSE_NONE:SFX_REFUSE_EXTENSION);
    zm->current_file_size=zm->sfx_refusal?0:(int64_t)fields[0];
    zm->current_file_time=(time_t)fields[1];
    zm->files_remaining=zm->total_files=1;
    zm->bytes_remaining=zm->total_bytes=zm->current_file_size;
    return 1;
}
#endif
