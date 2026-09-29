// Create a uinput "controller" that mimics the Trimui Brick's evdev button
// codes, then inject a scripted press sequence. Verifies the full
// evdev->DisplayServer->Input pipeline without physical buttons.
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
  struct input_event e; memset(&e,0,sizeof e);
  gettimeofday(&e.time,0); e.type=type; e.code=code; e.value=val;
  return write(fd,&e,sizeof e)==sizeof e?0:-1;
}
int main(int argc,char**argv){
  int dur = argc>1?atoi(argv[1]):15000; // total ms
  int fd=open("/dev/uinput",O_WRONLY|O_NONBLOCK);
  if(fd<0){perror("uinput");return 1;}
  ioctl(fd,UI_SET_EVBIT,EV_KEY);
  ioctl(fd,UI_SET_EVBIT,EV_ABS);
  ioctl(fd,UI_SET_KEYBIT,304);ioctl(fd,UI_SET_KEYBIT,305);ioctl(fd,UI_SET_KEYBIT,307);ioctl(fd,UI_SET_KEYBIT,308);
  ioctl(fd,UI_SET_KEYBIT,314);ioctl(fd,UI_SET_KEYBIT,315);
  struct uinput_abs_setup a;
  memset(&a,0,sizeof a);
  a.code=16;
a.absinfo.minimum=-1;a.absinfo.maximum=1;
a.absinfo.fuzz=0;a.absinfo.flat=0;
  ioctl(fd,UI_ABS_SETUP,&a);
  memset(&a,0,sizeof a);
  a.code=17;
a.absinfo.minimum=-1;a.absinfo.maximum=1;
  ioctl(fd,UI_ABS_SETUP,&a);
  struct uinput_setup us; memset(&us,0,sizeof us);
  us.id.bustype=BUS_USB; us.id.vendor=0x045e; us.id.product=0x028e; us.id.version=0x0114;
  strncpy(us.name,"synth trimui",sizeof us.name-1);
  us.ff_effects_max=0;
  ioctl(fd,UI_DEV_SETUP,&us);
  if(ioctl(fd,UI_DEV_CREATE)<0){perror("create");return 1;}
  printf("synth uinput created\n"); fflush(stdout);
  int t0=0, t=dur;
  // A(304) x3, B(305) x3, X(308) x3, Y(307) x3, hat, L1(310? not in set->use 314 select/315 start)
  int seq[][3]={{304,1,120},{304,0,80},{304,1,120},{304,0,80},{304,1,120},{304,0,200},
                {305,1,120},{305,0,80},{305,1,120},{305,0,80},{305,1,120},{305,0,200},
                {308,1,120},{308,0,80},{308,1,120},{308,0,80},{308,1,120},{308,0,200},
                {307,1,120},{307,0,80},{307,1,120},{307,0,80},{307,1,120},{307,0,300},
                {16,1,100},{16,0,100},{17,1,100},{17,0,100},{16,-1,100},{16,0,100},{17,-1,100},{17,0,300},
                {315,1,120},{315,0,80},{314,1,120},{314,0,300}};
  int n=sizeof seq/sizeof seq[0];
  while(t0<t){
    for(int i=0;i<n && t0<t;i++){
      int type = seq[i][1]==seq[i][1] && (seq[i][0]==16||seq[i][0]==17)?EV_ABS:EV_KEY;
      push(fd,type,(unsigned)seq[i][0],seq[i][1]);
      if(type==EV_ABS) push(fd,EV_SYN,0,0);
      ms(seq[i][2]); t0+=seq[i][2];
    }
  }
  printf("injection done\n");
  ioctl(fd,UI_DEV_DESTROY);
  close(fd);
  return 0;
}
