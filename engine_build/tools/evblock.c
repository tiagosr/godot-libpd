#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
int main(void){
  int fd=open("/dev/input/event3",O_RDONLY|O_NONBLOCK);
  if(fd<0){perror("open");return 1;}
  printf("evblock: waiting up to 12s for the first event — MASH the buttons\n");fflush(stdout);
  long t0=time(NULL);
  for(;;){
    if(time(NULL)-t0>12){printf("evblock: TIMEOUT — no events in 12s\n");return 2;}
    struct input_event ev;
    ssize_t n=read(fd,&ev,sizeof ev);
    if(n==sizeof ev){printf("GOT: type=%d code=%d val=%d\n",ev.type,ev.code,ev.value);fflush(stdout);return 0;}
    if(n<0 && (errno==EAGAIN||errno==EWOULDBLOCK)) usleep(1000);
  }
}
