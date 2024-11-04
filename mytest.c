#include "types.h"
#include "user.h"
#include "stat.h"
#include "fcntl.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "proc.h"
#include "syscall.h"

int main() {
	int fd = open("README", O_RDONLY);
	int num = freemem();
	printf(1, "before mmap : %d\n", num);
	int addr = mmap(0,8192, PROT_READ, MAP_POPULATE,fd,0);
	printf(1, "0x%x\n", addr);
	num = freemem();
	printf(1, "after mmap : %d\n", num);
	char* ptr = (char*)addr;
	int value = *ptr;
	printf(1, "value : %d\n", value);

	//num = freemem();
	//printf(1, "after page handler : %d\n", num);
	int f;	
	if ((f = fork()) == 0) { 
		munmap(addr);
		num = freemem();
		printf(1, "child) after unmap : %d\n", num);
		exit();

		return 0;

	} else {
		munmap(addr);
		num = freemem();
		printf(1, "parent) after unmap : %d\n", num);	
		wait();
	}
	num = freemem();
	printf(1,"last mem : %d\n", num);
	close(fd);
	exit();
	return 0;
}
			
