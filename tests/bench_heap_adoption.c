#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include "dyn-ds.h"
typedef struct { double key; int node; } it_t;
typedef struct { it_t *a; unsigned n, cap; } hp_t;
static int hpush(hp_t*h,double k,int nd){unsigned i;if(h->n==h->cap){unsigned c=h->cap?h->cap*2:16;it_t*na=realloc(h->a,(size_t)c*sizeof*na);if(!na)return -1;h->a=na;h->cap=c;}i=h->n++;h->a[i].key=k;h->a[i].node=nd;while(i>0){unsigned p=(i-1)/2;if(h->a[p].key<=h->a[i].key)break;{it_t t=h->a[p];h->a[p]=h->a[i];h->a[i]=t;}i=p;}return 0;}
static int hpop(hp_t*h,it_t*o){unsigned i=0;if(!h->n)return 0;*o=h->a[0];h->a[0]=h->a[--h->n];for(;;){unsigned l=2*i+1,r=2*i+2,s=i;if(l<h->n&&h->a[l].key<h->a[s].key)s=l;if(r<h->n&&h->a[r].key<h->a[s].key)s=r;if(s==i)break;{it_t t=h->a[s];h->a[s]=h->a[i];h->a[i]=t;}i=s;}return 1;}
#define N 400000
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){
  unsigned seed=12345,i; double t0,tp,tm; long long sink=0;
  hp_t h={0,0,0}; it_t o;
  t0=now();
  for(i=0;i<N;i++){seed=seed*1103515245u+12345u;hpush(&h,(double)(seed>>8),(int)i);}
  while(hpop(&h,&o))sink+=o.node;
  tp=now()-t0;
  free(h.a);
  { dyn_mmheap_t *m=dyn_mmheap_new(); dyn_cell_t c,out; double pri;
    seed=12345; t0=now();
    for(i=0;i<N;i++){seed=seed*1103515245u+12345u;c.w[0]=(uint64_t)i;c.w[1]=0;dyn_mmheap_push(m,(double)(seed>>8),&c);}
    while(dyn_mmheap_pop_min(m,&pri,&out))sink+=(long long)out.w[0];
    tm=now()-t0; dyn_mmheap_free(m,NULL,NULL); }
  printf("private (double,int) min-heap : %.1f ms\n", tp*1000);
  printf("dyn-ds MinMaxHeap            : %.1f ms   (%.2fx)\n", tm*1000, tm/tp);
  printf("sink %lld\n", sink);
  return 0;
}
