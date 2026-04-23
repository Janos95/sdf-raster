#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#define MAX_INS 10000
#define MAX_FRONTIER 2000
#define MAX_LOCAL 128
#define MAX_BOXES_PER_TERM 16
#define IMAGE_W 1024
#define IMAGE_H 1024
#define NEG_INF (-1e30f)
#define OMP_SCHEDULE_KIND guided,1
#define BENCH_RUNS 8
#define BASE_REFINE_ROUNDS 4
#define CHILD_REFINE_ROUNDS 2
#define MAX_SPLIT_DEPTH 4
#define AREA_THRESHOLD 0.0095f
#define UNION_AREA_SHRINK 0.95f

typedef enum {OP_CONST, OP_VARX, OP_VARY, OP_ADD, OP_SUB, OP_MUL, OP_NEG, OP_SQUARE, OP_SQRT, OP_MIN, OP_MAX} Op;
typedef enum {MODE_SCALAR, MODE_X, MODE_Y, MODE_FULL} ValueMode;
typedef struct { Op op; int16_t a, b; float fa, fb; } Instr;
typedef struct { float lo, hi; } Interval;
typedef struct { float x0,x1,y0,y1; } Box;
typedef struct { char key[32]; int idx; } IdMap;
typedef struct {
    uint16_t n;
    uint16_t root;
    int16_t xlocal, ylocal;
    uint8_t nxops, nyops;
    Instr ins[MAX_LOCAL];
    uint8_t mode[MAX_LOCAL];
    uint8_t xops[MAX_LOCAL];
    uint8_t yops[MAX_LOCAL];
} CompactTape;

typedef struct {
    Interval fwd[MAX_LOCAL], desired[MAX_LOCAL];
    uint16_t desired_tag[MAX_LOCAL], mark_tag[MAX_INS], local_map[MAX_INS];
    uint16_t cur_tag, cur_mark;
    int post[MAX_LOCAL];
    int sp; int stack[MAX_LOCAL*2];
    float vals[MAX_LOCAL][IMAGE_W] __attribute__((aligned(64)));
} Worker;

static Instr gins[MAX_INS];
static int nins=0, root=-1, xidx=-1, yidx=-1;
static int frontier[MAX_FRONTIER], nfrontier=0;
static uint16_t frontier_mark[MAX_INS];
static uint16_t frontier_mark_gen = 1;
static IdMap ids[MAX_INS]; static int nids=0;
static int nwork_boxes = 0;
static uint8_t image[IMAGE_W * IMAGE_H];
static float gxcoords[IMAGE_W], gycoords[IMAGE_H];
static inline int clampi(int v, int lo, int hi);
static inline float box_area(Box b);

static inline double now_sec(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec*1e-9; }
static inline Interval I(float lo,float hi){ Interval r={lo,hi}; return r; }
static inline float fmin4(float a,float b,float c,float d){ return fminf(fminf(a,b),fminf(c,d)); }
static inline float fmax4(float a,float b,float c,float d){ return fmaxf(fmaxf(a,b),fmaxf(c,d)); }
static inline Interval intersect_i(Interval a, Interval b){ return I(fmaxf(a.lo,b.lo), fminf(a.hi,b.hi)); }
static inline bool valid_i(Interval a){ return a.lo <= a.hi; }
static inline Interval iadd(Interval x, Interval y){ return I(x.lo+y.lo,x.hi+y.hi); }
static inline Interval isub(Interval x, Interval y){ return I(x.lo-y.hi,x.hi-y.lo); }
static inline Interval ineg(Interval x){ return I(-x.hi,-x.lo); }
static inline Interval imul(Interval x, Interval y){ float a=x.lo*y.lo,b=x.lo*y.hi,c=x.hi*y.lo,d=x.hi*y.hi; return I(fmin4(a,b,c,d),fmax4(a,b,c,d)); }
static inline Interval isquare(Interval x){ float l=x.lo,u=x.hi; if(l<=0&&0<=u) return I(0.0f,fmaxf(l*l,u*u)); float a=l*l,b=u*u; return I(fminf(a,b),fmaxf(a,b)); }
static inline Interval isqrt_i(Interval x){ if(x.hi<0) return I(1.0f,0.0f); return I(sqrtf(fmaxf(0.0f,x.lo)), sqrtf(fmaxf(0.0f,x.hi))); }
static inline Interval imin_i(Interval x, Interval y){ return I(fminf(x.lo,y.lo), fminf(x.hi,y.hi)); }
static inline Interval imax_i(Interval x, Interval y){ return I(fmaxf(x.lo,y.lo), fmaxf(x.hi,y.hi)); }
static inline Interval div_i(Interval z, Interval d){ float a=z.lo/d.lo,b=z.lo/d.hi,c=z.hi/d.lo,e=z.hi/d.hi; return I(fmin4(a,b,c,e), fmax4(a,b,c,e)); }
static inline float box_area(Box b){ return fmaxf(0,b.x1-b.x0)*fmaxf(0,b.y1-b.y0); }
static inline uint8_t combine_mode(uint8_t a, uint8_t b){
    if(a == MODE_FULL || b == MODE_FULL) return MODE_FULL;
    if(a == MODE_SCALAR) return b;
    if(b == MODE_SCALAR) return a;
    return a == b ? a : MODE_FULL;
}

static int parse_op(const char* s){
    if(!strcmp(s,"const")) return OP_CONST; if(!strcmp(s,"var-x")) return OP_VARX; if(!strcmp(s,"var-y")) return OP_VARY;
    if(!strcmp(s,"add")) return OP_ADD; if(!strcmp(s,"sub")) return OP_SUB; if(!strcmp(s,"mul")) return OP_MUL; if(!strcmp(s,"neg")) return OP_NEG;
    if(!strcmp(s,"square")) return OP_SQUARE; if(!strcmp(s,"sqrt")) return OP_SQRT; if(!strcmp(s,"min")) return OP_MIN; if(!strcmp(s,"max")) return OP_MAX;
    fprintf(stderr,"unknown op %s\n", s); exit(1);
}
static int lookup_id(const char* s){ for(int i=0;i<nids;i++) if(!strcmp(ids[i].key,s)) return ids[i].idx; return -1; }
static void add_id(const char* s,int idx){ strcpy(ids[nids].key,s); ids[nids].idx=idx; nids++; }

static void init_coords(void){
    for(int i=0;i<IMAGE_W;i++) gxcoords[i] = -1.0f + 2.0f * (float)i / (float)(IMAGE_W - 1);
    for(int i=0;i<IMAGE_H;i++) gycoords[i] = -1.0f + 2.0f * (float)i / (float)(IMAGE_H - 1);
}

static void load_vm(const char* path){
    FILE* f=fopen(path,"r"); if(!f){ perror("fopen"); exit(1);} char line[256];
    while(fgets(line,sizeof(line),f)){
        if(line[0]=='#'||line[0]=='\n'||line[0]=='\0') continue;
        char id[32], op[32], a[64]="", b[64]="";
        int n=sscanf(line,"%31s %31s %63s %63s", id, op, a, b); if(n<2) continue;
        add_id(id,nins); Instr *in=&gins[nins]; in->op=parse_op(op); in->a=in->b=-1; in->fa=in->fb=0;
        if(in->op==OP_VARX) xidx=nins; if(in->op==OP_VARY) yidx=nins;
        if(n>=3){ if(a[0]=='_') in->a=lookup_id(a); else in->fa=strtof(a,NULL); }
        if(n>=4){ if(b[0]=='_') in->b=lookup_id(b); else in->fb=strtof(b,NULL); }
        nins++;
    }
    fclose(f); root=nins-1;
}

static void extract_frontier(void){
    nfrontier=0;
    if(++frontier_mark_gen == 0){
        memset(frontier_mark, 0, sizeof(frontier_mark));
        frontier_mark_gen = 1;
    }
    int st[MAX_INS], sp=0;
    st[sp++]=root;
    while(sp){
        int v=st[--sp];
        if(frontier_mark[v] == frontier_mark_gen) continue;
        frontier_mark[v] = frontier_mark_gen;
        Instr in=gins[v];
        if(in.op==OP_MIN){
            if(in.a>=0) st[sp++]=in.a;
            if(in.b>=0) st[sp++]=in.b;
        } else frontier[nfrontier++]=v;
    }
}

static inline void build_worker_init(Worker* w){
    memset(w->desired_tag,0,sizeof(w->desired_tag));
    memset(w->mark_tag,0,sizeof(w->mark_tag));
    w->cur_tag=1;
    w->cur_mark=1;
}

static inline bool build_compact_tape_term(Worker* w, int fi, CompactTape* tp){
    int fr = frontier[fi];
    if(++w->cur_mark == 0){ memset(w->mark_tag,0,sizeof(w->mark_tag)); w->cur_mark = 1; }
    int postn=0, sp=0;
    w->stack[sp++] = fr;
    w->stack[sp++] = 0;
    while(sp){
        int state = w->stack[--sp];
        int v = w->stack[--sp];
        if(state==0){
            if(w->mark_tag[v] == w->cur_mark) continue;
            w->mark_tag[v] = w->cur_mark;
            w->stack[sp++] = v; w->stack[sp++] = 1;
            Instr in = gins[v];
            if(in.b >= 0){ w->stack[sp++] = in.b; w->stack[sp++] = 0; }
            if(in.a >= 0){ w->stack[sp++] = in.a; w->stack[sp++] = 0; }
        } else {
            if(postn >= MAX_LOCAL) return false;
            w->post[postn++] = v;
        }
    }
    tp->n = (uint16_t)postn;
    tp->xlocal = tp->ylocal = -1;
    tp->nxops = tp->nyops = 0;
    for(int i=0;i<postn;i++) w->local_map[w->post[i]] = i;
    for(int i=0;i<postn;i++){
        int gv = w->post[i];
        Instr in = gins[gv];
        if(in.a >= 0) in.a = w->local_map[in.a];
        if(in.b >= 0) in.b = w->local_map[in.b];
        tp->ins[i] = in;
        switch(in.op){
            case OP_CONST: tp->mode[i] = MODE_SCALAR; break;
            case OP_VARX: tp->mode[i] = MODE_X; break;
            case OP_VARY: tp->mode[i] = MODE_Y; break;
            case OP_NEG:
            case OP_SQUARE:
            case OP_SQRT:
                tp->mode[i] = in.a >= 0 ? tp->mode[in.a] : MODE_SCALAR;
                break;
            case OP_ADD:
            case OP_SUB:
            case OP_MUL:
            case OP_MIN:
            case OP_MAX: {
                uint8_t ma = in.a >= 0 ? tp->mode[in.a] : MODE_SCALAR;
                uint8_t mb = in.b >= 0 ? tp->mode[in.b] : MODE_SCALAR;
                tp->mode[i] = combine_mode(ma, mb);
            } break;
        }
        if(gv == xidx) tp->xlocal = i;
        if(gv == yidx) tp->ylocal = i;
        if(gv == fr) tp->root = i;
        if(tp->mode[i] != MODE_Y && tp->mode[i] != MODE_FULL) tp->xops[tp->nxops++] = (uint8_t)i;
        if(tp->mode[i] != MODE_SCALAR && tp->mode[i] != MODE_X) tp->yops[tp->nyops++] = (uint8_t)i;
    }
    return true;
}

static inline void upd_desired(Worker* w, int child, Interval iv, Interval child_fwd){
    if(child<0) return;
    iv = intersect_i(iv, child_fwd);
    if(!valid_i(iv)) return;
    if(w->desired_tag[child] != w->cur_tag){ w->desired[child]=iv; w->desired_tag[child]=w->cur_tag; }
    else w->desired[child] = intersect_i(w->desired[child], iv);
}

static inline bool contract_once(Worker* w, const CompactTape* tp, Box domain, Box* out){
    int n=tp->n;
    for(int v=0; v<n; v++){
        Instr in=tp->ins[v];
        switch(in.op){
            case OP_CONST: w->fwd[v]=I(in.fa,in.fa); break;
            case OP_VARX: w->fwd[v]=I(domain.x0,domain.x1); break;
            case OP_VARY: w->fwd[v]=I(domain.y0,domain.y1); break;
            case OP_ADD: w->fwd[v]=iadd(w->fwd[in.a],w->fwd[in.b]); break;
            case OP_SUB: w->fwd[v]=isub(w->fwd[in.a],w->fwd[in.b]); break;
            case OP_MUL: w->fwd[v]=imul(w->fwd[in.a],w->fwd[in.b]); break;
            case OP_NEG: w->fwd[v]=ineg(w->fwd[in.a]); break;
            case OP_SQUARE: w->fwd[v]=isquare(w->fwd[in.a]); break;
            case OP_SQRT: w->fwd[v]=isqrt_i(w->fwd[in.a]); break;
            case OP_MIN: w->fwd[v]=imin_i(w->fwd[in.a],w->fwd[in.b]); break;
            case OP_MAX: w->fwd[v]=imax_i(w->fwd[in.a],w->fwd[in.b]); break;
        }
    }
    int r = tp->root;
    if(!valid_i(w->fwd[r]) || w->fwd[r].lo >= 0.0f) return false;
    if(++w->cur_tag == 0){ memset(w->desired_tag,0,sizeof(w->desired_tag)); w->cur_tag=1; }
    w->desired[r]=I(NEG_INF,0.0f); w->desired_tag[r]=w->cur_tag;
    for(int v=n-1; v>=0; v--){
        if(w->desired_tag[v] != w->cur_tag) continue;
        Interval z = w->desired[v]; Instr in=tp->ins[v];
        Interval xa = (in.a>=0? w->fwd[in.a] : I(in.fa,in.fa));
        Interval xb = (in.b>=0? w->fwd[in.b] : I(in.fb,in.fb));
        switch(in.op){
            case OP_CONST: case OP_VARX: case OP_VARY: break;
            case OP_ADD: upd_desired(w,in.a,I(z.lo-xb.hi,z.hi-xb.lo),xa); upd_desired(w,in.b,I(z.lo-xa.hi,z.hi-xa.lo),xb); break;
            case OP_SUB: upd_desired(w,in.a,I(z.lo+xb.lo,z.hi+xb.hi),xa); upd_desired(w,in.b,I(xa.lo-z.hi,xa.hi-z.lo),xb); break;
            case OP_MUL: if(xb.lo>0.0f||xb.hi<0.0f) upd_desired(w,in.a,div_i(z,xb),xa); if(xa.lo>0.0f||xa.hi<0.0f) upd_desired(w,in.b,div_i(z,xa),xb); break;
            case OP_NEG: upd_desired(w,in.a,I(-z.hi,-z.lo),xa); break;
            case OP_SQUARE: { float s=sqrtf(fmaxf(0.0f,z.hi)); upd_desired(w,in.a,I(-s,s),xa);} break;
            case OP_SQRT: if(z.hi>=0.0f){ float zl=fmaxf(0.0f,z.lo), zh=fmaxf(0.0f,z.hi); upd_desired(w,in.a,I(zl*zl,zh*zh),xa);} break;
            case OP_MAX: upd_desired(w,in.a,I(NEG_INF,z.hi),xa); upd_desired(w,in.b,I(NEG_INF,z.hi),xb); break;
            case OP_MIN:
                upd_desired(w,in.a,I(z.lo,xa.hi),xa);
                upd_desired(w,in.b,I(z.lo,xb.hi),xb);
                if(xa.lo > z.hi) upd_desired(w,in.b,I(NEG_INF,z.hi),xb);
                if(xb.lo > z.hi) upd_desired(w,in.a,I(NEG_INF,z.hi),xa);
                break;
        }
    }
    int xl=tp->xlocal, yl=tp->ylocal;
    Interval xr = (xl>=0 && w->desired_tag[xl]==w->cur_tag ? intersect_i(w->desired[xl], I(domain.x0,domain.x1)) : I(domain.x0,domain.x1));
    Interval yr = (yl>=0 && w->desired_tag[yl]==w->cur_tag ? intersect_i(w->desired[yl], I(domain.y0,domain.y1)) : I(domain.y0,domain.y1));
    if(!valid_i(xr) || !valid_i(yr)) return false;
    *out=(Box){xr.lo,xr.hi,yr.lo,yr.hi}; return true;
}

static inline bool refine_iters(Worker* w, const CompactTape* tp, Box in, int rounds, Box* out){
    Box cur=in;
    for(int i=0;i<rounds;i++){
        Box prev = cur;
        Box nxt; if(!contract_once(w,tp,cur,&nxt)) return false;
        cur.x0=fmaxf(cur.x0,nxt.x0); cur.x1=fminf(cur.x1,nxt.x1); cur.y0=fmaxf(cur.y0,nxt.y0); cur.y1=fminf(cur.y1,nxt.y1);
        if(cur.x0>cur.x1 || cur.y0>cur.y1) return false;
        if(cur.x0 == prev.x0 && cur.x1 == prev.x1 && cur.y0 == prev.y0 && cur.y1 == prev.y1) break;
    }
    *out=cur; return true;
}

static inline void split_box(Box b, Box out[2]){ float dx=b.x1-b.x0, dy=b.y1-b.y0; if(dx>=dy){ float xm=0.5f*(b.x0+b.x1); out[0]=(Box){b.x0,xm,b.y0,b.y1}; out[1]=(Box){xm,b.x1,b.y0,b.y1}; } else { float ym=0.5f*(b.y0+b.y1); out[0]=(Box){b.x0,b.x1,b.y0,ym}; out[1]=(Box){b.x0,b.x1,ym,b.y1}; } }

static inline int multi_box_refine(Worker* w, const CompactTape* tp, Box* out_boxes){
    Box base; if(!refine_iters(w,tp,(Box){-1,1,-1,1},BASE_REFINE_ROUNDS,&base) || box_area(base)<=0.0f) return 0;
    Box leaves[MAX_BOXES_PER_TERM]; int nleaves=1; leaves[0]=base;
    for(int depth=0; depth<MAX_SPLIT_DEPTH; depth++){
        Box next[MAX_BOXES_PER_TERM]; int nnext=0; bool changed=false;
        for(int i=0;i<nleaves;i++){
            Box b=leaves[i]; float ba=box_area(b);
            if(ba<=AREA_THRESHOLD || nnext + 2 > MAX_BOXES_PER_TERM){ next[nnext++]=b; continue; }
            Box kids[2]; split_box(b,kids); Box kept[2]; int nkept=0; float union_area=0.0f;
            for(int k=0;k<2;k++){ Box rb; if(refine_iters(w,tp,kids[k],CHILD_REFINE_ROUNDS,&rb)){ float ra=box_area(rb); if(ra>0.0f){ kept[nkept++]=rb; union_area += ra; } } }
            if(nkept>=1 && union_area < UNION_AREA_SHRINK*ba){ for(int k=0;k<nkept;k++) next[nnext++]=kept[k]; changed=true; }
            else next[nnext++]=b;
        }
        memcpy(leaves,next,nnext*sizeof(Box)); nleaves=nnext; if(!changed) break;
    }
    for(int i=0;i<nleaves;i++) out_boxes[i] = leaves[i];
    return nleaves;
}

static inline int clampi(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }

static inline void eval_box_pixels_tp(Worker* w, const CompactTape* tp, Box b){
    int x0 = clampi((int)ceilf ((b.x0 + 1.0f) * 0.5f * (IMAGE_W - 1)), 0, IMAGE_W);
    int x1 = clampi((int)floorf((b.x1 + 1.0f) * 0.5f * (IMAGE_W - 1)), -1, IMAGE_W - 1);
    int y0 = clampi((int)ceilf ((b.y0 + 1.0f) * 0.5f * (IMAGE_H - 1)), 0, IMAGE_H);
    int y1 = clampi((int)floorf((b.y1 + 1.0f) * 0.5f * (IMAGE_H - 1)), -1, IMAGE_H - 1);
    if(x1 < x0 || y1 < y0) return;
    int width = x1 - x0 + 1;

    for(int vi=0; vi<tp->nxops; vi++){
        int v = tp->xops[vi];
        Instr in = tp->ins[v];
        float *dst = w->vals[v];
        switch(in.op){
            case OP_CONST:
                #pragma omp simd
                for(int ix=0; ix<width; ix++) dst[ix] = in.fa;
                break;
            case OP_VARX:
                #pragma omp simd
                for(int ix=0; ix<width; ix++) dst[ix] = gxcoords[x0 + ix];
                break;
            case OP_ADD: {
                const float *a = w->vals[in.a], *bb = w->vals[in.b];
                #pragma omp simd
                for(int ix=0; ix<width; ix++) dst[ix] = a[ix] + bb[ix];
            } break;
            case OP_SUB: {
                const float *a = w->vals[in.a], *bb = w->vals[in.b];
                #pragma omp simd
                for(int ix=0; ix<width; ix++) dst[ix] = a[ix] - bb[ix];
            } break;
            case OP_MUL: {
                const float *a = w->vals[in.a], *bb = w->vals[in.b];
                #pragma omp simd
                for(int ix=0; ix<width; ix++) dst[ix] = a[ix] * bb[ix];
            } break;
            case OP_NEG: {
                const float *a = w->vals[in.a];
                #pragma omp simd
                for(int ix=0; ix<width; ix++) dst[ix] = -a[ix];
            } break;
            case OP_SQUARE: {
                const float *a = w->vals[in.a];
                #pragma omp simd
                for(int ix=0; ix<width; ix++) { float t = a[ix]; dst[ix] = t * t; }
            } break;
            case OP_SQRT: {
                const float *a = w->vals[in.a];
                #pragma omp simd
                for(int ix=0; ix<width; ix++) dst[ix] = sqrtf(a[ix]);
            } break;
            case OP_MIN: {
                const float *a = w->vals[in.a], *bb = w->vals[in.b];
                #pragma omp simd
                for(int ix=0; ix<width; ix++) dst[ix] = fminf(a[ix], bb[ix]);
            } break;
            case OP_MAX: {
                const float *a = w->vals[in.a], *bb = w->vals[in.b];
                #pragma omp simd
                for(int ix=0; ix<width; ix++) dst[ix] = fmaxf(a[ix], bb[ix]);
            } break;
            case OP_VARY:
                break;
        }
    }

    for(int py = y0; py <= y1; py++){
        float yv = gycoords[py];
        for(int vi=0; vi<tp->nyops; vi++){
            int v = tp->yops[vi];
            Instr in = tp->ins[v];
            float *dst = w->vals[v];
            switch(in.op){
                case OP_CONST:
                    break;
                case OP_VARY:
                    #pragma omp simd
                    for(int ix=0; ix<width; ix++) dst[ix] = yv;
                    break;
                case OP_VARX:
                    break;
                case OP_ADD: {
                    const float *a = w->vals[in.a], *bb = w->vals[in.b];
                    #pragma omp simd
                    for(int ix=0; ix<width; ix++) dst[ix] = a[ix] + bb[ix];
                } break;
                case OP_SUB: {
                    const float *a = w->vals[in.a], *bb = w->vals[in.b];
                    #pragma omp simd
                    for(int ix=0; ix<width; ix++) dst[ix] = a[ix] - bb[ix];
                } break;
                case OP_MUL: {
                    const float *a = w->vals[in.a], *bb = w->vals[in.b];
                    #pragma omp simd
                    for(int ix=0; ix<width; ix++) dst[ix] = a[ix] * bb[ix];
                } break;
                case OP_NEG: {
                    const float *a = w->vals[in.a];
                    #pragma omp simd
                    for(int ix=0; ix<width; ix++) dst[ix] = -a[ix];
                } break;
                case OP_SQUARE: {
                    const float *a = w->vals[in.a];
                    #pragma omp simd
                    for(int ix=0; ix<width; ix++) { float t = a[ix]; dst[ix] = t * t; }
                } break;
                case OP_SQRT: {
                    const float *a = w->vals[in.a];
                    #pragma omp simd
                    for(int ix=0; ix<width; ix++) dst[ix] = sqrtf(a[ix]);
                } break;
                case OP_MIN: {
                    const float *a = w->vals[in.a], *bb = w->vals[in.b];
                    #pragma omp simd
                    for(int ix=0; ix<width; ix++) dst[ix] = fminf(a[ix], bb[ix]);
                } break;
                case OP_MAX: {
                    const float *a = w->vals[in.a], *bb = w->vals[in.b];
                    #pragma omp simd
                    for(int ix=0; ix<width; ix++) dst[ix] = fmaxf(a[ix], bb[ix]);
                } break;
            }
        }
        const float *rootv = w->vals[tp->root];
        uint8_t *row = &image[py * IMAGE_W + x0];
        #pragma omp simd
        for(int ix=0; ix<width; ix++) if(rootv[ix] < 0.0f) row[ix] = 255;
    }
}

static void process_terms_streaming_omp(Worker* w){
    int local_box_count = 0;
    Box local_boxes[MAX_BOXES_PER_TERM];
    #pragma omp for schedule(OMP_SCHEDULE_KIND) nowait
    for(int i=0;i<nfrontier;i++){
        CompactTape tp;
        if(!build_compact_tape_term(w, i, &tp)) continue;
        int nb = multi_box_refine(w, &tp, local_boxes);
        for(int k=0;k<nb;k++) eval_box_pixels_tp(w, &tp, local_boxes[k]);
        local_box_count += nb;
    }
    if(local_box_count){
        #pragma omp atomic
        nwork_boxes += local_box_count;
    }
}

static void clear_image_omp(void){
    #pragma omp for schedule(static) nowait
    for(int i=0;i<IMAGE_W*IMAGE_H;i++) image[i] = 0;
}

static void run_full_pipeline_omp(Worker* w){
    #pragma omp single
    extract_frontier();
    #pragma omp single
    nwork_boxes = 0;
    clear_image_omp();
    #pragma omp barrier
    process_terms_streaming_omp(w);
    #pragma omp barrier
}

static int checksum_image(void){
    int sum = 0;
    for(int i=0;i<IMAGE_W*IMAGE_H;i++) sum += image[i];
    return sum;
}

static void write_image_ppm(const char* path){
    FILE* f = fopen(path, "wb");
    if(!f){
        perror("fopen");
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", IMAGE_W, IMAGE_H);
    for(int y = IMAGE_H - 1; y >= 0; y--){
        const uint8_t* row = &image[y * IMAGE_W];
        for(int x = 0; x < IMAGE_W; x++){
            uint8_t pixel[3] = { row[x], row[x], row[x] };
            if(fwrite(pixel, sizeof(pixel), 1, f) != 1){
                perror("fwrite");
                fclose(f);
                exit(1);
            }
        }
    }
    if(fclose(f) != 0){
        perror("fclose");
        exit(1);
    }
}

static int run_full_pipeline_parallel(void){
    #pragma omp parallel
    {
        Worker w;
        build_worker_init(&w);
        run_full_pipeline_omp(&w);
    }
    return nwork_boxes;
}

int main(int argc, char** argv){ init_coords();
    const char* path = argc>1? argv[1] : "/mnt/data/prospero.vm";
    const char* ppm_path = argc>2? argv[2] : NULL;
    load_vm(path);
    int max_threads=1;
    #ifdef _OPENMP
    max_threads = omp_get_max_threads();
    #endif
    int warm_boxes = run_full_pipeline_parallel();
    int warm_sum = checksum_image();
    printf("Full pipeline + frontier extraction + raster/eval benchmark\n");
    printf("instructions: %d\nfrontier_terms: %d\nimage: %dx%d\nmax_threads: %d\n", nins, nfrontier, IMAGE_W, IMAGE_H, max_threads);
    const int runs=BENCH_RUNS; double times[runs]; int boxes=-1, sum=-1;
    double timed_t0 = 0.0;
    #pragma omp parallel
    {
        Worker w;
        build_worker_init(&w);
        for(int r=0;r<runs;r++){
            #pragma omp single
            timed_t0 = now_sec();
            run_full_pipeline_omp(&w);
            #pragma omp single
            {
                double t1=now_sec();
                times[r]=(t1-timed_t0)*1000.0;
                boxes = nwork_boxes;
                sum = checksum_image();
                if(boxes!=warm_boxes || sum!=warm_sum){ fprintf(stderr,"mismatch boxes=%d/%d checksum=%d/%d\n", boxes, warm_boxes, sum, warm_sum); exit(1); }
            }
        }
    }
    double agg=0,minv=1e9,maxv=0; for(int i=0;i<runs;i++){ agg+=times[i]; if(times[i]<minv) minv=times[i]; if(times[i]>maxv) maxv=times[i]; }
    printf("threads=%d boxes=%d checksum=%d runs_ms:", max_threads, boxes, sum);
    for(int i=0;i<runs;i++) printf(" %.3f", times[i]);
    printf("\nmean_ms=%.3f min_ms=%.3f max_ms=%.3f\n", agg/runs, minv, maxv);
    if(ppm_path) write_image_ppm(ppm_path);
    return 0;
}
