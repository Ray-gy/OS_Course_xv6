#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int p1[2], p2[2];
  char buf[1];
  int pid;

  if(pipe(p1) < 0 || pipe(p2) < 0) {
    fprintf(2, "pipe failed\n");
    exit(1);
  }

  pid = fork();
  if(pid < 0) {
    fprintf(2, "fork failed\n");
    exit(1);
  }

  if(pid == 0) {
    close(p1[1]);
    close(p2[0]);
    
    read(p1[0], buf, 1);
    printf("%d: received ping\n", getpid());
    
    write(p2[1], buf, 1);
    close(p1[0]);
    close(p2[1]);
    exit(0);
  } else {
    close(p1[0]);
    close(p2[1]);
    
    buf[0] = 'x';
    write(p1[1], buf, 1);
    
    read(p2[0], buf, 1);
    printf("%d: received pong\n", getpid());
    
    close(p1[1]);
    close(p2[0]);
    wait(0);
  }

  exit(0);
}