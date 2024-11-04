#include "types.h"
#include "user.h"
#include "stat.h"

int main() {
	/*
  int i; 
  for (i=1; i<11; i++) {
    printf(1, "%d: ", i);
    if (getpname(i))
      printf(1, "Wrong pid\n");
  }
  exit();
  */
  /*
  for (i=1; i<11; i++) {
	  ps(i);
	  }
  exit();
  */
  //printf(1, "%d\n", getnice(1));
  //printf(1, "%d\n", getnice(2));
 // setnice(1,25);
  //setnice(2, 30);
  int pid;

  pid = fork();
  if (pid < 0) {
	printf(1, "fork failed\n");
	exit(); 
	} else if (pid == 0) {
 		printf(1, "Hello from child process!\n");
		ps(0);
		exit();	
	} else {
		wait();
		printf(1, "Hello from parent process!\n");
		exit();
		
		}
  //ps(0);
 // ps(1);
  //ps(2);
  //ps(3);
  exit();
  return 0;
}
