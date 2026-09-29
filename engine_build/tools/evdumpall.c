/* evdumpall.c — dump evdev events from ALL /dev/input/event* for N seconds.
 * Prefixes each line with the device name. Usage: ./evdumpall [seconds]
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#define MAXDEV 16
static const char *evname(int t){return t==EV_SYN?"SYN":t==EV_ABS?"ABS":t==EV_REL?"REL":t==EV_KEY?"KEY":"?";}
int main(int argc,char**argv){
  int secs=(argc>1)?atoi(argv[1]):12;
  DIR*d=opendir("/dev/input"); if(!d){perror("opendir");return 1;}
  int fds[MAXDEV]; const char*nms[MAXDEV]; int n=0;
  struct dirent*de;
  while((de=readdir(d))&&n<MAXDEV){
    if(strncmp(de->d_name,"event",5))continue;
    char path[64];snprintf(path,sizeof path,"/dev/input/%s",de->d_name);
    int fd=open(path,O_RDONLY|O_NONBLOCK); if(fd<0)continue;
    /* only devices with keys or abs axes */
    unsigned long kb=0,ab=0; ioctl(fd,EVIOCGBIT(EV_KEY,sizeof kb),&kb); ioctl(fd,EVIOCGBIT(EV_ABS,sizeof ab),&ab);
    if(!kb&&!ab){close(fd);continue;}
    char nm[64];size_t len=ioctl(fd,EVIOCGNAME(sizeof nm),nm); if(len)nm[len]=0; else snprintf(nm,sizeof nm,de->d_name);
    fds[n]=fd;nms[n]=strdup(nm);n++;
  }
  closedir(d);
  printf("evdumpall: %d devices, %ds — press D-pad LEFT 3x slowly now\n",n,secs);fflush(stdout);
  struct pollfd pf[MAXDEV]; for(int i=0;i<n;i++){pf[i].fd=fds[i];pf[i].events=POLLIN;}
  time_t t0=time(NULL);
  while(time(NULL)-t0<secs){
    int r=poll(pf,n,100); if(r<=0)continue;
    for(int i=0;i<n;i++){
      if(!(pf[i].revents&POLLIN))continue;
      struct input_event ev;
      while(read(fds[i],&ev,sizeof ev)==sizeof ev){
        if(ev.type==EV_KEY||ev.type==EV_ABS||ev.type==EV_REL||ev.type==EV_SYN)
          printf("[%s] %-3s code=%-4d val=%d\n",nms[i],evname(ev.type),ev.code,ev.value);
      }
      pf[i].revents=0;
    }
    fflush(stdout);
  }
  printf("evdumpall: done\n");
  return 0;
}
