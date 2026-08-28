/* host_main.c - Linux AmiPart entry point (image-file mode).
 *
 * The CLI layer (src/cli.c) is shared verbatim with the Amiga build; it
 * reads its arguments through ReadArgs, which the host shim implements
 * over argv (host_rdargs.c).  There is no GUI on the host: an empty
 * command line prints the usage pointer instead of opening a window. */
#include <stdio.h>
#include "amiga_compat.h"
#include "cli.h"
#include "locale_support.h"

/* argv handed to the shim's ReadArgs */
int          host_argc;
char       **host_argv;

int main(int argc, char **argv)
{
    LONG rc;

    host_argc = argc;
    host_argv = argv;

    LocaleOpen();
    rc = cli_run();
    LocaleClose();

    if (rc == CLI_NO_ARGS) {
        printf("AmiPart (host build, image-file mode)\n"
               "Usage: amipart ?             for the argument template\n"
               "       amipart IMAGE=<file> <command...>\n"
               "See cmdline.txt for all commands.\n");
        return 5;
    }
    return (int)rc;
}
