#include "../kernel/types.h"
#include "../kernel/stat.h"
#include "user.h"
#include "../kernel/pstat.h"


void spin() {
    unsigned x = 0;
    unsigned y = 0;

    while (x < 100000) {
        y = 0;
        while (y < (10000)) {
            y++;
        }
        x++;
    }
}

void printpinfo(int pid)
{
	struct pstat pi = {0};
	getpinfo(&pi);
	int i;
    for (i = 0; i < NPROC; i++) {
        if(pi.pid[i] == pid) {
		    printf("Number of tickets that PID %d has: %d\n", pid, pi.tickets[i]);
	        printf("Number of ticks that PID %d has: %d\n", pid, pi.ticks[i]);
	        printf("Is the process with PID %d in use? (0 or 1): %d\n", pid, pi.inuse[i]);
        }
    }
}

int
main(int argc, char *argv[])
{
    int pid1, pid2, pid3;
      
    if ((pid1 = fork()) == 0) {
        int pp1 = getpid();
	printf("Process started with PID %d\n\n", pp1);
        settickets(10);
        spin();
	printpinfo(pp1);
        printf("Process with PID %d finished!\n\n", pp1);
        exit(0); 
    }
    else if ((pid2 = fork()) == 0) {
        int pp2 = getpid();
	printf("Process started with PID %d\n\n", pp2);
        settickets(20);
        spin();
	printpinfo(pp2);
        printf("Process with PID %d finished!\n\n", pp2);
        exit(0);
    }
    else if ((pid3 = fork()) == 0) {
        int pp3 = getpid();
	printf("Process started with PID %d\n\n", pp3);
        settickets(30);
        spin();
	printpinfo(pp3);
        printf("Process with PID %d finished!\n\n", pp3);
        exit(0);
    }
    wait(0);
    wait(0);
    wait(0);
    exit(0);
}
