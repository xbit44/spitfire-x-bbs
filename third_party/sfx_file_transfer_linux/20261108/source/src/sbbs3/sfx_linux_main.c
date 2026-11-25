/* SFX Linux locked general-file helper, November 8 2026.
 * GPL-2.0-or-later. Derived from the accepted SFX SEXYZ fork;
 * Copyright Rob Swindell and contributors. See retained notices/licenses.
 * This is a separate executable, never linked into SFX. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "genwrap.h"
#include "dirwrap.h"
#include "sexyz.h"
#include "git_hash.h"
#include "sfx_linux_platform.inc"
#include "sfx_linux_completion.inc"
#include "sfx_file_transfer.inc"
int main(int argc,char** argv)
{
    if(argc==2&&!strcmp(argv[1],"--version")){
        printf("SFX Linux general-file SEXYZ fork: file-job1 ZMODEM/file-job2 X/YMODEM, upstream %s, GPL-2.0-or-later; Copyright Rob Swindell and contributors\n",GIT_HASH);
        return 0;
    }
    if(!sfx_private_process())return SFX_LOCAL_IO;
    if(argc==3&&!strcmp(argv[1],"--sfx-file-receive-v1"))return sfx_file_transfer_main(argc,argv,FALSE);
    if(argc==3&&!strcmp(argv[1],"--sfx-file-send-v1"))return sfx_file_transfer_main(argc,argv,TRUE);
    if(argc==4&&!strcmp(argv[1],"--sfx-file-receive-v2"))return sfx_file_transfer_main(argc,argv,FALSE);
    if(argc==4&&!strcmp(argv[1],"--sfx-file-send-v2"))return sfx_file_transfer_main(argc,argv,TRUE);
    return SFX_LOCAL_IO;
}
