
#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"
#include "memlayout.h"


int main () {
	int a, b;
	//ls();
	//int mem;
	printf(1,"hi~~\n");
	//mem =freemem();
	//printf(1, "mem : %d\n", mem);
	//sbrk(0);
	
	malloc(5990000);
		
//	int f;
	/*
	for (int i = 0; i < 20; i++) {
	if ((f = fork()) == 0) {
		mem = freemem();
		printf(1, "1 mem : %d\n", mem);
		exit();
		} else {
			mem = freemem();
			printf(1, "2 mem : %d\n", mem);
			wait();
			}
	}*/


    swapstat(&a, &b);
    printf(1, "swap read : %d, swap write : %d\n", a, b);
    int mem = freemem();
    printf(1, "memory : %d\n", mem);
    exit();
    return 0;
}

