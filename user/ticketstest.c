#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"
#include "kernel/pstat.h"

#define MAX_CHILDREN 32
#define DEFAULT_CHILDREN 4

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

  int pid = 0;
  int dadpid = getpid();

  for(int i = 0; i < childs; i++){
    pid = fork();
    switch(pid){
      case -1:
        fprintf(1, "Error, Fork - testsettickets\n");
        exit(1);
        break;
      case 0:
        settickets((childs-i)*10);
        break;
    }
    if(pid == 0){
      break;
    }
  }

  // Cara inútil para pseudosincronizar los procesos y que el padre pueda
  // leer los tickets asignados a los hijos correctamente.
  int aux = 0;

  for(int i = 0; i < 99999; i++){
      for(int j = 0; j < 9999; j++){
        aux = aux*j;
      }
    }

  struct pstat info;
  getpinfo(&info);
  // Obtenemos la cabecera para el CSV, solo lo ejecuta el proceso padre.
  if(dadpid == getpid()){
    for(int i=0; i<NPROC; i++){
      if(info.inuse[i] == 1){
          printf("%d,", info.pid[i]);
      }
    }
    printf("\n");
    for(int i=0; i<NPROC; i++){
      if(info.inuse[i] == 1){
          printf("%d,", info.tickets[i]);
      }
    }
    printf("\n");
  }

  while(1){
    for(int i = 0; i < 99999; i++){
      for(int j = 0; j < 9999; j++){
        aux = aux*j;
      }
    }
    // Realizamos lecturas de los valores para todos los procesos.
    getpinfo(&info);
    for(int i=0; i<NPROC; i++){
      if(info.inuse[i] == 1){
        printf("%d,", info.ticks[i]);
      }
    }
    printf("\n");
  }
  
  exit(0);
}