#include "systemcalls.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>

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
 *  Call the system() function with the command set in the cmd
 *   and return a boolean true if the system() call completed with success
 *   or false() if it returned a failure
*/
    int status = system(cmd);
    return status == 0;
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

/*
 *   Execute a system command by calling fork, execv(),
 *   and wait instead of system (see LSP page 161).
 *   Use the command[0] as the full path to the command to execute
 *   (first argument to execv), and use the remaining arguments
 *   as second argument to the execv() command.
 *
*/
    int    wstatus;
    pid_t  cpid;
    bool   result = false;

    cpid = fork();
    if (cpid == -1) {
        goto end_do_exec;
    }

    if (cpid == 0) {            /* Code executed by child */
        execv(command[0], command);
        // if execv returns, it failed
        exit(EXIT_FAILURE);
    }
    else {
        if (waitpid (cpid, &wstatus, 0) == -1) {
            goto end_do_exec;
        }
        else if (WIFEXITED(wstatus)) {
            result = WEXITSTATUS(wstatus) == 0;
        }
    }
    end_do_exec:

    va_end(args);

    return result;
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

/*
 *   Call execv, but first using https://stackoverflow.com/a/13784315/1446624 as a refernce,
 *   redirect standard out to a file specified by outputfile.
 *   The rest of the behaviour is same as do_exec()
 *
*/
    int    wstatus;
    pid_t  cpid;
    bool   result = false;

    int fd = open(outputfile, O_WRONLY|O_TRUNC|O_CREAT, 0644);
    if (fd < 0) { 
        goto end_no_file;
    }

    cpid = fork();
    if (cpid == -1) {
        goto end;
    }

    if (cpid == 0) {            /* Code executed by child */
        int dup_fd = dup2(fd, STDOUT_FILENO);
        if (dup_fd < 0) {
            exit(EXIT_FAILURE);
        }
        execv(command[0], command);
        // if execv returns, it failed
        exit(EXIT_FAILURE);
    } 
    
    if (waitpid (cpid, &wstatus, 0) == -1) {
        goto end;
    }
    else if (WIFEXITED(wstatus)) {
        result = WEXITSTATUS(wstatus) == 0;
    }

    
    end:

    close(fd);
    
    end_no_file:

    va_end(args);

    return result;
}
