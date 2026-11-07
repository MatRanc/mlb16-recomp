// LD_PRELOAD shim, experiment only: guest RAM and big mmaps on 2 MB pages, MyGameRecomp .text moved to 2 MB pages.
// Arms: THP_SHIM=mem, text, or all. Takes effect only in the game process.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#define TWO_MB (2UL<<20)
static int s_on, s_mem, s_text;
static void *(*real_mmap)(void*,size_t,int,int,int,off_t);
static size_t s_adv;
static int is_game(void){ char b[256]; ssize_t n=readlink("/proc/self/exe",b,sizeof b-1); if(n<=0)return 0; b[n]=0; return strstr(b,"MyGameRecomp")!=NULL; }
static void advise(void*p,size_t len){
  uintptr_t a=((uintptr_t)p+TWO_MB-1)&~(TWO_MB-1), e=((uintptr_t)p+len)&~(TWO_MB-1);
  if(e>a && madvise((void*)a,e-a,MADV_HUGEPAGE)==0) s_adv+=e-a;
}
void* mmap(void*addr,size_t len,int prot,int flags,int fd,off_t off){
  if(!real_mmap) real_mmap=dlsym(RTLD_NEXT,"mmap");
  void*p=real_mmap(addr,len,prot,flags,fd,off);
  if(s_mem && p!=MAP_FAILED && (flags&MAP_ANONYMOUS) && !(flags&MAP_SHARED) && len>=(8UL<<20)) advise(p,len);
  return p;
}
void* mmap64(void*addr,size_t len,int prot,int flags,int fd,off_t off) __attribute__((alias("mmap")));
static void remap_text(void){
  FILE*f=fopen("/proc/self/maps","r"); if(!f)return; char l[512]; uintptr_t s=0,e=0;
  while(fgets(l,sizeof l,f)){ uintptr_t a,b; char perm[8],path[300]=""; if(sscanf(l,"%lx-%lx %7s %*s %*s %*s %299s",&a,&b,perm,path)<3)continue;
    if(strstr(path,"MyGameRecomp")&&perm[2]=='x'){s=a;e=b;break;} }
  fclose(f); if(!s)return;
  uintptr_t as=(s+TWO_MB-1)&~(TWO_MB-1), ae=e&~(TWO_MB-1); if(ae<=as)return; size_t sz=ae-as;
  char*raw=real_mmap(NULL,sz+TWO_MB,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(raw==MAP_FAILED)return;
  char*t=(char*)(((uintptr_t)raw+TWO_MB-1)&~(TWO_MB-1));
  madvise(t,sz,MADV_HUGEPAGE);
  memcpy(t,(void*)as,sz);
  mprotect(t,sz,PROT_READ|PROT_EXEC);
  void*r=mremap(t,sz,sz,MREMAP_MAYMOVE|MREMAP_FIXED,(void*)as);
  dprintf(2,"[thp-shim] text %lx-%lx (%zu MB) moved: %s\n",as,ae,sz>>20,r==(void*)as?"ok":"FAILED");
}
__attribute__((constructor)) static void init(void){
  real_mmap=dlsym(RTLD_NEXT,"mmap");
  const char*e=getenv("THP_SHIM"); if(!e||!is_game())return;
  s_on=1; s_mem=!strcmp(e,"mem")||!strcmp(e,"all"); s_text=!strcmp(e,"text")||!strcmp(e,"all");
  dprintf(2,"[thp-shim] on mode=%s\n",e);
  if(s_text) remap_text();
}
__attribute__((destructor)) static void fini(void){ if(s_on) dprintf(2,"[thp-shim] madvised %zu MB\n",s_adv>>20); }
