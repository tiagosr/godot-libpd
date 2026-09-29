// Press START (315) + SELECT (314) together via uinput (force-quit combo).
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/time.h>
static void ms(int t){ struct timeval s={t/1000,t%1000*1000}; select(0,0,0,0,&s); }
static int push(int fd,int type,unsigned code,int val){
  struct input_event e; memset(&e,0,sizeof e); gettimeofday(&e.time,0);
  e.type=type; e.code=code; e.value=val;
  return write(fd,&e,sizeof e)==sizeof e?0:-1;
}
int main(void){
  int fd=open("/dev/uinput",O_WRONLY|O_NONBLOCK);
  if(fd<0){perror("uinput");return 1;}
  ioctl(fd,UI_SET_EVBIT,EV_KEY);
  ioctl(fd,UI_SET_KEYBIT,315);ioctl(fd,UI_SET_KEYBIT,314);
  struct uinput_setup us; memset(&us,0,sizeof us);
  us.id.bustype=BUS_USB; us.id.vendor=0x045e; us.id.product=0x028e; us.id.version=0x0114;
  strncpy(us.name,"synth-ss",sizeof us.name-1);
  ioctl(fd,UI_DEV_SETUP,&us);
  if(ioctl(fd,UI_DEV_CREATE)<0){perror("create");return 1;}
  printf("synth-ss ready\n"); fflush(stdout);
  ms(15000); // device must outlive the app boot (~8s from exfat)
  push(fd,EV_KEY,315,1);
  push(fd,EV_KEY,314,1); push(fd,EV_SYN,0,0);
  ms(400);
  push(fd,EV_KEY,315,0); push(fd,EV_KEY,314,0); push(fd,EV_SYN,0,0);
  printf("combo sent\n");
  ms(1500);
  ioctl(fd,UI_DEV_DESTROY); close(fd);
  return 0;
}
