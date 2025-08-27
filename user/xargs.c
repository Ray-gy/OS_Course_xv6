#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  char buf[512];
  char *args[32];
  int i, j;
  
  if(argc < 2){
    fprintf(2, "Usage: xargs command [args...]\n");
    exit(1);
  }
  
  for(i = 1; i < argc; i++){
    args[i-1] = argv[i];
  }
  
  while(1){
    i = 0;
    
    while(1){
      int n = read(0, &buf[i], 1);
      if(n <= 0){
        if(i > 0){
          buf[i] = 0;
          break;
        }
        exit(0);
      }
      if(buf[i] == '\n'){
        buf[i] = 0;
        break;
      }
      i++;
      if(i >= sizeof(buf) - 1){
        fprintf(2, "xargs: line too long\n");
        exit(1);
      }
    }
    
    if(i == 0) continue;
    
    // Parse the line into arguments and add to args array
    j = argc - 1; // Start adding after command line args
    char *p = buf;
    while(*p){
      // Skip whitespace
      while(*p == ' ' || *p == '\t') p++;
      if(*p == 0) break;
      
      args[j++] = p;
      if(j >= sizeof(args)/sizeof(args[0]) - 1){
        fprintf(2, "xargs: too many arguments\n");
        exit(1);
      }
      
      // Find end of current argument
      while(*p && *p != ' ' && *p != '\t') p++;
      if(*p){
        *p = 0;
        p++;
      }
    }
    args[j] = 0; // Null terminate args array
    
    // Fork and execute
    if(fork() == 0){
      exec(args[0], args);
      fprintf(2, "xargs: exec %s failed\n", args[0]);
      exit(1);
    } 
    else{
      wait(0);
    }
  }
}