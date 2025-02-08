#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>

int main(int argc, char *argv[]) {
    // begin logging to syslog
    openlog("writer", LOG_CONS | LOG_PID, LOG_USER);

    // check that number of arguments is correct
    if (argc < 3) {
        syslog(LOG_ERR, "Error: missing arguments");
        closelog();

        return 1;
    }

    // assign arguments to variables
    const char *filename = argv[1];
    const char *filestr = argv[2];
    FILE *fileptr;

    // open stream to file
    fileptr = fopen(filename, "w");

    // srite to file and save return code
    int rc = fprintf(fileptr, "%s\n", filestr);

    // check if file write was successful
    if (rc < 0) {
        syslog(LOG_ERR, "Error: failed to write to file");
    } else {
        syslog(LOG_DEBUG, "Writing %s to %s", filestr, filename);
    }

    // close and cleanup
    fclose(fileptr);
    closelog();

    return 0;
}
