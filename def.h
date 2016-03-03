#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <libgen.h>                    // dirname/basename
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#ifdef THREAD
#include <pthread.h>
#endif

#define XXL_VER "v0 spiral insana"
#define TESTITERS 1
#ifndef DEBUG
#define DEBUG 0
#endif

/* Control structures of sorts: */
#define MAX(a,b) \
	({ __typeof__ (a) _a = (a); \
		 __typeof__ (b) _b = (b); \
		 _a > _b ? _a : _b; })
#define MIN(a,b) \
	({ __typeof__ (a) _a = (a); \
		 __typeof__ (b) _b = (b); \
		 _a < _b ? _a : _b; })
#define ABS(a) ( (a)<0 ? -1*(a) : (a) )
#define CASE(a,b) case (a): b; break
#define FOR(st,en,stmt) ({ int _i;for(_i=(st);_i<(en);_i++)stmt; })
#define IF_RET(cond,thing) if((cond)) return thing
#define ITER(thing,n,body) ({ int _i;for(_i=0;_i<sizeof(thing);_i++) { typeof(*thing) _x; _x=thing[_i]; body; } })
#define ITERV(x,body) ({ int _i;for(_i=0;_i<x->n;_i++) { body; } })
#define ITER2(x,y,body) ({ \
	int _i; int _j; \
	if(x==NULL || y==NULL || x->n==0 || y->n==0) { \
	} else if(x->n > 1) { \
		for(_i=0;_i<x->n;_i++) { \
			if(y->n > 1) { \
				for (_j=0;_j<y->n;_j++) { \
					if(IS_i(x)&&IS_i(y)) { \
						int _x = EL(x,int,_i); \
						int _y = EL(y,int,_i); \
						PF("_x %d,_y %d\n", _x, _y); \
						body; \
					} \
				} \
			} \
		} \
	} \
})
#define PERR(msg) {perror(msg);exit(1);}

/* Debugging: */
#define APF(sz,fmt,...) ({ int len=strlen(s); if(sz-len>5) { snprintf(s+len,sz-len,fmt,__VA_ARGS__); } s; })
#define ASSERT(cond,txt) ({ if (!(cond)) { printf("ASSERT: %s\n", txt); raise(SIGABRT); exit(1); } })
#define P0(fmt,x) ({ typeof(x) xx=x; char* s=malloc(1024);snprintf(fmt,1024,xx); xx; })
#define _PF(...) ({ FOR(0,PF_LVL,printf("  ")); printf(__VA_ARGS__);})
#define PF(...) (DEBUG && PF_LVL && _PF(__VA_ARGS__))
#define PFIN() (DEBUG && PF_LVL > 0 && PF_LVL++)
#define PFOUT() (DEBUG && PF_LVL > 0 && PF_LVL--)
#define PFW(stmt) ({ int opf=PF_LVL; PF_LVL++; PFIN(); stmt; PFOUT(); PF_LVL=opf; })
#define MEMPF(...) (MEM_WATCH && IN_OUTPUT_HANDLER==0 && _PF(__VA_ARGS__))
#if DEBUG 
	#define DUMP(x) ({ if (PF_LVL) { char* s = reprA(x); PF("%s\n", s); free(s); } x; })
	#define DUMPRAW(x,sz) ({ printf("%p ",x); FOR(0,sz,printf("%d ",x[_i])); printf("\n"); x; })
#else
	#define DUMP(x) ({})
	#define DUMPRAW(x,sz) ({})
#endif

/* Element access and type checks*/
#define BUF(v) ((buf_t)( (v)->alloc ? (v->dyn) : (buf_t)&((v)->st) )) // data ptr
#define EL(v,type,n) (((type*)BUF(v))[n])                             // ..as type, for index n
// TODO ELl() and friends should do more checking on arguments, or provide a safe wrapper to do it 
// for callers - perhaps simply delist(v,n)
#define ELl(v,n) ((EL(v,VP,n)))                                       // ..deref linked list item
#define ELb(v,n) ELsz(v,1,n)                                          // ..as a byte* for index n
#define ELi(v,n) ((BUF(v))+((v->itemsz)*(n)))                         // ..for index n (no type assumed)
#define ELsz(v,sz,n) ((BUF(v))+(sz*n))                                // ..for index n, when casted to size = sz

// Handy functions for manipulating values and their types - from simple, to complex
#define LEN(v) ((v)->n)
#define SCALAR(v) ((v)->n==1)                            // is v a single value?
#define NUM(v) (IS_c(v)||IS_b(v)||IS_i(v)||IS_j(v)||IS_o(v)||IS_f(v))      // is v an int type?
#define SIMPLE(v) (IS_t(v)||IS_c(v)||IS_b(v)||IS_i(v)||IS_j(v)||IS_o(v)||IS_f(v))
#define COMPARABLE(v) (NUM(v) || IS_c(v))
#define LIST(v) ((v)->t==0)                              // is v a general list type?
#define LIST_of_lists(v) (LIST(v) && LEN(v) && LIST(ELl(v,0)))
#define ENLISTED(v) (LIST(v)&&SCALAR(v))                 // is v a single item inside a list?
#define DISCLOSE(v) (ENLISTED(v) ? LIST_first(v) : v)
#define EMPTYLIST(v) (LIST(v)&&v->n==0)                  // empty list
#define LIST_first(v) (ELl(v,0))
#define LIST_item(v,n) (ELl(v,n))
#define KEYS(v) (ELl(v,0))                               // keys for dict/table/ctx v
#define VALS(v) (ELl(v,1))                               // values for dict/table/ctx v
#define DICT(v) (IS_d(v))                                // is v a dictionary?
#define LISTDICT(v) (IS_l(v)||IS_d(v))                   // is v a list or dictionary?
#define TABLE(v) (IS_a(v))                               // is v a dictionary?
// is v any kind of container? (i.e., non-vec but has children)
#define CONTAINER(v) ((IS_l(v)||IS_d(v)||IS_a(v)||IS_x(v)) && !IS_EXC(v))         

#define INDEXABLE(v) (CALLABLE(v) || !SCALAR(v))
#define CALLABLE(v) (IS_1(v)||IS_2(v)||IS_p(v)||IS_x(v)) // callable types - represent funcs or contexts

// Helpful macros for specific types
#define NUM_item(x,n) ( IS_i(x)?AS_i(x,n) : (IS_b(x)?AS_b(x,n) : (IS_j(x)?AS_j(x,n) : (IS_o(x)?AS_o(x,n) : -1))) )
#define NUM_val(x) NUM_item(x,0)

#define DICT_find(x,y) ({ int i = _find1(KEYS(x),y); VP result=(i==-1?((VP)NULL):ELl(VALS(x),i)); result; })
#define DICT_key_n(x,y) (ELl(KEYS(x),y))
#define DICT_val_n(x,y) (ELl(VALS(x),y))

#define TABLE_col(x,n) (ELl(VALS(x),n))
#define TABLE_col_named(x,n) ({ int i = _find1(KEYS(x),n); i==-1 ? ((VP)NULL) : (TABLE_col(x,i)); })
#define TABLE_ncols(x) (LEN(KEYS(x)))
#define TABLE_nrows(x) (LEN(VALS(x))==0 ? 0 : TABLE_col(x,0)->n)

// is this member of a context (gen list) a body of code? 
#define LAMBDAISH(ctxmem) (LIST(ctxmem)&&(CALLABLE(ELl(ctxmem,0))||(ctxmem)->tag==Ti(lambda))) 
// is this member a dictionary of scope definitions (resolvable identifiers)
#define FRAME(ctxmem) (DICT(ctxmem))
#define LAMBDAARITY(x) (AS_i(ELl(x,1),0))

#define Ti(n) (_tagnums(#n))                             // int value for tag n (literal not string)
#define Tt(n) (xt(_tagnums(#n)))                         // tag n (literal not string) as a scalar of type tag
#define TAG_class(tag) (((char)tag>='A') && ((char)tag<='Z'))

#define BEST_NUM_FIT(val) ({ int t; \
	if(val<MAX_i)t=T_i; else if (val<MAX_j)t=T_j; else t=T_o; \
	t; })
#define ALLOC_BEST(x,y) ({ \
	VP new_ = xalloc(MAX(x->t,y->t),MAX(x->n,y->n)); \
	if(UNLIKELY(x->tag))new_->tag=x->tag; new_; })
#define ALLOC_LIKE(x) ({ VP new_ = xalloc(x->t,x->n); if(UNLIKELY(x->tag))new_->tag=x->tag; new_; })
#define ALLOC_LIKE_SZ(x,sz) ({ VP new_ = xalloc(x->t,sz); if(UNLIKELY(x->tag))new_->tag=x->tag; new_; })
#define ALLOC_BEST_FIT(val,sz) (xalloc(BEST_NUM_FIT(bval),sz))

// does nothing for now, but as we ponder the switch to immutability, i thought it would be good
// to mark parts of code that change their input arguments' values, so that we can have a good map
// of the semantic and structural changes that immutability might require
#define ARG_MUTATING(x) 
// at some point this will be intelligent about not cloning values for mapped types
#define MUTATE_CLONE(x) clone(x)

#ifdef DEBUG
	#define TRACELBL(x,lbl) ( (x)->tag=lbl, x )
#else
	#define TRACELBL(x,lbl) (x) 
#endif
#define REPR_MAX_ITEMS 500

#define EXC(type,lbl,x,y) ({ \
	VP exc; exc = entag(xln(4,type,xfroms(lbl),x,y),Tt(exception));  \
	if(0) printf("exception: %s\n", bfromx(repr(exc))); \
	exc; }) 
#define IF_EXC(cond,type,msg,x,y) if((cond)) return EXC(type,msg,x,y)
#define IS_EXC(x) (x==0 || (x)->tag==Ti(exception))
// TODO if_exc doesnt give us a chance to free memory :-/
#define RETURN_IF_EXC(x) if(IS_EXC(x)) return x;

#define MEMO_sz 1024
#define MEMO_make(varname) THREADLOCAL VP varname##_key[MEMO_sz]; THREADLOCAL VP varname##_val[MEMO_sz]
#define MEMO_clear(varname) memset(varname##_key,0,MEMO_sz*sizeof(VP)); memset(varname##_val,0,MEMO_sz*sizeof(VP))
#define MEMO_check(varname,val,body,ctr) for(ctr=0; ctr<MEMO_sz; ctr++) { \
	if(val==0||varname##_key[ctr]==0)break; \
	if(varname##_key[ctr]==val){ VP memo_val=varname##_val[ctr]; body; }}
#define MEMO_set(varname,key,val,ctr) for(ctr=0; ctr<MEMO_sz; ctr++) { \
	if(varname##_key[ctr]==key || varname##_key[ctr]==NULL){ varname##_key[ctr]=key; varname##_val[ctr]=val; break; }}

// misc
#define CH_SET_A "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define CH_SET_a "abcdefghijklmnopqrstuvwxyz"
#define CH_SET_n "0123456789"
#define CH_SET_na CH_SET_n CH_SET_a
#define CH_SET_nA CH_SET_n CH_SET_A

#define IOBLOCKSZ 100*1024
#define MAXSTACK 2048
#define LIKELY(x)       __builtin_expect((x),1)
#define UNLIKELY(x)     __builtin_expect((x),0)
#define TIME(n,expr) ({ int i; clock_t st,en; \
	st=clock(); for(i=0;i<n;i++) { expr; } \
	en=clock(); printf("%0.2f", ((double)(en-st)) / CLOCKS_PER_SEC); })


// TYPES:

#define TYD(name,type) typedef type name
TYD(I8,unsigned char); TYD(I32,int); TYD(I64,__int64_t); TYD(I128,__int128_t);
TYD(type_t,I8); TYD(buf_t,I8*);

TYD(tag_t,I128);

/* Structure for most values. 'st' and 'dyn' static and dynamic storage for data */
struct V { 
	type_t t; tag_t tag; 
	I32 n; I32 cap; I32 itemsz; I32 sz; // cur number of items, capacity, size each, total sz
	I32 rc;                             // ref count. starts at 1
	unsigned int alloc:1;               // self-contained (st member) or has an allocated dynamic buffer (dyn)?
	unsigned int _pad:7; 
	buf_t next; 
	union { I8 st[32]; buf_t dyn;};};
typedef struct V* VP; /* value pointer */

typedef VP (unaryFunc)(VP x);
typedef VP (binaryFunc)(VP x,VP y);
typedef char* (reprFunc)(VP x,char*s,size_t sz);

// TODO projection C type should work with VP's of type 1/2 (un/bin func), rather than C-style function pointers
struct Proj0 { int type; union { unaryFunc* f1; binaryFunc* f2; VP ctx; }; VP left; VP right; };
typedef struct Proj0 Proj;

struct type_info { type_t t; char c; int sz; char name[32]; reprFunc* repr; };
typedef struct type_info type_info_t;

#ifdef STDLIBSHAREDLIB
struct xxl_index_t {
	char name[40];
	char implfunc[40];
	int arity;
};
#endif

#ifdef THREAD
#define THREADLOCAL __thread
#else
#define THREADLOCAL
#endif

// GLOBALS FROM xxl.c
extern VP XB0; extern VP XB1; extern VP XI0; extern VP XI1; extern I8 PF_ON; extern I8 PF_LVL; 
extern THREADLOCAL I8 IN_OUTPUT_HANDLER; 
extern I8 MEM_WATCH;

// TODO create "slice type" that literally represents a scalar value inside a pointer
