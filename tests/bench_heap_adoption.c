/* bench_heap_adoption.c -- the measurement behind W1.7's VERDICT.
 *
 * W1.7 proposed deleting dyna-graph.c's private min-heap and using dyn-ds's
 * heap instead, on the general principle that one implementation beats two.
 * The plan rated it Medium risk and required a shortest-path bench to gate it.
 * This is that bench, and it says DO NOT.
 *
 * The two structures are not the same thing:
 *
 *   private   a bare binary min-heap of (double key, int node). 16 bytes per
 *             item, declared on the stack, no indirection, and pop_min is the
 *             only operation Dijkstra performs.
 *   dyn-ds    a MinMaxHeap -- a double-ended INTERVAL heap. It is O(log n) at
 *             both ends, which Dijkstra does not need, and it pays for that
 *             with more comparisons per sift. It also boxes each element in a
 *             16-byte dyn_cell_t alongside the priority, and the heap object
 *             itself is a separate allocation.
 *
 * Measured on Dijkstra's actual access pattern -- 400k pushes then drain by
 * pop_min, three runs -- the adoption costs 1.52-1.58x. That is a direct
 * regression on the inner loop of dijkstra() and aStar(), bought for nothing:
 * the "one implementation" argument does not apply when the two are different
 * data structures with different asymptotics at one end.
 *
 * This is the same shape as W1.6b's verdict on the incumbent 13 containers,
 * and it is recorded the same way: a measured decision not to convert, not a
 * deferral. Adding a plain min-heap to dyn-ds purely so graph could call it
 * would be adding a third implementation to remove a second.
 *
 * Build: cc -std=c11 -O2 -Isrc/core tests/bench_heap_adoption.c \
 *           src/core/dyn-ds.c src/core/dyn-hash.c -o /tmp/hb && /tmp/hb */
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
