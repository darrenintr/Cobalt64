/* SPDX-License-Identifier: MIT
 * launchd entry point for the Navi33 boot autorun. It only spawns
 * /bin/sh on the root-owned navi33-autorun.sh and waits. Being a separate
 * binary matters for macOS privacy (TCC): the script's access to the USB
 * stick is attributed to this file, so if macOS refuses it, Full Disk
 * Access can be granted to this one binary instead of to /bin/sh.
 */
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>

extern char **environ;

int main(void) {
    char *argv[]={"sh","/usr/local/libexec/cobalt64/navi33-autorun.sh",NULL};
    pid_t pid;
    int status=0;
    const int err=posix_spawn(&pid,"/bin/sh",NULL,NULL,argv,environ);
    if(err) {
        errno=err;
        perror("n33autorun: posix_spawn /bin/sh");
        return 1;
    }
    while(waitpid(pid,&status,0)<0)
        if(errno!=EINTR)return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
