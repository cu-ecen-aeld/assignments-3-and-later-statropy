#include <syslog.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int main(int argc, const char* argv[])
{
    int retval = 1;
    openlog(NULL,0,LOG_USER);

    if(argc != 3)
    {
        syslog(LOG_ERR, "Incorrect number of arguments: %d", argc);
        fprintf(stderr, "usage: writer <writefile> <writestr>\n");
        goto end;
    }

    const char *writefile = argv[1];
    const char *writestr = argv[2];

    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    FILE *f = fopen(writefile, "w");
    if (f == NULL)
    {
        syslog(LOG_ERR, "Could not open file: %s", writefile);
        goto end;
    }
    int written = fputs(writestr, f);
    if (written == EOF)
    {
        syslog(LOG_ERR, "could not write %s to %s", writestr, writefile);
        goto end;
    }
    fclose(f);

    retval = 0;
end:
    closelog();
    return retval;
}
