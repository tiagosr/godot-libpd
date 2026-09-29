// Hammer the D-pad (HAT0X/Y) + B button at high rate via uinput.
// Reproduces the menu-session main-thread spin for bisection.
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
static void hat(int fd,int x,int y){
  if(x) { push(fd,EV_ABS,16,x); push(fd,EV_SYN,0,0); }
  if(y) { push(fd,EV_ABS,17,y); push(fd,EV_SYN,0,0); }
  if(!x && !y) { push(fd,EV_ABS,16,0); push(fd,EV_ABS,17,0); push(fd,EV_SYN,0,0); }
}
int main(int argc,char**argv){
  int dur = argc>1?atoi(argv[1]):20000;
  int delay = argc>2?atoi(argv[2]):0;
  int fd=open("/dev/uinput",O_WRONLY|O_NONBLOCK);
  if(fd<0){perror("uinput");return 1;}
  ioctl(fd,UI_SET_EVBIT,EV_KEY);ioctl(fd,UI_SET_EVBIT,EV_ABS);
  ioctl(fd,UI_SET_KEYBIT,305);
  struct uinput_abs_setup a; memset(&a,0,sizeof a);
  a.code=16;a.absinfo.minimum=-1;a.absinfo.maximum=1;ioctl(fd,UI_ABS_SETUP,&a);
  a.code=17;a.absinfo.minimum=-1;a.absinfo.maximum=1;ioctl(fd,UI_ABS_SETUP,&a);
  struct uinput_setup us; memset(&us,0,sizeof us);
  us.id.bustype=BUS_USB; us.id.vendor=0x045e; us.id.product=0x028e; us.id.version=0x0114;
  strncpy(us.name,"synth-hat",sizeof us.name-1);
  ioctl(fd,UI_DEV_SETUP,&us);
  if(ioctl(fd,UI_DEV_CREATE)<0){perror("create");return 1;}
  printf("synth-hat ready\n"); fflush(stdout);
  if(delay) ms(delay);
  int t0=0;
  while(t0<dur){
    hat(fd,1,0);  ms(40);
    hat(fd,0,0);  ms(40);
    hat(fd,0,1);  ms(40);
    hat(fd,0,0);  ms(40);
    hat(fd,-1,0); ms(40);
    hat(fd,0,0);  ms(40);
    hat(fd,0,-1); ms(40);
    hat(fd,0,0);  ms(60);
    ms(60);
    ms(120);
    t0 += 540;
  }
  printf("done\n");
  ioctl(fd,UI_DEV_DESTROY); close(fd);
  return 0;
}
