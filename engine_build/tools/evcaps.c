#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char**argv){
  const char*d=argc>1?argv[1]:"/dev/input/event3";
  int fd=open(d,O_RDONLY);
  if(fd<0){perror(d);return 1;}
  struct input_id id; ioctl(fd,EVIOCGID,&id);
  printf("%s id=%04x:%04x:%04x\n",d,id.vendor,id.product,id.version);
  unsigned long ev[8],key[12],abs[8],rel[8];
  if(ioctl(fd,EVIOCGBIT(0,sizeof(ev)*8),ev)<0)perror("EV");
  printf("EV  :");for(int i=0;i<64;i++)if(ev[i/64]>>(i%64)&1)printf(" %d",i);printf("\n");
  if(ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(key)*12),key)<0)perror("KEY");
  printf("KEY :");for(int i=0;i<768;i++)if(key[i/64]>>(i%64)&1)printf(" %d",i);printf("\n");
  if(ioctl(fd,EVIOCGBIT(EV_ABS,sizeof(abs)*8),abs)<0)perror("ABS");
  printf("ABS :");for(int i=0;i<64;i++)if(abs[i/64]>>(i%64)&1)printf(" %d",i);printf("\n");
  if(ioctl(fd,EVIOCGBIT(EV_REL,sizeof(rel)*8),rel)<0)perror("REL");
  printf("REL :");for(int i=0;i<64;i++)if(rel[i/64]>>(i%64)&1)printf(" %d",i);printf("\n");
  return 0;
}
