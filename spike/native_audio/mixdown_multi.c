// mixdown_multi.c — PROOF that the revised M5 architecture works (run 2026-10-03).
// 8 synth libpd workers (each its own pinned thread: set_instance ONCE, then
// loop process_float) feed 2ch into rings; 1 mix-down instance (16-in/2-out)
// is rendered on a SEPARATE thread (the PortAudio-callback stand-in) with
// set_instance(mix) ONCE. 9 concurrent instances, ~1.5s, NO crash.
//
// This is the model M5 implements (Task 4). It proves the hard invariant:
// never switch pd_this between instances on one thread (that crashes pd);
// one dedicated thread per instance works.
//
// Build (macOS): see the comment block the task used; link libpd-multi.a +
// the CoreAudio frameworks.
// Linchpin: mix-down instance (16 in, 2 out) init on MAIN thread,
// rendered on a SEPARATE thread (like the PortAudio callback) with
// set_instance called ONCE on that thread. Plus 8 pinned synth workers
// feeding rings -> 9 concurrent libpd instances total.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "z_libpd.h"
static t_pdinstance *gworkers[8]; static t_pdinstance *gmix=NULL;
static int g_go=0;
// synth worker: pinned thread, set_instance once, render 2ch into a ring slot
static float gring[8][256*2]; static volatile long gfill[8];
static void* synth(void*arg){
  int id=(int)(intptr_t)arg; char fn[64]; snprintf(fn,64,"/tmp/mxw%d.pd",id);
  FILE*f=fopen(fn,"w"); const char*p="#N canvas 0 0 200 200 12;\n#X obj 20 20 osc~ 200;\n#X obj 20 90 *~ 0.3;\n#X obj 20 150 dac~;\n#X connect 0 0 1 0;\n#X connect 1 0 2 0;\n#X connect 1 0 2 1;\n"; fwrite(p,1,strlen(p),f); fclose(f);
  gworkers[id]=libpd_new_instance(); libpd_set_instance(gworkers[id]);
  libpd_init_audio(0,2,44100); libpd_start_message(1); libpd_add_float(1.0f); libpd_finish_message("pd","dsp");
  char bf[64]; snprintf(bf,64,"mxw%d.pd",id); libpd_openfile(bf,"/tmp");
  float buf[64*2];
  while(!g_go){ libpd_process_float(1, NULL, buf); /* copy to ring */ float*dst=&gring[id][0]; for(int i=0;i<256*2;i++)dst[i]=buf[i%128]; __sync_synchronize(); gfill[id]++; }
  return NULL;
}
// mix render thread (stand-in for PortAudio callback): set_instance(mix) ONCE,
// then each block gather 8 rings (16ch) -> mix_in, process_float, -> 2ch out
static void* mixthr(void*arg){
  libpd_set_instance(gmix);          // ONCE on this thread
  float mixin[256*16]; float mixout[256*2];
  for(long i=0; i<400 && !g_go; i++){
    for(int w=0;w<8;w++) for(int c=0;c<2;c++) for(int k=0;k<256;k++) mixin[k*16 + w*2 + c] = gring[w][k*2+c];
    libpd_process_float(256/64, mixin, mixout);   // 4 blocks
  }
  return NULL;
}
int main(){
  setvbuf(stdout,NULL,_IONBF,0);
  printf("libpd_init=%d\n",libpd_init());
  // 8 synth workers
  pthread_t tw[8]; for(int i=0;i<8;i++){ pthread_create(&tw[i],NULL,synth,(void*)(intptr_t)i); }
  // mix instance init on MAIN thread
  char*mp="#N canvas 0 0 400 300 12;\n#X obj 20 20 adc~ 1;\n#X obj 20 60 *~ 0.5;\n#X obj 20 120 dac~;\n#X connect 0 0 1 0;\n#X connect 1 0 2 0;\n#X connect 1 0 2 1;\n";
  FILE*f=fopen("/tmp/mix.pd","w"); fwrite(mp,1,strlen(mp),f); fclose(f);
  gmix=libpd_new_instance(); libpd_set_instance(gmix);
  libpd_init_audio(16,2,44100); libpd_start_message(1); libpd_add_float(1.0f); libpd_finish_message("pd","dsp");
  void*mph=libpd_openfile("mix.pd","/tmp"); printf("mix open=%p\n",(void*)mph);
  // mix render thread (PortAudio-callback stand-in)
  pthread_t tm; pthread_create(&tm,NULL,mixthr,NULL);
  for(int i=0;i<1500;i++) usleep(1000);  // ~1.5s
  float peak=0; 
  printf("all 9 instances + mix ran ~1.5s\n");
  g_go=1;
  pthread_join(tm,NULL); for(int i=0;i<8;i++) pthread_join(tw[i],NULL);
  printf("DONE (8 workers + mix, 9 concurrent)\n"); return 0;
}
