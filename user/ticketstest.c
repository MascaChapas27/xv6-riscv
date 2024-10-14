#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"
#include "kernel/pstat.h"

#define MAX_CHILDREN 32
#define DEFAULT_CHILDREN 2

float test_child(){
  float aux = 0;
  for(int i = 0; i < 999999; i++){
    aux += 2.0 * 4.0;
  }
  return aux;
}

int main(int argc, char *argv[]){

  int childs;
  if(argc == 2){
    childs = atoi(argv[1]);
    if(childs < 0 || childs > MAX_CHILDREN){
      childs = DEFAULT_CHILDREN;
    }
  }
  else{
    childs = DEFAULT_CHILDREN;
  }
  settickets((childs+1)*10);

  int pid;

  for(int i = 0; i < childs; i++){
    pid = fork();
    switch(pid){
      case -1:
        fprintf(1, "Error, Fork - testsettickets\n");
        exit(1);
        break;
      case 0:
        settickets((childs-i)*10);
        fprintf(1,"que es crotolamo\n");
        break;
    }
    if(pid == 0){
      break;
    }
  }

  int aux = 0;
  while(1){
    for(int i = 0; i < 99999; i++){
      for(int j = 0; j < 9999; j++){
        aux = aux*j;
      }
    }
    struct pstat info;
    fprintf(1,"este\n");
    getpinfo(&info);
    fprintf(1,"no xddddd\n");
    for(int i=0; i<NPROC; i++){
      if(info.inuse[i] == 1){
        
        printf("pid: %d; tickets: %d; ticks: %d;\n", info.pid[i], info.tickets[i], info.ticks[i]);
      }
    }
  }
    fprintf(1,"que es crotolamo\n");

  exit(0);
}