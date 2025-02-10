#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "systemcalls.h"

/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{

/*
 * TODO  add your code here
 *  Call the system() function with the command set in the cmd
 *   and return a boolean true if the system() call completed with success
 *   or false() if it returned a failure
*/

    // system() returns 0 if successful
    return system(cmd) == 0;

    // return true;
}

// do_exec and do_exec_redirect helper function
bool _do_exec(const char* outputfile, char* command[]) {
    // create child process and save its pid
    pid_t pid = fork();

    // check if child process failed to create (fork will return -1)
    if (pid < 0) return false;

    // check if current process is parent or child (child process has pid of 0)
    if (pid != 0) {
        // wait for child process to exit and get its pid and exit status
        int status;
        pid_t child_pid = waitpid(pid, &status, 0);
        
        // check if child process existed (child pid must be a non-zero positive integer)
        if (child_pid < 0) return false;

        // check if child process ran and exited successfully (generally success exit codes are 0)
        if (WEXITSTATUS(status) != 0) return false;
    } else {
        // redirect and write contents of stdout to outputfile if outputfile is specified
        if (outputfile != NULL) {
            // create using permission bits 0644 and/or open outputfile and save file descriptor
            const int fd = open(outputfile, O_WRONLY|O_TRUNC|O_CREAT, S_IWUSR|S_IRUSR|S_IRGRP|S_IROTH);

            // duplicates fd to file descriptor 1, forcing stdout to write to outputfile when file descriptor 1 opens
            // file descriptors 0, 1, and 2 are reserved and open on stdin, stdout, and stderr respectively
            if (dup2(fd, 1) < 0) return false;

            close(fd);
        }

        // allow child process to execute command along with arguments
        execv(command[0], &command[0]);

        // exit process with failure exit code as process would not reach this point if exec was successful
        exit(EXIT_FAILURE);
    }

    return true;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;
    // this line is to avoid a compile warning before your implementation is complete
    // and may be removed
    // command[count] = command[count];

/*
 * TODO:
 *   Execute a system command by calling fork, execv(),
 *   and wait instead of system (see LSP page 161).
 *   Use the command[0] as the full path to the command to execute
 *   (first argument to execv), and use the remaining arguments
 *   as second argument to the execv() command.
 *
*/

    // get return status of exec
    bool status = _do_exec(NULL, command);

    va_end(args);

    return status;
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;
    // this line is to avoid a compile warning before your implementation is complete
    // and may be removed
    // command[count] = command[count];


/*
 * TODO
 *   Call execv, but first using https://stackoverflow.com/a/13784315/1446624 as a refernce,
 *   redirect standard out to a file specified by outputfile.
 *   The rest of the behaviour is same as do_exec()
 *
*/

    // get return status of exec
    bool status = _do_exec(outputfile, command);

    va_end(args);

    return status;
}
