#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
int main(void){
  DIR*d=opendir("/dev/input");
  if(!d){perror("opendir");return 1;}
  struct dirent*e;
  while((e=readdir(d))){
    if(strncmp(e->d_name,"event",5))continue;
    char path[64];snprintf(path,sizeof path,"/dev/input/%s",e->d_name);
    int fd=open(path,O_RDWR|O_NONBLOCK);
    if(fd<0){fd=open(path,O_RDONLY|O_NONBLOCK);}
    if(fd<0){printf("%s: OPEN FAIL errno=%d\n",e->d_name,errno);continue;}
    unsigned long k8[8]={0},k12[12]={0},a8[8]={0};
    int r8=ioctl(fd,EVIOCGBIT(EV_KEY,sizeof k8),k8);
    int r12=ioctl(fd,EVIOCGBIT(EV_KEY,sizeof k12),k12);
    int ra=ioctl(fd,EVIOCGBIT(EV_ABS,sizeof a8),a8);
    int bits8=0,bits12=0,bitsa=0;
    for(int i=0;i<8;i++)bits8|=k8[i];
    for(int i=0;i<12;i++)bits12|=k12[i];
    for(int i=0;i<8;i++)bitsa|=a8[i];
    printf("%s: open_ok key_ioctl8=%d(has=%d) key_ioctl12=%d(has=%d) abs_ioctl=%d(has=%d)\n",
      e->d_name,r8,bits8,r12,bits12,ra,bitsa);
    close(fd);
  }
  closedir(d);
  return 0;
}
