/* Copyright (C) 2022  Aaron Flin - All Rights Reserved
 * You may use, distribute this code under the
 * terms of the Rampart Source Available License.
 * see rsal.txt for details
 */
#include "txcoreconfig.h"
#include <limits.h>
#include <stdlib.h>
#include <pthread.h>
#include <ctype.h>
#include <float.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "dbquery.h"
#include "texint.h"
#include "texisapi.h"
#include "cgi.h"
#include "rampart.h"
#include "api3.h"
#include "../globals/csv_parser.h"
#include "event.h"

pthread_mutex_t tx_handle_lock;
pthread_mutex_t tx_create_lock;

// Don't like the globals, but gotta fight fire with fire
int defnoise=1, defsuffix=1, defsuffixeq=1, defprefix=1;

#define RESMAX_DEFAULT 10 /* default number of sql rows returned for select statements if max is not set */
//#define PUTMSG_STDERR                  /* print texis error messages to stderr */

#define QUERY_STRUCT struct rp_query_struct

#define QS_ERROR_DB 1
#define QS_ERROR_PARAM 2
#define QS_SUCCESS 0

QUERY_STRUCT
{
    const char *sql;    /* the sql statement (allocated by duk and on its stack) */
    duk_idx_t arr_idx;  /* location of array of parameters in ctx, or -1 */
    duk_idx_t obj_idx;
    duk_idx_t str_idx;
    duk_idx_t arg_idx;  /* location of extra argument for callback */
    duk_idx_t callback; /* location of callback in ctx, or -1 */
    int skip;           /* number of results to skip */
    int64_t max;        /* maximum number of results to return */
    signed char rettype;/* 0 for return object with key as column names,
                           1 for array
                           2 for novars                                           */
    char err;
    char getCounts;     /* whether to include metamorph counts in return */
};


duk_ret_t duk_rp_sql_close(duk_context *ctx);

extern int TXunneededRexEscapeWarning;
int texis_resetparams(TEXIS *tx);
int texis_cancel(TEXIS *tx);
/*
   info for shared memory
   which is used BOTH for thread and fork versions
   in order to keep it simple
*/
#define FORKMAPSIZE 1048576
//#define FORKMAPSIZE 2
#define FMINFO struct sql_map_info_s
FMINFO
{
    void *mem;
    void *pos;
};

#define mmap_used ((size_t)(mapinfo->pos - mapinfo->mem))
#define mmap_rem (FORKMAPSIZE - mmap_used)
#define mmap_reset do{ mapinfo->pos = mapinfo->mem;} while (0)

int thisfork =0; //set after forking.

/* shared mem for logging errors */
char **errmap=NULL;

/* one SFI struct for each thread in rampart-threads.
   Some threads can run without a fork (main thread and one thread in rampart-server)
   while the rest will fork in order to avoid locking
   around texis_* calls.

   If not in the server, or if not threading, then
   all operations will be non forking.
*/

#define SFI struct sql_fork_info_s
SFI
{
    int reader;         // pipe to read from, in parent or child
    int writer;         // pipe to write to, in parent or child
    pid_t childpid;     // process id of the child if in parent (return from fork())
    FMINFO *mapinfo;    // the shared mmap for reading and writing in both parent and child
    void *aux;          // if data is larger than mapsize, we need to copy it in chunks into here
    void *auxpos;
    size_t auxsz;
    FLDLST *fl;         // modified FLDLST for parent pointing to areas in mapinfo
};
SFI **sqlforkinfo = NULL;
static int n_sfi=0;


#define DB_HANDLE struct db_handle_s_list
DB_HANDLE
{
    TEXIS *tx;          // a texis handle, NULL if needs_fork!=0
    int idx;            // the index of this handle in all_handles[]
    int fork_idx;       // the index of this handle in the forks all_handles[]
    uint16_t forkno;    // corresponds to the threadno from rampart-threads. One fork per thread (excepts apply)
    char *db;           // the db name.  Handles must be closed and reopened if used on a different db.
    char inuse;         // whether this handle is being used by a transaction
    char needs_fork;    // whether we talk to a fork with this one.
};

#define NDB_HANDLES 128

DB_HANDLE *all_handles[NDB_HANDLES] = {0};

// for yosemite:
#ifdef __APPLE__
#include <Availability.h>
#  if __MAC_OS_X_VERSION_MIN_REQUIRED < 101300
#    include "fmemopen.h"
#    include "fmemopen.c"
#  endif
#endif

extern int RP_TX_isforked;

static int sql_set(duk_context *ctx, DB_HANDLE *hcache, char *errbuf);

//#define TXLOCK if(!RP_TX_isforked) {printf("lock from %d\n", thisfork);pthread_mutex_lock(&lock);}
//#define TXLOCK if(!RP_TX_isforked) pthread_mutex_lock(&lock);
//#define TXUNLOCK if(!RP_TX_isforked) pthread_mutex_unlock(&lock);

#define TXLOCK
#define TXUNLOCK

// no handle locking in forks
#define HLOCK if(!RP_TX_isforked) RP_PTLOCK(&tx_handle_lock);
//#define HLOCK if(!RP_TX_isforked) {printf("lock from %d\n", thisfork);pthread_mutex_lock(&lock);}
#define HUNLOCK if(!RP_TX_isforked) RP_PTUNLOCK(&tx_handle_lock);

int db_is_init = 0;
int tx_rp_cancelled = 0;
/*
#define EXIT_IF_CANCELLED \
    if (tx_rp_cancelled)  \
        exit(0);
*/

#define EXIT_IF_CANCELLED

#ifdef DEBUG_TX_CALLS

#define xprintf(...)                 \
    printf("(%d): ", (int)getpid()); \
    printf(__VA_ARGS__);

#else

#define xprintf(...) /* niente */

#endif

#define TEXIS_OPEN(tdb) ({                        \
    xprintf("Open\n");                            \
    TXLOCK                                        \
    TEXIS *rtx = texis_open((char *)(tdb), "PUBLIC", ""); \
    TXUNLOCK                                      \
    EXIT_IF_CANCELLED                             \
    rtx;                                          \
})

#define TEXIS_CLOSE(rtx) ({     \
    xprintf("Close\n");         \
    TXLOCK                      \
    (rtx) = texis_close((rtx)); \
    TXUNLOCK                    \
    EXIT_IF_CANCELLED           \
    rtx;                        \
})

#define TEXIS_PREP(a, b) ({           \
    xprintf("Prep\n");                \
    TXLOCK                            \
    int r = texis_prepare((a), (b));  \
    TXUNLOCK                          \
    EXIT_IF_CANCELLED                 \
    r;                                \
})

#define TEXIS_EXEC(a) ({        \
    xprintf("Exec\n");          \
    TXLOCK                      \
    int r = texis_execute((a)); \
    TXUNLOCK                    \
    EXIT_IF_CANCELLED           \
    r;                          \
})

#define TEXIS_FETCH(a, b) ({           \
    xprintf("Fetch\n");                \
    TXLOCK                             \
    FLDLST *r = texis_fetch((a), (b)); \
    TXUNLOCK                           \
    EXIT_IF_CANCELLED                  \
    r;                                 \
})

#define TEXIS_SKIP(a, b) ({               \
    xprintf("skip\n");                    \
    TXLOCK                                \
    int r = texis_flush_scroll((a), (b)); \
    TXUNLOCK                              \
    EXIT_IF_CANCELLED                     \
    r;                                    \
})

#define TEXIS_GETCOUNTINFO(a, b) ({       \
    xprintf("getCountInfo\n");            \
    TXLOCK                                \
    int r = texis_getCountInfo((a), (b)); \
    TXUNLOCK                              \
    EXIT_IF_CANCELLED                     \
    r;                                    \
})

#define TEXIS_FLUSH(a) ({                 \
    xprintf("skip\n");                    \
    TXLOCK                                \
    int r = texis_flush((a));             \
    TXUNLOCK                              \
    EXIT_IF_CANCELLED                     \
    r;                                    \
})

#define TEXIS_RESETPARAMS(a) ({           \
    xprintf("resetparams\n");             \
    TXLOCK                                \
    int r = texis_resetparams((a));       \
    TXUNLOCK                              \
    EXIT_IF_CANCELLED                     \
    r;                                    \
})

#define TEXIS_PARAM(a, b, c, d, e, f) ({               \
    xprintf("Param\n");                                \
    TXLOCK                                             \
    int r = texis_param((a), (b), (c), (d), (e), (f)); \
    TXUNLOCK                                           \
    EXIT_IF_CANCELLED                                  \
    r;                                                 \
})

void die_nicely(int sig)
{
    DB_HANDLE *h;
    int i=0;
    for (; i<NDB_HANDLES; i++)
    {
        h= all_handles[i];
        if(h && h->inuse)
        {
            texis_cancel(h->tx);
        }
    }
    tx_rp_cancelled = 1;
}


pid_t g_hcache_pid = 0;
pid_t parent_pid = 0;

#define msgbufsz 4096

#define throw_tx_error(ctx,h,pref) do{\
    char pbuf[msgbufsz] = {0};\
    duk_rp_log_tx_error(ctx,h,pbuf);\
    RP_THROW(ctx, "%s error: %s",pref,pbuf);\
}while(0)

#define clearmsgbuf() do {                \
    fseek(mmsgfh, 0, SEEK_SET);           \
    fwrite("\0", 1, 1, mmsgfh);           \
    fseek(mmsgfh, 0, SEEK_SET);           \
} while(0)

#define msgtobuf(buf)  do {                          \
    int pos = ftell(mmsgfh);                         \
    if(pos && errmap[thisfork][pos-1]=='\n') pos--;  \
    errmap[thisfork][pos]='\0';                      \
    strcpy((buf), errmap[thisfork]);                 \
    clearmsgbuf();                                   \
} while(0)

/* **************************************************
     store an error string in this.errMsg
   **************************************************   */
void duk_rp_log_error(duk_context *ctx, char *pbuf)
{
    duk_push_this(ctx);
    if(duk_has_prop_string(ctx,-1,"errMsg") )
    {
        duk_get_prop_string(ctx,-1,"errMsg");
        duk_push_string(ctx, pbuf);
        duk_concat(ctx,2);
    }
    else
        duk_push_string(ctx, pbuf);

#ifdef PUTMSG_STDERR
    if (pbuf && strlen(pbuf))
    {
//        pthread_mutex_lock(&printlock);
        fprintf(stderr, "%s\n", pbuf);
//        pthread_mutex_unlock(&printlock);
    }
#endif
    duk_put_prop_string(ctx, -2, "errMsg");
    duk_pop(ctx);
}

void duk_rp_log_tx_error(duk_context *ctx, DB_HANDLE *h, char *buf)
{
    msgtobuf(buf);
    duk_rp_log_error(ctx, buf);
    //duk_rp_log_error(ctx, errmap[thisfork]);
}

/* get the expression from a /pattern/ or a "string" */
static const char *get_exp(duk_context *ctx, duk_idx_t idx)
{
    const char *ret=NULL;

    if(duk_is_object(ctx,idx) && duk_has_prop_string(ctx,idx,"source") )
    {
        duk_get_prop_string(ctx,idx,"source");
        ret=duk_get_string(ctx,-1);
        duk_pop(ctx);
    }
    else if ( duk_is_string(ctx,idx) )
        ret=duk_get_string(ctx,idx);

    return ret;
}
/* if in a child process, bail on error.  A new child will be forked
   and hopefully everything will get back on track                    */
#define forkwrite(b,c) ({\
    int r=0;\
    r=write(finfo->writer, (b),(c));\
    if(r==-1) {\
        fprintf(stderr, "fork write failed: '%s' at %d, fd:%d\n",strerror(errno),__LINE__,finfo->writer);\
        if(thisfork) {fprintf(stderr, "child proc exiting\n");exit(0);}\
    };\
    r;\
})

#define forkread(b,c) ({\
    int r=0;\
    r= read(finfo->reader,(b),(c));\
    if(r==-1) {\
        fprintf(stderr, "fork read failed: '%s' at %d\n",strerror(errno),__LINE__);\
        if(thisfork) {fprintf(stderr, "child proc exiting\n");exit(0);}\
    };\
    r;\
})
/*
static size_t mmwrite(FMINFO *mapinfo, void *data, size_t size)
{
    if(size < mmap_rem)
    {
        memcpy(mapinfo->pos, data, size);
        mapinfo->pos += size;
        return size;
    }
    return 0;
}

static size_t mmread(FMINFO *mapinfo, void *data, size_t size)
{
    if(size < mmap_rem)
    {
        memcpy(data, mapinfo->pos, size);
        mapinfo->pos += size;
        return size;
    }
    return 0;
}
*/

static void kill_child(void *arg)
{
    pid_t *kpid = (pid_t*)arg;
    kill(*kpid,SIGTERM);
    //printf("killed child %d\n",(int)*kpid);
}

static void do_fork_loop(SFI *finfo);

#define Create 1
#define NoCreate 0

static SFI *check_fork(DB_HANDLE *h, int create)
{
    int pidstatus;
    SFI *finfo = sqlforkinfo[h->forkno];

    if(finfo == NULL)
    {
        if(!create)
        {
            fprintf(stderr, "Unexpected Error: previously opened pipe info no longer exists for forkno %d\n",h->forkno);
            exit(1);
        }
        else
        {
            //printf("creating finfo for forkno %d\n", h->forkno);
            REMALLOC(sqlforkinfo[h->forkno], sizeof(SFI));
            finfo=sqlforkinfo[h->forkno];
            finfo->reader=-1;
            finfo->writer=-1;
            finfo->childpid=0;
            /* the field list for any fetch calls*/
            finfo->fl=NULL;
            /* the shared mmap */
            finfo->mapinfo=NULL;
            /* the aux buffer */
            finfo->aux=NULL;
            finfo->auxsz=0;
            finfo->auxpos=NULL;
            REMALLOC(finfo->mapinfo, sizeof(FMINFO));

            finfo->mapinfo->mem = mmap(NULL, FORKMAPSIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_SHARED, -1, 0);;
            if(finfo->mapinfo->mem == MAP_FAILED)
            {
                fprintf(stderr, "mmap failed: %s\n",strerror(errno));
                exit(1);
            }
            finfo->mapinfo->pos = finfo->mapinfo->mem;

            errmap[h->forkno] = mmap(NULL, msgbufsz, PROT_READ|PROT_WRITE, MAP_ANON|MAP_SHARED, -1, 0);;
            if(errmap[h->forkno] == MAP_FAILED)
            {
                fprintf(stderr, "errmsg mmap failed: %s\n",strerror(errno));
                exit(1);
            }
        }

    }

    parent_pid=getpid();

    /* waitpid: like kill(pid,0) except only works on child processes */
    if (!finfo->childpid || waitpid(finfo->childpid, &pidstatus, WNOHANG))
    {
        if (!create)
            return NULL;

        int child2par[2], par2child[2];
        //signal(SIGPIPE, SIG_IGN); //macos
        //signal(SIGCHLD, SIG_IGN);
        /* our creation run.  create pipes and setup for fork */
        if (rp_pipe(child2par) == -1)
        {
            fprintf(stderr, "child2par pipe failed\n");
            return NULL;
        }

        if (rp_pipe(par2child) == -1)
        {
            fprintf(stderr, "par2child pipe failed\n");
            return NULL;
        }

        /* if child died, close old pipes */
        if (finfo->writer > 0)
        {
            close(finfo->writer);
            finfo->writer = -1;
        }
        if (finfo->reader > 0)
        {
            close(finfo->reader);
            finfo->reader = -1;
        }


        /***** fork ******/
        finfo->childpid = fork();
        if (finfo->childpid < 0)
        {
            fprintf(stderr, "fork failed");
            finfo->childpid = 0;
            return NULL;
        }

        if(finfo->childpid == 0)
        { /* child is forked once then talks over pipes. */
            struct sigaction sa = { {0} };

            RP_TX_isforked=1; /* mutex locking not necessary in fork */
            memset(&sa, 0, sizeof(struct sigaction));
            sa.sa_flags = 0;
            sa.sa_handler =  die_nicely;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGUSR2, &sa, NULL);

            setproctitle("rampart sql_helper");

            close(child2par[0]);
            close(par2child[1]);
            finfo->writer = child2par[1];
            finfo->reader = par2child[0];
            thisfork=h->forkno;
            mmsgfh = fmemopen(errmap[h->forkno], msgbufsz, "w+");
            fcntl(finfo->reader, F_SETFL, 0);

            libevent_global_shutdown();

            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);

            do_fork_loop(finfo); // loop and never come back;
        }
        else
        {
            pid_t *pidarg=NULL;

            //parent
            signal(SIGPIPE, SIG_IGN); //macos
            signal(SIGCHLD, SIG_IGN);
            rp_pipe_close(child2par,1);
            rp_pipe_close(par2child,0);
            finfo->reader = child2par[0];
            finfo->writer = par2child[1];
            fcntl(finfo->reader, F_SETFL, 0);

            // a callback to kill child
            REMALLOC(pidarg, sizeof(pid_t));
            *pidarg = finfo->childpid;
            set_thread_fin_cb(rpthread[h->forkno], kill_child, pidarg);
        }
    }

    return finfo;
}


static void free_all_handles(void *unused)
{
    DB_HANDLE *h;
    int i=0;
    for (; i<NDB_HANDLES; i++)
    {
        tx_rp_cancelled = 1;
        h= all_handles[i];
        if(h)
        {
            free(h->db);
//             if(h->inuse && h->forkno)
            if(h->inuse && h->needs_fork) //TODO: determine why we want h->needs_fork.  Can't remember.
                texis_cancel(h->tx);

            TEXIS_CLOSE(h->tx);
            free(h);
            all_handles[i]=NULL;
        }
    }
}


static void free_all_handles_noClose(void *unused)
{
    DB_HANDLE *h;
    int i=0;
    for (; i<NDB_HANDLES; i++)
    {
        h = all_handles[i];
        if(h)
        {
            free(h->db);
            free(h);
            all_handles[i]=NULL;
        }
    }
}

static DB_HANDLE *make_handle(int i, const char *db)
{
    //WARNING: thread_local_thread_num is not what you think it is after fork
    //see h_open where forkno is set to 0, needs_fork needs to be set there as well
    int thrno = get_thread_num();
    RPTHR *thr = rpthread[thrno];
    DB_HANDLE *h = NULL;

    REMALLOC(h, sizeof(DB_HANDLE));

    h->idx = i;
    h->forkno = thrno;
    h->inuse=0;

    if(RPTHR_TEST(thr, RPTHR_FLAG_THR_SAFE))
        h->needs_fork=0;
    else
        h->needs_fork=1;

    h->db=strdup(db);
    h->tx=NULL;

    return h;
}

static int fork_open(DB_HANDLE *h);

#define fromContext -1

/* find first unused handle, create as necessary */
static DB_HANDLE *h_open(const char *db, int forkno)
{
    DB_HANDLE *h = NULL;
    int i=0;

    if(forkno == fromContext)
    {
        forkno=get_thread_num();
    }

    if (g_hcache_pid != getpid())
    {
        free_all_handles_noClose(NULL);
    }

    for (i=0; i<NDB_HANDLES; i++)
    {
        h = all_handles[i];
        if(!h)
        {
            g_hcache_pid = getpid();
            h=make_handle(i,db);
            all_handles[i]=h;
            break;
        }
        else
        {
            if(!h->inuse && !strcmp(db, h->db) && forkno == h->forkno)
            {
                break;
            }
        }
        h=NULL; // no suitable candidate found.
    }
    /* no handle open with same db, need to close an unused one and reopen*/
    if(!h)
    {
        HLOCK
        for (i=0; i<NDB_HANDLES; i++)
        {
            h = all_handles[i];
            if(!h->inuse)
            {
                if(h->tx)
                {
                    texis_close(h->tx);
                }
                free(h);
                h = all_handles[i] = make_handle(i, db);
                h->inuse=1;
                break;
            }
            h = NULL;
        }
        HUNLOCK
    }
    if(h)
    {
        // not sure we need to lock this early, but definitely need to lock on fork_open
        // where the sqlforkinfo array is created and initialized.
        HLOCK
        h->inuse=1;
        h->forkno = forkno;
        if(h->forkno==0)  //forkno 0 is always needs_fork==0. Set again for child proc
            h->needs_fork=0;
        if(h->needs_fork)
        {
            h->fork_idx = fork_open(h);
        }
        else if (h->tx == NULL)
            h->tx = texis_open((char *)(db), "PUBLIC", "");
        HUNLOCK
    }
    return h;
}

static inline void release_handle(DB_HANDLE *h)
{
    h->inuse=0;
}

/*********** CHILD/PARENT FUNCTION PAIRS ************/

static int get_chunks(SFI *finfo, int size)
{
    int pos=0;
    size *= -1;

    if(finfo->auxsz < FORKMAPSIZE * 2)
    {
        finfo->auxsz = FORKMAPSIZE * 2;
        REMALLOC(finfo->aux, finfo->auxsz);
    }

    while(1)
    {
        finfo->auxpos = finfo->aux + pos;
        memcpy(finfo->auxpos, finfo->mapinfo->mem, size);
        pos += size; // for next round
        if(forkwrite("C",sizeof(char))==-1) //ask for more
            return 0;
        if(forkread(&size,sizeof(int))==-1) //get the next chunk size
            return 0;
        if(size > -1) //we are done, get remaining data
        {
            if(size + pos > finfo->auxsz)
            {
                finfo->auxsz += size;
                REMALLOC(finfo->aux, finfo->auxsz);
            }
            finfo->auxpos = finfo->aux + pos;
            memcpy(finfo->auxpos, finfo->mapinfo->mem, size);

            return (int)size; //size of final chunk
        }
        size *=-1;
        if (size + pos > finfo->auxsz)
        {
            finfo->auxsz *=2;
            REMALLOC(finfo->aux, finfo->auxsz);
        }
    }
    return 0; // no nag
}

static FLDLST * fork_fetch(DB_HANDLE *h,  int stringsFrom)
{
    SFI *finfo = check_fork(h, NoCreate);
    FLDLST *ret=NULL;
    int i=0, retsize=0;
    int *ilst=NULL;
    FMINFO *mapinfo;
    size_t eos=0;
    void *buf;

    mapinfo = finfo->mapinfo;
    buf=mapinfo->mem;

    if(!finfo)
        return NULL;

    if(forkwrite("f", sizeof(char)) == -1)
        return NULL;

    if(forkwrite(&(h->fork_idx), sizeof(int)) == -1)
        return NULL;

    if(forkwrite(&(stringsFrom), sizeof(int)) == -1)
        return NULL;

    if(forkread(&retsize, sizeof(int)) == -1)
        return NULL;

    if(retsize==-1)
    {
        // -1 means error or no more rows
        if(finfo->aux)
        {
            free(finfo->aux);
            finfo->aux=NULL;
            finfo->auxsz=0;
            finfo->auxpos=NULL;
        }
        return NULL;
    }
    else if (retsize<-1)
    {
        //not enough space in memmap for entire response
        retsize=get_chunks(finfo,retsize);
        buf = finfo->aux;
    }

    /* unserialize results and make a new fieldlist */
    if (finfo->fl == NULL)
    {
        REMALLOC(finfo->fl, sizeof(FLDLST));
        finfo->fl->n=0;
        memset(finfo->fl, 0, sizeof(FLDLST));
    }
    ret = finfo->fl;

    /* first int is fl->n */
    ilst = buf;
    ret->n = ilst[0];
    eos += sizeof(int);

    /* types is an array of ints at beginning */
    ilst=(buf) + eos;
    for (i=0;i<ret->n;i++)
        ret->type[i]=ilst[i];
    eos += sizeof(int) * ret->n;

    /* ndata is an array of ints following type ints */
    ilst=(buf) + eos;
    for (i=0;i<ret->n;i++)
        ret->ndata[i] = ilst[i];
    eos += sizeof(int) * ret->n;

    /* next an array of null terminated strings for names */
    for (i=0;i<ret->n;i++)
    {
        ret->name[i]= (buf) + eos;
        eos += strlen(ret->name[i]) + 1;
    }

    /* last is the data itself.  Each field is not necessarily NULL terminated
       and strings may be shorter than field width */
    for (i=0;i<ret->n;i++)
    {
        char type = ret->type[i] & 0x3f;
        size_t type_size = ddftsize(type),
               size = type_size * (size_t)ret->ndata[i];
        if(size==0)
            ret->data[i]=NULL;
        else
        {
            size_t size_mod = eos % type_size;

            //align to type
            if (size_mod)
                eos += (type_size - size_mod);

            ret->data[i]= (buf) + eos;
            eos += size;
        }
    }
    return ret;
}

static int cwrite(SFI *finfo, void *data, size_t sz)
{
    FMINFO *mapinfo = finfo->mapinfo;
    size_t rem = mmap_rem; //space available in mmap
    char c;
    int used = -1 * FORKMAPSIZE;

    while (rem < sz)
    {
        memcpy(mapinfo->pos, data, rem);
        /* send negative to signal chunk */
        if( forkwrite(&used,sizeof(int)) == -1)
            return 0;
        /* wait for parent to be ready for next */
        if( forkread(&c,sizeof(char)) == -1)
            return 0;
        /* reset to beginning of map mem */
        mapinfo->pos=mapinfo->mem;
        data += rem;
        sz -= rem;
        rem = FORKMAPSIZE;
    }

    memcpy(mapinfo->pos, data, sz);
    mapinfo->pos += sz;

    return 1;
}

static int cwrite_aligned(SFI *finfo, void *data, size_t sz, size_t tsz)
{
    FMINFO *mapinfo = finfo->mapinfo;

    //align to type
    if(tsz>1)
    {
        size_t sz_mod = (size_t)mapinfo->pos % tsz;
        if(sz_mod)
            mapinfo->pos += (tsz - sz_mod);
    }

    return cwrite(finfo, data, sz);

}

static int serialize_fl(SFI *finfo, FLDLST *fl)
{
    FMINFO *mapinfo = finfo->mapinfo;
    int i=0;
    mmap_reset;

    /* single int = fl->n */
    if(!cwrite(finfo, &(fl->n), sizeof(int)))
        return -1;

    /* array of ints for type*/
    for (i = 0; i < fl->n; i++)
    {
        int type =  fl->type[i];
        if(!cwrite(finfo, &type, sizeof(int)))
            return -1;
    }

    /* array of ints for ndata */
    for (i = 0; i < fl->n; i++)
    {
        int ndata =  fl->ndata[i];

        // TODO: I think this is always the case, but check with TS
        // before removing the next two lines.
        if(fl->data[i] == NULL)
            ndata=0;

        if(!cwrite(finfo, &ndata, sizeof(int)))
            return -1;
    }

    /* array of names -> [col_name1, \0, col_name2, \0 ...] */
    for (i = 0; i < fl->n; i++)
    {
        char *name =  fl->name[i];
        size_t l = strlen(name)+1; //include the \0
        if(!cwrite(finfo, name, l))
            return -1;
    }
    /* data in seq - length of each determined by sizeof(type) * ndata */
    for (i = 0; i < fl->n; i++)
    {
        char type = fl->type[i] & 0x3f;
        size_t type_size = ddftsize(type),
               size = type_size * (size_t)fl->ndata[i];
        if(size !=0 && fl->data[i] != NULL)
        {
            // align ints, etc for arm
            if(!cwrite_aligned(finfo, fl->data[i], size, type_size))
                return -1;
        }
    }

    return (int) mmap_used;
}


static int child_fetch(SFI *finfo, int *idx)
{
    int stringFrom=-9999;
    DB_HANDLE *h;
    int ret=-1;
    FLDLST *fl;

    /* idx */
    if (forkread(idx, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(size_t));
        return 0;
    }
    if(*idx != -1)
    {
        h=all_handles[*idx];
    }
    else
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    /* stringFrom */
    if (forkread(&stringFrom, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }
    if(stringFrom == -9999)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    fl = texis_fetch(h->tx, stringFrom);

    if(fl)
        ret = serialize_fl(finfo,fl);

    if(forkwrite(&ret, sizeof(int)) == -1)
        return 0;

    if(ret<0)
        return 0;

    return ret;
}

static int fork_param(
    DB_HANDLE *h,
    int ipar,
    void *buf,
    long *len,
    int ctype,
    int sqltype
)
{
    SFI *finfo = check_fork(h, NoCreate);
    int ret=0;
    FMINFO *mapinfo;

    if(!finfo)
        return 0;

    mapinfo = finfo->mapinfo;
    mmap_reset;
    if(forkwrite("P", sizeof(char)) == -1)
        return ret;

    if(forkwrite(&(h->fork_idx), sizeof(int)) == -1)
        return 0;

    if(!cwrite(finfo, &ipar, sizeof(int)))
        return 0;

    if(!cwrite(finfo, &ctype, sizeof(int)))
        return 0;

    if(!cwrite(finfo, &sqltype, sizeof(int)))
        return 0;

    if(!cwrite(finfo, len, sizeof(long)))
        return 0;

    if(!cwrite(finfo, buf, (size_t)*len))
        return 0;

    ret = (int)mmap_used;

    if(forkwrite(&ret, sizeof(int)) == -1)
        return 0;

    if(forkread(&ret, sizeof(int)) == -1)
        return 0;

    return ret;
}

static int child_param(SFI *finfo, int *idx)
{
    DB_HANDLE *h;
    int ret=0, retsize=0;
    void *buf = finfo->mapinfo->mem;
    int ipar, ctype, sqltype;
    long *len;
    int *ip;
    void *data;
    size_t pos=0;

    /* idx */
    if (forkread(idx, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(size_t));
        return 0;
    }
    if(*idx != -1)
    {
        h=all_handles[*idx];
    }
    else
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    if(forkread(&retsize, sizeof(int)) == -1)
        return 0;

    if (retsize<0)
    {
        //not enough space in memmap for entire response
        retsize=get_chunks(finfo,retsize);
        buf = finfo->aux;
    }

    ip = buf;
    ipar = *ip;
    pos += sizeof(int);

    ip = pos + buf;
    ctype = *ip;
    pos += sizeof(int);

    ip = pos + buf;
    sqltype = *ip;
    pos += sizeof(int);

    len = pos + buf;
    pos += sizeof(long);

    data = pos + buf;

    ret = texis_param(h->tx, ipar, data, len, ctype, sqltype);

    if(finfo->aux)
    {
        free(finfo->aux);
        finfo->aux=NULL;
        finfo->auxsz=0;
        finfo->auxpos=NULL;
    }

    if(forkwrite(&ret, sizeof(int)) == -1)
        return 0;

    return ret;
}



static int fork_exec(DB_HANDLE *h)
{
    SFI *finfo = check_fork(h, NoCreate);
    int ret=0;

    if(!finfo)
        return 0;

    if(forkwrite("e", sizeof(char)) == -1)
        return ret;

    if(forkwrite(&(h->fork_idx), sizeof(int)) == -1)
        return ret;

    if(forkread(&ret, sizeof(int)) == -1)
        return 0;

    return ret;
}

static int child_exec(SFI *finfo, int *idx)
{
    DB_HANDLE *h;
    int ret=0;

    if (forkread(idx, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    if(*idx != -1)
    {
        h=all_handles[*idx];
    }
    else
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    ret = texis_execute(h->tx);
    if (forkwrite(&ret, sizeof(int))  == -1)
        return 0;

    return ret;
}

static int fork_prep(DB_HANDLE *h, char *sql)
{
    SFI *finfo = check_fork(h, NoCreate);
    int ret=0;

    if(!finfo)
        return 0;

    /* write sql statement to start of mmap */

    sprintf(finfo->mapinfo->mem,"%s", sql);

    if(forkwrite("p", sizeof(char)) == -1)
        return 0;

    if(forkwrite(&(h->fork_idx), sizeof(int)) == -1)
        return 0;

    // get the result back
    if(forkread(&ret, sizeof(int)) == -1)
    {
        return 0;
    }

    return ret;
}

static int child_prep(SFI *finfo, int *idx)
{
    DB_HANDLE *h;
    int ret=0;
    char *sql=finfo->mapinfo->mem;

    if (forkread(idx, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    if(*idx != -1)
    {
        h=all_handles[*idx];
    }
    else
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    ret = texis_prepare(h->tx, sql);
    if(forkwrite(&ret, sizeof(int)) == -1)
        return 0;

    return ret;
}

static int fork_open(DB_HANDLE *h)
{
    SFI *finfo;
    int ret=-1;
    if(n_sfi < nrpthreads)
    {
        int i=n_sfi;
        REMALLOC(sqlforkinfo, nrpthreads  * sizeof(SFI *));
        REMALLOC(errmap, nrpthreads  * sizeof(char *));
        n_sfi = nrpthreads;
        while (i<n_sfi)
        {
            if(i) /* errmap[0] is elsewhere */
                errmap[i]=NULL;
            sqlforkinfo[i++]=NULL;
        }
    }

    finfo = check_fork(h, Create);
    if(finfo->childpid)
    {
        /* write db string to map */
        sprintf(finfo->mapinfo->mem, "%s", h->db);

        /* write o for open and the string db is in memmap */
        if(forkwrite("o", sizeof(char)) == -1)
            return ret;

        if(forkread(&ret, sizeof(int)) == -1)
        {
            return -1;
        }
    }
    return ret;
}

static int child_open(SFI *finfo, int *idx)
{
    DB_HANDLE *h;
    char *db = finfo->mapinfo->mem;

    h=h_open(db,0);
    if(h)
        *idx = h->idx;

    if(forkwrite(idx, sizeof(int)) == -1)
        return 0;

    return 1;
}

static int fork_close(DB_HANDLE *h)
{
    SFI *finfo = check_fork(h, NoCreate);
    int ret=0;

    if(!finfo)
        return 0;

    if(forkwrite("c", sizeof(char)) == -1)
        return 0;

    if(forkwrite(&(h->fork_idx), sizeof(int)) == -1)
        return 0;

    if(forkread(&ret, sizeof(int)) == -1)
        return 0;

    release_handle(h);
    return ret;
}

static int child_close(SFI *finfo, int *idx)
{
    int ret=0;

    if (forkread(idx, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    if(*idx != -1)
    {
        release_handle(all_handles[*idx]);
        ret=1;
    }
    else
        ret=0;

    if (forkwrite(&ret, sizeof(int))  == -1)
        return 0;

    return ret;
}

static int fork_flush(DB_HANDLE *h)
{
    SFI *finfo = check_fork(h, NoCreate);
    int ret=0;

    if(!finfo)
        return 0;

    if(forkwrite("F", sizeof(char)) == -1)
        return 0;

    if(forkwrite(&(h->fork_idx), sizeof(int)) == -1)
        return 0;

    if(forkread(&ret, sizeof(int)) == -1)
        return 0;

    return ret;
}

static int child_flush(SFI *finfo, int *idx)
{
    int ret=0;

    if (forkread(idx, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    if(*idx != -1)
    {
        ret = texis_flush(all_handles[*idx]->tx);
    }
    else
        ret=0;

    if (forkwrite(&ret, sizeof(int))  == -1)
        return 0;

    return ret;
}

static int fork_getCountInfo(DB_HANDLE *h, TXCOUNTINFO *countInfo)
{
    SFI *finfo = check_fork(h, NoCreate);
    int ret=0;

    if(!finfo)
        return 0;

    if(forkwrite("g", sizeof(char)) == -1)
        return 0;

    if(forkwrite(&(h->fork_idx), sizeof(int)) == -1)
        return 0;

    if(forkread(&ret, sizeof(int)) == -1)
        return 0;

    if(ret)
    {
        memcpy(countInfo, finfo->mapinfo->mem, sizeof(TXCOUNTINFO));
    }

    return ret;
}

static int child_getCountInfo(SFI *finfo, int *idx)
{
    int ret=0;
    TXCOUNTINFO *countInfo = finfo->mapinfo->mem;

    if (forkread(idx, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    if(*idx != -1)
    {
        ret = texis_getCountInfo(all_handles[*idx]->tx, countInfo);
    }
    else
        ret=0;

    if (forkwrite(&ret, sizeof(int))  == -1)
        return 0;

    return ret;
}

static int fork_skip(DB_HANDLE *h, int nrows)
{
    SFI *finfo = check_fork(h, NoCreate);
    int ret=0;

    if(!finfo)
        return 0;

    if(forkwrite("s", sizeof(char)) == -1)
        return 0;

    if(forkwrite(&(h->fork_idx), sizeof(int)) == -1)
        return 0;

    if(forkwrite(&nrows, sizeof(int)) == -1)
        return 0;

    if(forkread(&ret, sizeof(int)) == -1)
        return 0;

    return ret;
}

static int child_skip(SFI *finfo, int *idx)
{
    int ret=0, nrows=0;

    if (forkread(idx, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    if (forkread(&nrows, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    if(*idx != -1)
    {
        ret = texis_flush_scroll(all_handles[*idx]->tx, nrows);
    }
    else
        ret=0;

    if (forkwrite(&ret, sizeof(int))  == -1)
        return 0;

    return ret;
}

static int fork_resetparams(DB_HANDLE *h)
{
    SFI *finfo = check_fork(h, NoCreate);
    int ret=1;

    if(!finfo)
        return 0;

    if(forkwrite("r", sizeof(char)) == -1)
        return 0;

    if(forkwrite(&(h->fork_idx), sizeof(int)) == -1)
        return 0;

    if(forkread(&ret, sizeof(int)) == -1)
        return 0;

    return ret;
}

static int child_resetparams(SFI *finfo, int *idx)
{
    int ret=0;

    if (forkread(idx, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    if(*idx != -1)
    {
        ret=texis_resetparams(all_handles[*idx]->tx);
    }
    else
        ret=0;

    if (forkwrite(&ret, sizeof(int))  == -1)
        return 0;

    return ret;
}

static int fork_set(duk_context *ctx, DB_HANDLE *h, char *errbuf)
{
    SFI *finfo = check_fork(h, Create);
    duk_size_t sz;
    int size;
    void *p;

    int ret=0;

    if(!finfo)
        return 0;

    duk_cbor_encode(ctx, -1, 0);
    p=duk_get_buffer_data(ctx, -1, &sz);
    memcpy(finfo->mapinfo->mem, p, (size_t)sz);

    if(forkwrite("S", sizeof(char)) == -1)
        return 0;

    if(forkwrite(&(h->fork_idx), sizeof(int)) == -1)
        return 0;

    size = (int)sz;
    if(forkwrite(&size, sizeof(int)) == -1)
        return 0;

    if(forkread(&ret, sizeof(int)) == -1)
        return 0;

    if(ret > 0)
    {
        if(forkread(&size, sizeof(int)) == -1)
            return 0;

        duk_push_external_buffer(ctx);
        duk_config_buffer(ctx, -1, finfo->mapinfo->mem, (duk_size_t)size);
        duk_cbor_decode(ctx, -1, 0);
    }
    else if (ret < 0)
    {
        strncpy(errbuf, finfo->mapinfo->mem, 1023);
    }

    return ret;
}

static int child_set(SFI *finfo, int *idx)
{
    int ret=0, bufsz=0;
    duk_context *ctx;
    int size=-1;
    int thrno = get_thread_num();
    RPTHR *thr=rpthread[thrno];

    ctx=thr->ctx;

    if (forkread(idx, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    if (forkread(&bufsz, sizeof(int))  == -1)
    {
        forkwrite(&ret, sizeof(int));
        return 0;
    }

    if(*idx != -1)
    {
        char errbuf[1024];
        void *p;

        errbuf[0]='\0';

        /* get js object needed for sql_set to do its stuff */
        duk_push_external_buffer(ctx);
        duk_config_buffer(ctx, -1, finfo->mapinfo->mem, (duk_size_t)bufsz);
        duk_cbor_decode(ctx, -1, 0);

        /* do the set in this child */
        ret = sql_set(ctx, all_handles[*idx], errbuf);

        ((char*)finfo->mapinfo->mem)[0] = '\0';//abundance of caution.

        /* this is only filled if ret < 0
           and is error text to be sent back to JS  */
        if( ret < 0 )
            memcpy(finfo->mapinfo->mem, errbuf, 1024);

        /*there's some JS data in an object that needs to be sent back */
        else if (ret > 0 )
        {
            duk_size_t sz;

            duk_cbor_encode(ctx, -1, 0);
            p=duk_get_buffer_data(ctx, -1, &sz);
            memcpy(finfo->mapinfo->mem, p, (size_t)sz);
            size = (int)sz;
        }
        /* else == 0 -- all is ok, just no data to send back */
    }
    else
        ret=-1;

    if (forkwrite(&ret, sizeof(int))  == -1)
        return 0;

    if(size > -1)
    {
        if (forkwrite(&size, sizeof(int))  == -1)
            return 0;
    }

    return 1; // close will always happen
}

/* in child process, loop and await commands */
static void do_fork_loop(SFI *finfo)
{
    while(1)
    {
        int idx_d=-1, *idx = &idx_d;
        char command='\0';
        int ret = kill(parent_pid,0);

        if( ret )
            exit(0);

        ret = forkread(&command, sizeof(char));
        if (ret == 0)
        {
            /* a read of 0 size might mean the parent exited,
               otherwise this shouldn't happen                 */
            usleep(10000);
            continue;
        }
        /* this is in fork read now
        else if (ret == -1)
            exit(0);
        */

        clearmsgbuf(); // reset the position of the errmap buffer.

        switch(command)
        {
            case 'o':
                ret = child_open(finfo, idx);
                break;
            case 'c':
                ret = child_close(finfo, idx);
                break;
            case 'p':
                ret = child_prep(finfo, idx);
                break;
            case 'e':
                ret = child_exec(finfo, idx);
                break;
            case 'f':
                ret = 1;
                child_fetch(finfo, idx);
                break;
            case 'r':
                ret = child_resetparams(finfo, idx);
                break;
            case 'P':
                ret = child_param(finfo, idx);
                break;
            case 'F':
                ret = child_flush(finfo, idx);
                break;
            case 's':
                ret = child_skip(finfo, idx);
                break;
            case 'g':
                ret = child_getCountInfo(finfo, idx);
                break;
            case 'S':
                ret = child_set(finfo, idx);
                break;
        }
        /* there will be a call to h_close in parent as well
           but since something went wrong somewhere, we want
           to make sure any errors don't result in
           a pileup of unreleased handles */
        if(!ret && *idx > -1)
            release_handle(all_handles[*idx]);
    }
}

static int h_flush(DB_HANDLE *h)
{
    if(h->needs_fork)
        return fork_flush(h);
    return TEXIS_FLUSH(h->tx);
}

static int h_getCountInfo(DB_HANDLE *h, TXCOUNTINFO *countInfo)
{
    if(h->needs_fork)
        return fork_getCountInfo(h, countInfo);
    return TEXIS_GETCOUNTINFO(h->tx, countInfo);
}

static int h_resetparams(DB_HANDLE *h)
{
    if(h->needs_fork)
        return fork_resetparams(h);
    return TEXIS_RESETPARAMS(h->tx);
}

static int h_param(DB_HANDLE *h, int pn, void *d, long *dl, int t, int st)
{
    if(h->needs_fork)
        return fork_param(h, pn, d, dl, t, st);
    return TEXIS_PARAM(h->tx, pn, d, dl, t, st);
}

static int h_skip(DB_HANDLE *h, int n)
{
    if(h->needs_fork)
        return fork_skip(h,n);
    return TEXIS_SKIP(h->tx, n);
}

static int h_prep(DB_HANDLE *h, char *sql)
{
    if(h->needs_fork)
        return fork_prep(h, sql);
    return TEXIS_PREP(h->tx, sql);
}

static int h_close(DB_HANDLE *h)
{
    if(!h) {
        return 1;
    }

    if(h->needs_fork)
        return fork_close(h);

    release_handle(h);
    return 1;
}

/* TODO: check for this
   Error: sql exec error: 005 Corrupt block header at 0x0 in KDBF file ./docdb/SYSTABLES.tbl in the function: read_head
   and if so, try redoing all the texis handles to see if the table was dropped and readded.
*/

static int h_exec(DB_HANDLE *h)
{
    if(h->needs_fork)
        return fork_exec(h);
    return TEXIS_EXEC(h->tx);
}

static FLDLST *h_fetch(DB_HANDLE *h,  int stringsFrom)
{
    if(h->needs_fork)
        return fork_fetch(h, stringsFrom);
    return TEXIS_FETCH(h->tx, stringsFrom);
}


/* **************************************************
   Sql.prototype.close
   ************************************************** */
duk_ret_t duk_rp_sql_close(duk_context *ctx)
{
    SET_THREAD_UNSAFE(ctx);
    free_all_handles(NULL);
    return 0;
}



#include "db_misc.c" /* copied and altered thunderstone code for stringformat and abstract */

/* **************************************************
    initialize query struct
   ************************************************** */
void duk_rp_init_qstruct(QUERY_STRUCT *q)
{
    q->sql = (char *)NULL;
    q->arr_idx = -1;
    q->str_idx = -1;
    q->obj_idx=-1;
    q->arg_idx=-1;
    q->callback = -1;
    q->skip = 0;
    q->max = -432100000; //-1 means unlimit, -0.4321 billion means not set.
    q->rettype = -1;
    q->getCounts = 0;
    q->err = QS_SUCCESS;
}

/* **************************************************
   get up to 4 parameters in any order.
   object=settings, string=sql,
   array=params to sql, function=callback
   example:
   sql.exec(
     "select * from SYSTABLES where NAME=?",
     ["mytable"],
     {max:1,skip:0,returnType:"array:},
     function (row) {console.log(row);}
   );
   ************************************************** */
/* TODO: leave stack as you found it */

QUERY_STRUCT duk_rp_get_query(duk_context *ctx)
{
    duk_idx_t i = 0;
    int gotsettings=0, maxset=0, selectmax=-432100000;
    QUERY_STRUCT q_st;
    QUERY_STRUCT *q = &q_st;

    duk_rp_init_qstruct(q);

    for (i = 0; i < 6; i++)
    {
        int vtype = duk_get_type(ctx, i);
        switch (vtype)
        {
            case DUK_TYPE_NUMBER:
            {
                if(maxset==1)
                    q->skip=duk_get_int(ctx, i);
                else if(!maxset)
                    q->max=duk_get_int(ctx, i);
                else
                    RP_THROW(ctx, "too many Numbers in parameters to sql.exec()");

                maxset++;
                break;
            }
            case DUK_TYPE_STRING:
            {
                int l;
                if (q->sql != (char *)NULL)
                {
                    RP_THROW(ctx, "Only one string may be passed as a parameter and must be a sql statement.\n");
                    //duk_push_int(ctx, -1);
                    //q->sql = (char *)NULL;
                    //q->err = QS_ERROR_PARAM;
                    //return (q_st);
                }
                q->sql = duk_get_string(ctx, i);
                q->str_idx=i;
                l = strlen(q->sql) - 1;
                while (*(q->sql + l) == ' ' && l > 0)
                    l--;
                if (*(q->sql + l) != ';')
                {
                    duk_dup(ctx, i);
                    duk_push_string(ctx, ";");
                    duk_concat(ctx, 2);
                    duk_replace(ctx, i);
                    q->sql = (char *)duk_get_string(ctx, i);
                }
                /* it hasn't been set yet. we don't want to overwrite returnRows or returnType */
                if(q->rettype == -1)
                {
                    if(strncasecmp(q->sql, "select", 6))
                        q->rettype=2;
                    else
                        q->rettype=0;
                }

                /* selectMaxRows from this */
                if(!strncasecmp(q->sql, "select", 6))
                {
                    duk_push_this(ctx);
                    duk_get_prop_string(ctx, -1, "selectMaxRows");
                    selectmax=duk_get_int_default(ctx, -1, RESMAX_DEFAULT);
                    duk_pop_2(ctx);
                }
                break;
            }
            case DUK_TYPE_OBJECT:
            {
                /* array of parameters*/

                if (duk_is_array(ctx, i) && q->arr_idx == -1)
                    q->arr_idx = i;

                /* argument is a function, save where it is on the stack */
                else if (duk_is_function(ctx, i))
                {
                    q->callback = i;
                }

                /* object of settings or parameters*/
                else
                {

                    /* the first object with these properties is our settings object */
                    if(!gotsettings)
                    {
                        if (duk_get_prop_string(ctx, i, "includeCounts"))
                        {
                            q->getCounts = REQUIRE_BOOL(ctx, -1, "sql: includeCounts must be a Boolean");
                            gotsettings=1;
                        }
                        duk_pop(ctx);

                        if (duk_get_prop_string(ctx, i, "argument"))
                        {
                            q->arg_idx = duk_get_top_index(ctx);
                            gotsettings=1;
                        }
                        /* leave it on the stack for use in callback */
                        else
                        {
                            duk_pop(ctx);
                            /* alternative */
                            if (duk_get_prop_string(ctx, i, "arg"))
                            {
                                q->arg_idx = duk_get_top_index(ctx);
                                gotsettings=1;
                            }
                            /* leave it on the stack for use in callback */
                            else
                                duk_pop(ctx);
                        }

                        if (duk_get_prop_string(ctx, i, "skipRows"))
                        {
                            q->skip = REQUIRE_INT(ctx, -1, "skipRows must be a Number");
                            gotsettings=1;
                        }
                        duk_pop(ctx);

                        if (duk_get_prop_string(ctx, i, "maxRows"))
                        {
                            q->max = REQUIRE_INT(ctx, -1, "sql: maxRows must be a Number");
                            gotsettings=1;
                        }
                        duk_pop(ctx);

                        if (duk_get_prop_string(ctx, i, "returnRows"))
                        {
                            if (REQUIRE_BOOL(ctx, -1, "sql: returnRows must be a Boolean"))
                                q->rettype = 0;
                            else
                                q->rettype = 2;
                            gotsettings=1;
                        }
                        duk_pop(ctx);

                        if (duk_get_prop_string(ctx, i, "returnType"))
                        {
                            const char *rt = REQUIRE_STRING(ctx, -1, "sql: returnType must be a String");

                            if (!strcasecmp("array", rt))
                            {
                                q->rettype = 1;
                            }
                            else if (!strcasecmp("novars", rt))
                            {
                                q->rettype = 2;
                            }
                            else if (!strcasecmp("object", rt))
                                q->rettype=0;
                            else
                                RP_THROW(ctx, "sql: returnType '%s' is not valid", rt);
                            gotsettings=1;
                        }
                        duk_pop(ctx);

                        if(gotsettings)
                            break;
                    }

                    if ( q->arr_idx == -1 && q->obj_idx == -1)
                    {
                        q->obj_idx = i;
                        break;
                    }
                }
                break;
            } /* case */
        } /* switch */
    }     /* for */

    /* if qmax is not set and we are in a select, set to this.selectMaxRows, or RESMAX_DEFAULT */
    if( q->max == -432100000 && selectmax != -432100000)
        q->max = selectmax;

    if (q->max < 0)
        q->max = INT64_MAX;

    if (q->skip < 0)
        q->skip = 0;

    if (q->sql == (char *)NULL)
    {
        //q->err = QS_ERROR_PARAM;
        RP_THROW(ctx, "sql - No sql statement present.\n");
    }
    return (q_st);
}

#define push_sql_param do{\
    switch (duk_get_type(ctx, -1))\
    {\
        case DUK_TYPE_NUMBER:\
        {\
            double floord;\
            d = duk_get_number(ctx, -1);\
            floord = floor(d);\
            if( (d - floord) > 0.0 || (d - floord) < 0.0 || \
                floord < (double)INT64_MIN || floord > (double)INT64_MAX)\
            {\
                v = (double *)&d;\
                plen = sizeof(double);\
                in = SQL_C_DOUBLE;\
                out = SQL_DOUBLE;\
            }\
            else\
            {\
                lval = (int64_t) floord;\
                v = (int64_t *)&lval;\
                plen = sizeof(int64_t);\
                in = SQL_C_SBIGINT;\
                out = SQL_BIGINT;\
            }\
            break;\
        }\
        /* all objects are converted to json string\
           this works (or will work) for several datatypes (varchar,int(x),strlst,json varchar) */\
        case DUK_TYPE_OBJECT:\
        {\
            char *e;\
            char *s = v = (char *)duk_json_encode(ctx, -1);\
            plen = strlen(v);\
            e = s + plen - 1;\
            /* a date (and presumably other single values returned from an object which returns a string)\
             will end up in quotes upon conversion, we need to remove them */\
            if (*s == '"' && *e == '"' && plen > 1)\
            {\
                /* duk functions return const char* */\
                v = s + 1;\
                plen -= 2;\
            }\
            in = SQL_C_CHAR;\
            out = SQL_VARCHAR;\
            break;\
        }\
        /* insert binary data from a buffer */\
        case DUK_TYPE_BUFFER:\
        {\
            duk_size_t sz;\
            v = duk_get_buffer_data(ctx, -1, &sz);\
            plen = (long)sz;\
            in = SQL_C_BINARY;\
            out = SQL_BINARY;\
            break;\
        }\
        /* default for strings (not converted)and\
           booleans, null and undefined (converted to \
           true/false, "null" and "undefined" respectively */\
        default:\
        {\
            v = (char *)duk_to_string(ctx, -1);\
            plen = strlen(v);\
            in = SQL_C_CHAR;\
            out = SQL_VARCHAR;\
        }\
    }\
} while(0)


int duk_rp_add_named_parameters(
    duk_context *ctx,
    DB_HANDLE *h,
    duk_idx_t obj_loc,
    char **namedSqlParams,
    int nParams
)
{
    int rc=0, i=0;

    for(i=0;i<nParams;i++)
    {
        char *key = namedSqlParams[i];
        void *v;   /* value to be passed to db */
        long plen; /* lenght of value */
        double d;  /* for numbers */
        int64_t lval;
        int in, out;

        duk_get_prop_string(ctx, obj_loc, key);
        if(!duk_is_undefined(ctx, -1))
        {
            push_sql_param;

            /* texis_params is indexed starting at 1 */
            rc = h_param(h, i+1, v, &plen, in, out);
            if (!rc)
                return 0;
        }
        duk_pop(ctx);

    }
    return 1;
}

/* **************************************************
   Push parameters to the database for parameter
   substitution (?) is sql.
   i.e. "select * from tbname where col1 = ? and col2 = ?"
     an array of two values should be passed and will be
     processed here.
     arrayi is the place on the stack where the array lives.
   ************************************************** */

int duk_rp_add_parameters(duk_context *ctx, DB_HANDLE *h, duk_idx_t arr_loc)
{
    int rc=0;
    duk_uarridx_t arr_i = 0;

    /* Array is at arr_loc. Iterate over members.
       arr_i is the index of the array member we are examining */
    while (duk_has_prop_index(ctx, arr_loc, arr_i))
    {
        void *v;   /* value to be passed to db */
        long plen; /* lenght of value */
        double d;  /* for numbers */
        int64_t lval;
        int in, out;

        /* push array member to top of stack */
        duk_get_prop_index(ctx, arr_loc, arr_i);

        /* check the datatype of the array member */
        push_sql_param;
        arr_i++;
        rc = h_param(h, (int)arr_i, v, &plen, in, out);
        duk_pop(ctx);
        if (!rc)
            return 0;
    }
    return 1;
}

#define pushcounts do{\
    duk_push_object(ctx);\
    duk_push_number(ctx,(double)cinfo.indexCount );\
    duk_put_prop_string(ctx,-2,"indexCount");\
    duk_push_number(ctx,(double)cinfo.rowsMatchedMin );\
    duk_put_prop_string(ctx,-2,"rowsMatchedMin");\
    duk_push_number(ctx,(double)cinfo.rowsMatchedMax );\
    duk_put_prop_string(ctx,-2,"rowsMatchedMax");\
    duk_push_number(ctx,(double)cinfo.rowsReturnedMin );\
    duk_put_prop_string(ctx,-2,"rowsReturnedMin");\
    duk_push_number(ctx,(double)cinfo.rowsReturnedMax );\
    duk_put_prop_string(ctx,-2,"rowsReturnedMax");\
}while(0);


/* **************************************************
  push a single field from a row of the sql results
   ************************************************** */
void duk_rp_pushfield(duk_context *ctx, FLDLST *fl, int i)
{
    char type = fl->type[i] & 0x3f;

    if( !fl->data[i]  || !fl->ndata[i])
    {
        duk_push_null(ctx);
        return;
    }

    switch (type)
    {
    case FTN_CHAR:
    case FTN_INDIRECT:
    {
        duk_size_t  sz = (duk_size_t) strlen(fl->data[i]);

        if(sz > fl->ndata[i])
            sz = fl->ndata[i];
        duk_push_lstring(ctx, (char *)fl->data[i], sz );
        break;
    }
    case FTN_STRLST:
    {
        ft_strlst *p = (ft_strlst *)fl->data[i];
        char *s = p->buf;
        size_t l = strlen(s);
        char *end = s + (p->nb);
        int j = 0;

        duk_push_array(ctx);
        while (s < end)
        {
            duk_push_string(ctx, s);
            duk_put_prop_index(ctx, -2, j++);
            s += l;
            while (s < end && *s == '\0')
                s++;
            l = strlen(s);
        }
        break;
    }
    case FTN_INT:
    {
        duk_push_int(ctx, (duk_int_t) * ((ft_int *)fl->data[i]));
        break;
    }
    /*        push_number with (duk_t_double) cast,
              53bits of double is the best you can
              do for exact whole numbers in javascript anyway */
    case FTN_INT64:
    {
        duk_push_number(ctx, (duk_double_t) * ((ft_int64 *)fl->data[i]));
        break;
    }
    case FTN_UINT64:
    {
        duk_push_number(ctx, (duk_double_t) * ((ft_uint64 *)fl->data[i]));
        break;
    }
    case FTN_INTEGER:
    {
        duk_push_number(ctx, (duk_double_t) * ((ft_integer *)fl->data[i]));
        break;
    }
    case FTN_LONG:
    {
        duk_push_number(ctx, (duk_double_t) * ((ft_long *)fl->data[i]));
        break;
    }
    case FTN_SMALLINT:
    {
        duk_push_number(ctx, (duk_double_t) * ((ft_smallint *)fl->data[i]));
        break;
    }
    case FTN_SHORT:
    {
        duk_push_number(ctx, (duk_double_t) * ((ft_short *)fl->data[i]));
        break;
    }
    case FTN_DWORD:
    {
        duk_push_number(ctx, (duk_double_t) * ((ft_dword *)fl->data[i]));
        break;
    }
    case FTN_WORD:
    {
        duk_push_number(ctx, (duk_double_t) * ((ft_word *)fl->data[i]));
        break;
    }
    case FTN_DOUBLE:
    {
        duk_push_number(ctx, (duk_double_t) * ((ft_double *)fl->data[i]));
        break;
    }
    case FTN_FLOAT:
    {
        duk_push_number(ctx, (duk_double_t) * ((ft_float *)fl->data[i]));
        break;
    }
    case FTN_DATE:
    {
        /* equiv to js "new Date(seconds*1000)" */
        (void)duk_get_global_string(ctx, "Date");
        duk_push_number(ctx, 1000.0 * (duk_double_t) * ((ft_date *)fl->data[i]));
        duk_new(ctx, 1);
        break;
    }
    case FTN_COUNTER:
    {
        char s[33];
        //void *v=NULL;
        ft_counter *acounter = fl->data[i];

        //duk_push_object(ctx);
        snprintf(s, 33, "%lx%lx", acounter->date, acounter->seq);
        duk_push_string(ctx, s);
        /*
        duk_put_prop_string(ctx, -2, "counterString");

        (void)duk_get_global_string(ctx, "Date");
        duk_push_number(ctx, 1000.0 * (duk_double_t) acounter->date);
        duk_new(ctx, 1);
        duk_put_prop_string(ctx, -2, "counterDate");

        duk_push_number(ctx, (duk_double_t) acounter->seq);
        duk_put_prop_string(ctx, -2, "counterSequence");

        */

        break;
    }
    case FTN_BYTE:
    {
        unsigned char *p;

        /* create backing buffer and copy data into it */
        p = (unsigned char *)duk_push_fixed_buffer(ctx, fl->ndata[i] /*size*/);
        memcpy(p, fl->data[i], fl->ndata[i]);
        break;
    }
    default:
        duk_push_int(ctx, (int)type);
    }
}

/* **************************************************
   This is called when sql.exec() has no callback.
   fetch rows and push results to array
   return number of rows
   ************************************************** */
int duk_rp_fetch(duk_context *ctx, DB_HANDLE *h, QUERY_STRUCT *q)
{
    int i       = 0,
        rettype = q->rettype;
    uint64_t rown   = 0,
             resmax = q->max;
    FLDLST *fl;
    TXCOUNTINFO cinfo;

    if(q->getCounts)
        h_getCountInfo(h, &cinfo);

    /* create return object */
    duk_push_object(ctx);

    /* create results array (outer array if rettype>0) */
    duk_push_array(ctx);

    /* array of arrays or novars requested */
    if (rettype)
    {

        /* still fill columns if rexmax == 0 */
        /* WTF: return columns if table is empty */
        if (resmax < 1)
        {
            if((fl = h_fetch(h, -1)))
            {
                /* an array of column names */
                duk_push_array(ctx);
                for (i = 0; i < fl->n; i++)
                {
                    duk_push_string(ctx, fl->name[i]);
                    duk_put_prop_index(ctx, -2, i);
                }
                duk_put_prop_string(ctx, -3, "columns");
            }

        } else
        /* push values into subarrays and add to outer array */
        while (rown < resmax && (fl = h_fetch(h, -1)))
        {
            /* novars, we need to get each row (for del and return count value) but not return any vars */
            if (rettype == 2)
            {
                rown++;
                continue;
            }
            /* we want first row to be column names */
            if (!rown)
            {
                /* an array of column names */
                duk_push_array(ctx);
                for (i = 0; i < fl->n; i++)
                {
                    duk_push_string(ctx, fl->name[i]);
                    duk_put_prop_index(ctx, -2, i);
                }
                duk_put_prop_string(ctx, -3, "columns");
            }

            /* push values into array */
            duk_push_array(ctx);
            for (i = 0; i < fl->n; i++)
            {
                duk_rp_pushfield(ctx, fl, i);
                duk_put_prop_index(ctx, -2, i);
            }
            duk_put_prop_index(ctx, -2, rown++);

        } /* while */
    }
    else
    /* array of objects requested
     push object of {name:value,name:value,...} into return array */
    {
        while (rown < resmax && (fl = h_fetch(h, -1)))
        {
            if (!rown)
            {
                /* an array of column names */
                duk_push_array(ctx);
                for (i = 0; i < fl->n; i++)
                {
                    duk_push_string(ctx, fl->name[i]);
                    duk_put_prop_index(ctx, -2, i);
                }
                duk_put_prop_string(ctx, -3, "columns");
            }

            duk_push_object(ctx);
            for (i = 0; i < fl->n; i++)
            {
                duk_rp_pushfield(ctx, fl, i);
                //duk_dup_top(ctx);
                //printf("%s -> %s\n",fl->name[i],duk_to_string(ctx,-1));
                //duk_pop(ctx);
                duk_put_prop_string(ctx, -2, (const char *)fl->name[i]);
            }
            duk_put_prop_index(ctx, -2, rown++);
        }
    }
    /* added "rows", "results" to be removed
    duk_dup(ctx, -1);
    duk_put_prop_string(ctx,-3,"results");
    */
    duk_put_prop_string(ctx,-2,"rows");
    if(q->getCounts)
    {
        pushcounts;
        duk_put_prop_string(ctx,-2,"countInfo");
    }
    duk_push_int(ctx, rown);
    duk_put_prop_string(ctx,-2,"rowCount");

    return (rown);
}

/* **************************************************
   This is called when sql.exec() has a callback function
   Fetch rows and execute JS callback function with
   results.
   Return number of rows
   ************************************************** */
int duk_rp_fetchWCallback(duk_context *ctx, DB_HANDLE *h, QUERY_STRUCT *q)
{
    int i       = 0,
        rettype = q->rettype;
    uint64_t rown   = 0,
             resmax = q->max;
    duk_idx_t callback_idx = q->callback,
              colnames_idx = 0,
              count_idx    =-1;
    FLDLST *fl;
    TXCOUNTINFO cinfo;

    if(q->getCounts)
    {
        h_getCountInfo(h, &cinfo);
        pushcounts;             /* countInfo */
    }
    else
    {
        duk_push_object(ctx);
    }
    count_idx=duk_get_top_index(ctx);

#define docallback do {\
    duk_dup(ctx, count_idx);\
    if(q->arg_idx > -1){\
        duk_dup(ctx, q->arg_idx);\
        duk_call_method(ctx, 5);\
    } else duk_call_method(ctx, 4);\
} while(0)

    while (rown < resmax && (fl = h_fetch(h, -1)))
    {

        if (!rown)
        {
            /* an array of column names */
            duk_push_array(ctx);
            for (i = 0; i < fl->n; i++)
            {
                duk_push_string(ctx, fl->name[i]);
                duk_put_prop_index(ctx, -2, i);
            }
            colnames_idx=duk_get_top_index(ctx);
        }

        duk_dup(ctx, callback_idx); /* the function */
        duk_push_this(ctx);         /* calling with this */

        switch (rettype)
        {
            /* object requested */
            case 0:
            {
                duk_push_object(ctx);
                for (i = 0; i < fl->n; i++)
                {
                    duk_rp_pushfield(ctx, fl, i);
                    duk_put_prop_string(ctx, -2, (const char *)fl->name[i]);
                }
                duk_push_int(ctx, q->skip + rown++ );
                duk_dup(ctx, colnames_idx);
                docallback;
                break;
            }
            /* array */
            case 1:
            {
                duk_push_array(ctx);
                for (i = 0; i < fl->n; i++)
                {
                    duk_rp_pushfield(ctx, fl, i);
                    duk_put_prop_index(ctx, -2, i);
                }

                duk_push_int(ctx, q->skip + rown++ );
                duk_dup(ctx, colnames_idx);
                docallback;
                break;
            }
            /* novars */
            case 2:
            {
                duk_push_object(ctx);       /*empty object */
                duk_push_int(ctx, q->skip + rown++ ); /* index */
                duk_dup(ctx, colnames_idx);
                docallback;
                break;
            }
        } /* switch */
        /* if function returns false, exit while loop, return number of rows so far */
        if (duk_is_boolean(ctx, -1) && !duk_get_boolean(ctx, -1))
        {
            duk_pop(ctx);
            return (rown);
        }
        /* get rid of ret value from callback*/
        duk_pop(ctx);
    } /* while fetch */

    return (rown);
}

void reset_tx_default(duk_context *ctx, DB_HANDLE *h, duk_idx_t this_idx);

#undef pushcounts

/* **************************************************
   Sql.prototype.import
   ************************************************** */
duk_ret_t duk_rp_sql_import(duk_context *ctx, int isfile)
{
    /* currently only csv
       but eventually add option for others
       by checking options object for
       "type":"filetype" - with default "csv"
    */
    const char *func_name = isfile?"sql.importCsvFile":"sql.importCsv";
    DCSV dcsv=duk_rp_parse_csv(ctx, isfile, 1, func_name);
    int ncols=dcsv.csv->cols, i=0;
    int tbcols=0, start=0;
    DB_HANDLE *h = NULL;
    TEXIS *tx=NULL;
    char **field_names=NULL;
    uint8_t *field_type=NULL;
    int field_type_size=0;
    duk_idx_t this_idx;

#define closecsv do {\
    int col;\
    for(col=0;col<dcsv.csv->cols;col++) \
        free(dcsv.hnames[col]); \
    free(dcsv.hnames); \
    closeCSV(dcsv.csv); \
} while(0)


    if(strlen(dcsv.tbname)<1)
        RP_THROW(ctx, "%s(): option tableName is required", func_name);

    const char *db;
    char pbuf[msgbufsz];
    struct sigaction sa = { {0} };

    sa.sa_flags = 0; //SA_NODEFER;
    sa.sa_handler = die_nicely;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);

    //  signal(SIGUSR2, die_nicely);

    SET_THREAD_UNSAFE(ctx);

    duk_push_this(ctx);
    this_idx = duk_get_top_index(ctx);

    /* clear the sql.errMsg string */
    duk_del_prop_string(ctx,-1,"errMsg");

    if (!duk_get_prop_string(ctx, -1, "db"))
    {
        closecsv;
        RP_THROW(ctx, "no database has been opened");
    }
    db = duk_get_string(ctx, -1);

    duk_pop(ctx);

    h = h_open(db, fromContext);
    if(!h)
        throw_tx_error(ctx,h,"sql open");

    reset_tx_default(ctx, h, this_idx);
    duk_pop(ctx);//this

    tx = h->tx;
    if (!tx)
    {
        h_close(h);
        closecsv;
        throw_tx_error(ctx,h,"open sql");
    }

    {
        FLDLST *fl;
        char sql[256];

        snprintf(sql, 256, "select NAME, TYPE from SYSCOLUMNS where TBNAME='%s' order by ORDINAL_POSITION;", dcsv.tbname);

        if (!h_prep(h, sql))
        {
            closecsv;
            h_close(h);
            throw_tx_error(ctx,h,"sql prep");
        }

        if (!h_exec(h))
        {
            closecsv;
            h_close(h);
            throw_tx_error(ctx,h,"sql exec");
        }

        while((fl = h_fetch(h, -1)))
        {
            /* an array of column names */
            REMALLOC(field_names, (tbcols+2) * sizeof(char*) );

            /* a uint8_t array of column types */
            if (tbcols + 1 > field_type_size)
            {
                field_type_size +=256;
                REMALLOC(field_type, field_type_size * sizeof(uint8_t));
            }

            field_names[tbcols] = strdup(fl->data[0]);

            /* keep track of which fields are counter */
            if(!strncmp(fl->data[1],"counter",7))
                field_type[tbcols] = 1;
            else
                field_type[tbcols] = 0;

            tbcols++;
        }
        /* table doesn't exist? */
        if(!tbcols)
        {
            closecsv;
            h_close(h);
            RP_THROW(ctx, "Table '%s' does not exist.  Table must exist before importing CSV", dcsv.tbname);
        }

        field_names[tbcols] = NULL;

    }

#define fn_cleanup do { \
    int j=0; \
    if(tbcols){ \
      while(field_names[j]!=NULL) \
          free(field_names[j++]); \
    }\
    if(field_names)free(field_names); \
    if(field_type)free(field_type); \
    h_close(h);\
    closecsv; \
} while(0);

    {
        /* ncols = number of columns in the csv
           tbcols = number of columns in the table   */
        int col_order[tbcols]; /* one per table column and value is 0 -> ncols-1 */

        if(dcsv.arr_idx>-1)
        {
            duk_idx_t idx=dcsv.arr_idx;
            int aval=0, len= (int)duk_get_length(ctx, idx);
            char **hn=dcsv.hnames;

            for(i=0;i<len;i++)
            {
                duk_get_prop_index(ctx, idx, (duk_uarridx_t)i);
                if( duk_is_string(ctx, -1))
                {
                    int j=0;
                    const char *s=duk_get_string(ctx, -1);
                    if (strlen(s)==0)
                        aval=-1;
                    else
                    {
                        while (hn[j]!=NULL)
                        {
                            if(strcmp(hn[j],s)==0)
                            {
                                aval=j;
                                break;
                            }
                            j++;
                        }
                        if (hn[j]==NULL)
                        {
                            fn_cleanup;
                            RP_THROW(ctx, "%s(): array contains '%s', which is not a known column name", func_name,s);
                        }
                    }
                }
                else if(! duk_is_number(ctx, -1))
                {
                    fn_cleanup;
                    RP_THROW(ctx, "%s(): array requires an array of Integers/Strings (column numbers/names)", func_name);
                }
                else
                    aval=duk_get_int(ctx, -1);

                duk_pop(ctx);
                if( aval>=ncols )
                {
                    fn_cleanup;
                    RP_THROW(ctx, "%s(): array contains column number %d. There are %d columns in the csv (numbered 0-%d)",
                        func_name, aval, ncols, ncols-1);
                }
                col_order[i]=aval;
                //printf("order[%d]=%d\n",i,aval);
            }
            /* fill rest, if any, with -1 */
            for (i=len; i<tbcols; i++)
                col_order[i]=-1;
        }
        else
        {
            /* insert order is col order */
            for(i=0;i<tbcols;i++)
                col_order[i]=i;
        }

        {
            int slen = 24 + strlen(dcsv.tbname) + (2*tbcols) -1;
            char sql[slen];
            CSV *csv = dcsv.csv;
            void *v=NULL;   /* value to be passed to db */
            long plen, datelong;
            int in=0, out=0, row=0, col=0, intzero=0;
            DDIC *ddic=NULL;
            ft_counter *ctr = NULL;
            LPSTMT lpstmt;

            lpstmt = tx->hstmt;
            if(lpstmt && lpstmt->dbc && lpstmt->dbc->ddic)
                ddic = lpstmt->dbc->ddic;
            else
            {
                throw_tx_error(ctx,h,"sql open");;
            }

            snprintf(sql, slen, "insert into %s values (", dcsv.tbname);
            for (i=0;i<tbcols-1;i++)
                strcat(sql,"?,");

            strcat(sql,"?);");

            //printf("%s, %d, %d\n", sql, slen, (int)strlen(sql) );

            if (!h_prep(h, sql))
            {
                fn_cleanup;
                throw_tx_error(ctx,h,"sql prep");
            }

            if(dcsv.hasHeader) start=1;
            for(row=start;row<csv->rows;row++)      // iterate through the CSVITEMS contained in each row and column
            {
                for(  col=0; /* col<csv->cols && */col<tbcols; col++)
                {

                    if(col_order[col]>-1)
                    {
                    //printf("doing col_order[%d] = %d\n", col, col_order[col]);
                        CSVITEM item=csv->item[row][col_order[col]];
                        switch(item.type)
                        {
                            case integer:
                                in=SQL_C_SBIGINT;
                                out=SQL_BIGINT;
                                v=(int64_t*)&item.integer;
                                plen=sizeof(int64_t);
                                break;
                            case floatingPoint:
                                in=SQL_C_DOUBLE;
                                out=SQL_DOUBLE;
                                v=(double *)&item.integer;
                                plen=sizeof(double);
                                break;
                            case string:
                                v = (char *)item.string;
                                plen = strlen(v);
                                in = SQL_C_CHAR;
                                out = SQL_VARCHAR;
                                break;
                            case dateTime:
                            {
                                struct tm *t=&item.dateTime;
                                in=SQL_C_LONG;
                                out=SQL_DATE;
                                datelong=(long) mktime(t);
                                v = (long*)&datelong;
                                plen=sizeof(long);
                                break;
                            }
                            case nil:
                                in=SQL_C_INTEGER;
                                out=SQL_INTEGER;
                                v=(int*)&intzero;
                                plen=sizeof(int);
                                break;
                        }
                    }
                    else
                    {
                        /* insert texis counter if field is a counter type */
                        if (field_type[col]==1)
                        {
                            ctr = getcounter(ddic);
                            v=ctr;
                            plen=sizeof(ft_counter);
                            in=SQL_C_COUNTER;
                            out=SQL_COUNTER;
                        }
                        else
                        {
                            v=&intzero;
                            plen=sizeof(int);
                            in=SQL_C_INTEGER;
                            out=SQL_INTEGER;
                        }
                    }
                    if( !h_param(h, col+1, v, &plen, in, out))
                    {
                        if(ctr) free(ctr);
                        ctr=NULL;
                        fn_cleanup;
                        throw_tx_error(ctx,h,"sql add parameters");
                    }
                    if(ctr) free(ctr);
                    ctr=NULL;
                }
                if (col<tbcols)
                {
                    v=(long*)&intzero;
                    plen=sizeof(int);
                    in=SQL_C_INTEGER;
                    out=SQL_INTEGER;
                    for(; col<tbcols; col++)
                    {
                        if( !h_param(h, col+1, v, &plen, in, out))
                        {
                            fn_cleanup;
                            throw_tx_error(ctx,h,"sql add parameters");
                        }
                    }
                }

                if (!h_exec(h))
                {
                    fn_cleanup;
                    throw_tx_error(ctx,h,"sql exec");
                }
                if (!h_flush(h))
                {
                    fn_cleanup;
                    throw_tx_error(ctx,h,"sql flush");
                }

                h_resetparams(h);

                if (dcsv.func_idx > -1 && !( (row-start) % dcsv.cbstep ) )
                {
                    duk_dup(ctx, dcsv.func_idx);
                    duk_push_int(ctx, row-start);
                    duk_call(ctx, 1);
                    if(duk_is_boolean(ctx, -1) && ! duk_get_boolean(ctx, -1) )
                        goto funcend;
                    duk_pop(ctx);
                }
            }
        }
    }

    funcend:

    duk_push_int(ctx, dcsv.csv->rows - start);
    fn_cleanup;
    duk_rp_log_tx_error(ctx,h,pbuf); /* log any non fatal errors to this.errMsg */
    return 1;
}

duk_ret_t duk_rp_sql_import_csv_file(duk_context *ctx)
{
    return duk_rp_sql_import(ctx, 1);
}

duk_ret_t duk_rp_sql_import_csv_str(duk_context *ctx)
{
    return duk_rp_sql_import(ctx, 0);
}

/*
   Finds the closing char c. Used for finding single and double quotes
   Tries to deal with escapements
   Returns a pointer to the end of string or the matching character
   *pN is stuffed withe the number of characters it skipped
*/
static char * skip_until_c(char *s,int c,int *pN)
{
   *pN=0;                        // init the character counter to 0
   while(*s)
   {
      if(*s=='\\' && *(s+1)==c)  // deal with escapement
      {
         ++s;
         ++*pN;
      }
      else
      if(*s==c)
       return(s);
      ++s;
      ++*pN;
   }
  return(s);
}

// counts the number of ?'s in the sql string
static int count_sql_parameters(char *s)
{
   int n_params=0;
   int unused;
   while(*s)
   {
      switch(*s)
      {
         case '"' :
         case '\'': s=skip_until_c(s+1,*s,&unused);break;
         case '\\': ++s; break;
         case '?' : ++n_params;break;
      }
     ++s;
   }
   return (n_params);
}

/*
   This parses parameter names out of SQL statements.

   Parameter names must be int the form of:
      ?legal_SQL_variable_name   where legal is (\alpha || \digit || _)+
      or
      ?" almost anything "
      or
      ?' almost anything '

   Returns the number of parameters or -1 if there's syntax error involving parameters
   It removes the names from the SQL and places the result in *new_sql
   It places an array of pointers to the paramater names in names[] ( in order found )
   it places a pointer to a buffer it uses for the name space in *free_me.

   Both names[] and free_me must be freed by the caller BUT ONLY IF return is >0

*/
static int parse_sql_parameters(char *old_sql,char **new_sql,char **names[],char **free_me)
{
    int    n_params=count_sql_parameters(old_sql);
    char * my_copy  =NULL;
    char * sql      =NULL;
    char **my_names =NULL;
    char * out_p    =NULL;
    char * s        =NULL;

    int    name_index=0;
    int    quote_len;
    int    inlen = strlen(old_sql)+1;
    int    qm_index=0;
    /* width in chars of largest number + '\0' if all ? are treated as ?0, ?1, etc */
    int    numwidth;

    if(!n_params)                  // nothing to do
       return(0);

    if(n_params < 10)
        numwidth = 2;
    else if (n_params < 100)
        numwidth = 3;
    else
        numwidth = (int)(floor(log10((double)n_params))) + 2;

    /* copy the string, and make room at the end for printing extra numbers */
    REMALLOC(my_copy, inlen + n_params * numwidth +1);
    strcpy(my_copy, old_sql);      // extract the names in place and mangle the sql copy

    *free_me =my_copy;             // tell the caller what to free when they're done
    s        =my_copy;             // we're going to trash our copy with nulls

    //REMALLOC(sql, strlen(old_sql)+1); // the new_sql cant be bigger than the old
    REMALLOC(sql, strlen(old_sql)*2);   // now it can, because of extra
    *new_sql=sql;                 // give the caller the new SQL
    out_p=sql;                    // init the sql output pointer

    REMALLOC(my_names, n_params*sizeof(char *));

    *names=my_names;

    while(*s)
    {
       switch(*s)
       {
          case '"' :
          case '\'':
             {
                char *t=s;
                s=skip_until_c(s+1,*s,&quote_len);
                memcpy(out_p,t,quote_len+1);     // the plus 1 is for the quote character
                out_p+=quote_len+1;
                break;
             }
          case '\\': break;
          case '?' :
             {
                ++s;

                if(!(isalnum(*s) || *s=='_' || *s=='"' || *s=='\'')) // check for legal 1st char
                {
                    my_names[name_index++] = my_copy + inlen;
                    inlen += sprintf( my_copy + inlen, "%d", qm_index++) + 1;
                    *out_p='?';
                    ++out_p;
                    break;
                }

                if(*s=='"' || *s=='\'')          // handle ?"my var"
                {
                   int quote_type=*s;
                   my_names[name_index++]=++s;
                   s=skip_until_c(s,quote_type,&quote_len);
                   if(!*s)           // we hit a null without an ending "
                      goto error_return;
                   *s='\0';          // terminate this variable name
                   *out_p='?';
                   ++out_p;
                   ++s;
                   continue;
                }
                else
                {
                   my_names[name_index++]=s++;
                   while(*s && (isalnum(*s) || *s=='_'))
                      ++s;
                   *out_p='?';
                   ++out_p;
                    *out_p++=*s;
                   if(!*s)           //  terminated at the end of the sql we're done
                      return(n_params);
                   else
                   {
                      *out_p=*s;
                      *s='\0';
                   }
                   ++s;
                 continue;
                }
             } break;
       }

      *out_p++=*s;
      ++s;
    }
   *out_p='\0';
   return(n_params);

   error_return:
   if(my_names)
     free(my_names);
   if(my_copy)
     free(my_copy);
   if(sql)
     free(sql);
   return(-1);
}

/*
void check_parse(char *sql,char *new_sql,char **names,int n_names)
{
   int i;
   printf("IN :%s\nOUT:%s\n%d names\n",sql,new_sql,n_names);
   for(i=0;i<n_names;i++)
      printf("%5d %s\n",i,names[i]);
   printf("\n\n");
}
*/

/* **************************************************
   Sql.prototype.exec
   ************************************************** */
duk_ret_t duk_rp_sql_exec(duk_context *ctx)
{
    TEXIS *tx;
    QUERY_STRUCT *q, q_st;
    DB_HANDLE *hcache = NULL;
    const char *db;
    char pbuf[msgbufsz];
    duk_idx_t this_idx;
    struct sigaction sa = { {0} };

    sa.sa_flags = 0; //SA_NODEFER;
    sa.sa_handler = die_nicely;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);

    int nParams=0;
    char *newSql=NULL, **namedSqlParams=NULL, *freeme=NULL;
    //  signal(SIGUSR2, die_nicely);

    SET_THREAD_UNSAFE(ctx);

    duk_push_this(ctx);
    this_idx = duk_get_top_index(ctx);

    /* clear the sql.errMsg string */
    duk_del_prop_string(ctx,-1,"errMsg");

    if (!duk_get_prop_string(ctx, -1, "db"))
        RP_THROW(ctx, "no database has been opened");

    db = duk_get_string(ctx, -1);
    duk_pop(ctx); //db

    q_st = duk_rp_get_query(ctx);
    q = &q_st;

    /* call parameters error, message is already pushed */
    if (q->err == QS_ERROR_PARAM)
    {
        goto end;
    }

    nParams = parse_sql_parameters((char*)q->sql, &newSql, &namedSqlParams, &freeme);

    //check_parse((char*)q->sql, newSql, namedSqlParams, nParams);
    //return 0;
    if (nParams > 0)
    {
        duk_push_string(ctx, newSql);
        duk_replace(ctx, q->str_idx);
        q->sql = duk_get_string(ctx, q->str_idx);
        free(newSql);
    }
    else
    {
        namedSqlParams=NULL;
        freeme=NULL;
    }

    /* OPEN */
    hcache = h_open(db, fromContext);
    if(!hcache)
        throw_tx_error(ctx,hcache,"sql open");

    reset_tx_default(ctx, hcache, this_idx);

//  messes up the count for arg_idx, so just leave it
//    duk_remove(ctx, this_idx); //no longer needed

    tx = hcache->tx;
    if (!tx && !hcache->needs_fork)
        throw_tx_error(ctx,hcache,"open sql");

    /* PREP */
    if (!h_prep(hcache, (char *)q->sql))
    {
        h_close(hcache);
        throw_tx_error(ctx,hcache,"sql prep");
    }


    /* PARAMS
       sql parameters are the parameters corresponding to "?key" in a sql statement
       and are provide by passing an object in JS call parameters */
    if( namedSqlParams)
    {
        duk_idx_t idx=-1;
        if(q->obj_idx != -1)
            idx=q->obj_idx;
        else if (q->arr_idx != -1)
            idx = q->arr_idx;
        else
        {
            h_close(hcache);
            RP_THROW(ctx, "sql.exec - parameters specified in sql statement, but no corresponding object or array\n");
        }
        if (!duk_rp_add_named_parameters(ctx, hcache, idx, namedSqlParams, nParams))
        {
            free(namedSqlParams);
            free(freeme);
            h_close(hcache);
            throw_tx_error(ctx,hcache,"sql add parameters");
        }
        free(namedSqlParams);
        free(freeme);
    }
    /* sql parameters are the parameters corresponding to "?" in a sql statement
     and are provide by passing array in JS call parameters
     TODO: check that this is indeed dead code given that parse_sql_parameters now
           turns "?, ?" into "?0, ?1"
     */
    else if (q->arr_idx != -1)
    {
        if (!duk_rp_add_parameters(ctx, hcache, q->arr_idx))
        {
            h_close(hcache);
            throw_tx_error(ctx,hcache,"sql add parameters");
        }
    }
    else
    {
        h_resetparams(hcache);
    }
    /* EXEC */

    if (!h_exec(hcache))
    {
        h_close(hcache);
        throw_tx_error(ctx,hcache,"sql exec");
    }
    /* skip rows using texisapi */
    if (q->skip)
        h_skip(hcache, q->skip);

    /* callback - return one row per callback */
    if (q->callback > -1)
    {
        int rows = duk_rp_fetchWCallback(ctx, hcache, q);
        duk_push_int(ctx, rows);
        goto end; /* done with exec() */
    }

    /*  No callback, return all rows in array of objects */
    (void)duk_rp_fetch(ctx, hcache, q);

end:
    h_close(hcache);
    duk_rp_log_tx_error(ctx,hcache,pbuf); /* log any non fatal errors to this.errMsg */
    return 1; /* returning outer array */
}

/* **************************************************
   Sql.prototype.eval
   ************************************************** */
duk_ret_t duk_rp_sql_eval(duk_context *ctx)
{
    char *stmt = (char *)NULL;
    duk_idx_t str_idx = -1;
    duk_idx_t i = 0, top=duk_get_top(ctx);;

    /* find the argument that is a string */
    for (i = 0; i < top; i++)
    {
        if ( duk_is_string(ctx, i) )
        {
            stmt = (char *)duk_get_string(ctx, i);
            str_idx = i;
        }
        else if( duk_is_object(ctx, i) && !duk_is_array(ctx, i) )
        {
            /* remove returnType:'arrayh' as only one row will be returned */
            if(duk_get_prop_string(ctx, i, "returnType"))
            {
                if(! strcmp(duk_get_string(ctx, -1), "arrayh") )
                    duk_del_prop_string(ctx, i, "returnType");
            }
            duk_pop(ctx);
        }
    }

    if (str_idx == -1)
    {
        duk_rp_log_error(ctx, "Error: Eval: No string to evaluate");
        duk_push_int(ctx, -1);
        return (1);
    }

    duk_push_sprintf(ctx, "select %s;", stmt);
    duk_replace(ctx, str_idx);
    duk_rp_sql_exec(ctx);
    duk_get_prop_string(ctx, -1, "rows");
    duk_get_prop_index(ctx, -1, 0);
    return (1);
}

duk_ret_t duk_rp_sql_one(duk_context *ctx)
{
    duk_idx_t str_idx = -1, i = 0, obj_idx = -1;

    for (i = 0; i < 2; i++)
    {
        if ( duk_is_string(ctx, i) )
            str_idx = i;
        else if( duk_is_object(ctx, i) && !duk_is_array(ctx, i) )
            obj_idx=i;
    }

    if (str_idx == -1)
    {
        duk_rp_log_error(ctx, "sql.one: No string (sql statement) provided");
        duk_push_int(ctx, -1);
        return (1);
    }

    duk_push_object(ctx);
    duk_push_number(ctx, 1.0);
    duk_put_prop_string(ctx, -2, "maxRows");
    duk_push_true(ctx);
    duk_put_prop_string(ctx, -2, "returnRows");

    if( obj_idx != -1)
        duk_pull(ctx, obj_idx);

    duk_rp_sql_exec(ctx);
    duk_get_prop_string(ctx, -1, "rows");
    duk_get_prop_index(ctx, -1, 0);
    //if(duk_is_undefined(ctx, -1))
    //    duk_push_object(ctx);
    return (1);
}


static void free_list(char **nl)
{
    int i=0;
    char *f;

    if(nl==NULL)
        return;

    f=nl[i];

    while(1)
    {
        if (*f=='\0')
        {
            free(f);
            break;
        }
        free(f);
        i++;
        f=nl[i];
    }
    free(nl);
//    *needs_free=0;
}


/*
    reset defaults if they've been changed by another javascript sql handle

    if this_idx > -1 - then we use 'this' to grab previously set settings
    and apply them after the reset.

    if this_idx < 0 - then reset is forced, but don't apply any saved settings
*/

void reset_tx_default(duk_context *ctx, DB_HANDLE *h, duk_idx_t this_idx)
{
    int handle_no=-1, last_handle_no=-1;

    if(this_idx > -1)
    {
        if( ! duk_get_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("handle_no")) )
            RP_THROW(ctx, "internal error getting handle id");
        handle_no = duk_get_int(ctx, -1);
        duk_pop(ctx);
        // see rampart-server.c:initThread - last_handle is set to -2 upon thread ctx creation
        // to force a reset of all settings that may have been set in main_ctx, but not applied to this thread/fork
        if(duk_get_global_string(ctx, DUK_HIDDEN_SYMBOL("sql_last_handle_no")))
            last_handle_no = duk_get_int(ctx, -1);
        duk_pop(ctx);
    }

    //if hard reset, or  we've set a setting before and it was made by a different sql handel
    //printf("handle=%d, last=%d\n",handle_no, last_handle_no);
    if( this_idx < 0 || (last_handle_no != -1       &&  last_handle_no != handle_no) )
    {
        //printf("HARD RESET\n");
        char errbuf[1024];
        int ret;
        if (this_idx > -1) //we reapply old setting
        {
            if (!duk_get_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("sql_settings")) )
            {
                //we have no old settings
                duk_pop(ctx);//undefined
                duk_push_object(ctx); //empty object, reset anyway
            }

        }
        else // just reset, don't apply settings
        {
            duk_push_object(ctx);
        }

        //if empty object, all settings will be reset to default

        // set the reset and settings if any
        if(h->needs_fork)
            ret = fork_set(ctx, h, errbuf);
        else
            ret = sql_set(ctx, h, errbuf);

        duk_pop(ctx);// no longer need settings object on stack

        if(ret == -1)
        {
            h_close(h);
            RP_THROW(ctx, "%s", errbuf);
        }
        else if (ret ==-2)
        {
            h_close(h);
            throw_tx_error(ctx, h, errbuf);
        }
    }
    if(last_handle_no != handle_no)
    {
        //set this javascript sql handle as the last to have applied settings
        duk_push_int(ctx, handle_no);
        duk_put_global_string(ctx, DUK_HIDDEN_SYMBOL("sql_last_handle_no"));
    }
}


duk_ret_t duk_texis_reset(duk_context *ctx)
{
    const char *db;
    DB_HANDLE *hcache = NULL;

    duk_push_this(ctx); //idx == 0

    //remove saved settings
    duk_del_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("sql_settings"));
    duk_del_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("indlist"));
    duk_del_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("explist"));

    if (!duk_get_prop_string(ctx, -1, "db"))
        RP_THROW(ctx, "no database is open");

    db = duk_get_string(ctx, -1);
    duk_pop_2(ctx);

    hcache = h_open(db, fromContext);

    if(!hcache || (!hcache->tx && !hcache->needs_fork) )
    {
        h_close(hcache);
        throw_tx_error(ctx,hcache,"sql open");
    }
    reset_tx_default(ctx, hcache, -1);
    h_close(hcache);
    return 0;
}

static void sql_normalize_prop(char *prop, const char *dprop)
{
    int i=0;

    strcpy(prop, dprop);

    for(i = 0; prop[i]; i++)
        prop[i] = tolower(prop[i]);

    /* a few aliases */
    if(!strcmp("listexp", prop) || !strcmp("listexpressions", prop))
        strcpy(prop, "lstexp");
    else if (!strcmp("listindextmp", prop) || !strcmp("listindextemp", prop) || !strcmp("lstindextemp", prop))
        strcpy(prop, "lstindextmp");
    else if (!strcmp("deleteindextmp", prop) || !strcmp("deleteindextemp", prop) || !strcmp("delindextemp", prop))
        strcpy(prop, "delindextmp");
    else if (!strcmp("addindextemp", prop))
        strcpy(prop, "addindextmp");
    else if (!strcmp("addexpressions", prop))
        strcpy(prop, "addexp");
    else if (!strcmp("delexpressions", prop) || !strcmp("deleteexpressions", prop))
        strcpy(prop, "delexp");
    else if (!strcmp("keepequivs", prop) || !strcmp("useequivs", prop))
        strcpy(prop, "useequiv");
    else if (!strcmp("equivsfile", prop))
        strcpy(prop, "eqprefix");
    else if (!strcmp("userequivsfile", prop))
        strcpy(prop, "ueqprefix");
    else if (!strcmp ("listnoise",prop))
        strcpy (prop, "lstnoise");
    else if (!strcmp ("listsuffix",prop))
        strcpy (prop, "lstsuffix");
    else if (!strcmp ("listsuffixequivs",prop))
        strcpy (prop, "lstsuffixeqivs");
    else if (!strcmp ("listprefix",prop))
        strcpy (prop, "lstprefix");
    else if (!strcmp ("noiselist",prop))
        strcpy (prop, "noiselst");
    else if (!strcmp ("suffixlist",prop))
        strcpy (prop, "suffixlst");
    else if (!strcmp ("suffixequivslist",prop))
        strcpy (prop, "suffixeqivslst");
    else if (!strcmp ("suffixeqlist",prop))
        strcpy (prop, "suffixeqlst");
    else if (!strcmp ("prefixlist",prop))
        strcpy (prop, "prefixlst");
}


TEXIS *setprop_tx=NULL;

static char *prop_defaults[][2] = {
   {"defaultLike", "like"},
   {"matchMode", "0"},
   {"pRedoPtType", "0"},
   {"textSearchMode", "unicodemulti, ignorecase, ignorewidth, ignorediacritics, expandligatures"},
   {"stringCompareMode", "unicodemulti, respectcase"},
   {"btreeCacheSize", "20"},
   {"ramRows", "0"},
   {"ramLimit", "0"},
   {"bubble", "1"},
   {"ignoreNewList", "0"},
   {"indexWithin", "0xf"},
   {"wildOneWord", "1"},
   {"wildSufMatch", "1"},
   {"alLinearDict", "0"},
   {"indexMinSublen", "2"},
   {"dropWordMode", "0"},
   {"metamorphStrlstMode", "equivlist"},
   /*{"groupbymem", "1"}, produces an error */
   {"minWordLen", "255"},
   {"suffixProc", "1"},
   {"rebuild", "0"},
   {"intersects", "-1"},
   {"hyphenPhrase", "1"},
   {"wordc", "[\\alpha\\']"},
   {"langc", "[\\alpha\\'\\-]"},
   {"withinMode", "word span"},
   {"phrasewordproc", "last"},
   {"defSuffRm", "1"},
   {"eqPrefix", "builtin"},
   {"exactPhrase", "0"},
   /* {"withinProc", "1"}, produces error */
   {"likepProximity", "500"},
   {"likepLeadBias", "500"},
   {"likepOrder", "500"},
   {"likepDocFreq", "500"},
   {"likepTblFreq", "500"},
   {"likepRows", "100"},
   {"likepMode", "1"},
   {"likepAllMatch", "0"},
   {"likepObeyIntersects", "0"},
   {"likepInfThresh", "0"},
   /* {"likepIndexThresh", "-1"}, ??? */
   /*{"indexSpace", ""},
   {"indexBlock", ""}, */
   {"meter", "on"},
/*   {"addExp", ""},
   {"delExp", ""},
   {"addIndexTmp", ""}
   {"delIndexTmp", ""}, */
   {"indexValues", "splitStrlst"},
   {"btreeThreshold", "50"},
   {"maxLinearRows", "1000"},
   {"likerRows", "1000"},
   {"indexAccess", "0"},
   {"indexMmap", "1"},
   {"indexReadBufSz", "64KB"},
   {"indexWriteBufSz", "128KB"},
   {"indexMmapBufSz", "0"},
   {"indexSlurp", "1"},
   {"indexAppend", "1"},
   {"indexWriteSplit", "1"},
   {"indexBtreeExclusive", "1"},
   {"indexVersion", "2"},
   {"mergeFlush", "1"},
   {"tableReadBufSz", "16KB"},
   /*{"tableSpace", ""},*/
   {"dateFmt", ""},
   /*{"timeZone", ""},
   {"locale", ""}, */
   /*{"indirectSpace", ""},*/
   {"triggerMode", "0"},
   {"paramChk", "1"},
   /* {"message", "1"}, segfault */
   {"varcharToStrlstMode", "json"},
   {"strlstToVarcharMode", "json"},
   {"multiValueToMultiRow", "0"},
   {"inMode", "subset"},
   {"hexifyBytes", "0"},
   {"unalignedBufferWarning", "1"},
   {"nullOutputString", "NULL"},
   /* {"validateBtrees", ""}, no idea */
   {"querySettings", "defaults"},
   {"qMaxWords", "1000"},
   {NULL, NULL}
};
int nnoiseList=181;
char *noiseList[] = {
    "a","about","after","again","ago","all","almost","also","always","am",
    "an","and","another","any","anybody","anyhow","anyone","anything","anyway","are",
    "as","at","away","back","be","became","because","been","before","being",
    "between","but","by","came","can","cannot","come","could","did","do",
    "does","doing","done","down","each","else","even","ever","every","everyone",
    "everything","for","from","front","get","getting","go","goes","going","gone",
    "got","gotten","had","has","have","having","he","her","here","him",
    "his","how","i","if","in","into","is","isn't","it","just",
    "last","least","left","less","let","like","make","many","may","maybe",
    "me","mine","more","most","much","my","myself","never","no","none",
    "not","now","of","off","on","one","onto","or","our","ourselves",
    "out","over","per","put","putting","same","saw","see","seen","shall",
    "she","should","so","some","somebody","someone","something","stand","such","sure",
    "take","than","that","the","their","them","then","there","these","they",
    "this","those","through","till","to","too","two","unless","until","up",
    "upon","us","very","was","we","went","were","what","what's","whatever",
    "when","where","whether","which","while","who","whoever","whom","whose","why",
    "will","with","within","without","won't","would","wouldn't","yet","you","your", ""
};
int nsuffixList=91;

char *suffixList[] = {
    "'","anced","ancer","ances","atery","enced","encer","ences","ibler","ment",
    "ness","tion","able","less","sion","ance","ious","ible","ence","ship",
    "ical","ward","ally","atic","aged","ager","ages","ated","ater","ates",
    "iced","icer","ices","ided","ider","ides","ised","ises","ived","ives",
    "ized","izer","izes","ncy","ing","ion","ity","ous","ful","tic",
    "ish","ial","ory","ism","age","ist","ate","ary","ual","ize",
    "ide","ive","ier","ess","ant","ise","ily","ice","ery","ent",
    "end","ics","est","ed","red","res","ly","er","al","at",
    "ic","ty","ry","en","nt","re","th","es","ul","s", ""
};

int nsuffixEquivsList=4;
char *suffixEquivsList[] = {
    "'","s","ies", ""
};

int nprefixList=29;
char *prefixList[] = {
    "ante","anti","arch","auto","be","bi","counter","de","dis","em",
    "en","ex","extra","fore","hyper","in","inter","mis","non","post",
    "pre","pro","re","semi","sub","super","ultra","un", ""
};

char **copylist(char **list, int len){
    int i=0;

    char **nl=NULL; /* the list to be populated */

    REMALLOC(nl, sizeof(char*) * len);

    while (i<len)
    {
        nl[i]=strdup(list[i]);
        i++;
    }

    return nl;
}

static int sql_defaults(duk_context *ctx, DB_HANDLE *hcache, char *errbuf)
{
    LPSTMT lpstmt;
    DDIC *ddic=NULL;

    TEXIS *tx;
    char pbuf[msgbufsz];
    int i=0;
    char **props;

    clearmsgbuf();

    duk_rp_log_error(ctx, "");

    if(!setprop_tx)
    {
        setprop_tx = texis_open((char *)(hcache->db), "PUBLIC", "");
    }
    tx=setprop_tx;

    if(!tx)
    {
        msgtobuf(pbuf);
        sprintf(errbuf,"Texis setprop open failed: (fork:%d, db:%s)\n%s", thisfork, (char *)(hcache->db), pbuf);
        return -1;
    }

    lpstmt = tx->hstmt;
    if(lpstmt && lpstmt->dbc && lpstmt->dbc->ddic)
            ddic = lpstmt->dbc->ddic;
    else
    {
        sprintf(errbuf,"sql open");
        return -1;
    }

    props = prop_defaults[i];
    while (props[0])
    {
        if(setprop(ddic, props[0], props[1] )==-1)
        {
            sprintf(errbuf, "sql reset");
            return -2;
        }
        i++;
        props = prop_defaults[i];
    }

    if(!defnoise)
    {
        globalcp->noise=(byte**)copylist(noiseList, nnoiseList);
        defnoise=1;
    }
    if(!defsuffix)
    {
        globalcp->suffix=(byte**)copylist(suffixList, nsuffixList);
        defsuffix=1;
    }
    if(!defsuffixeq)
    {
        globalcp->suffixeq=(byte**)copylist(suffixEquivsList, nsuffixEquivsList);
        defsuffixeq=1;
    }
    if(!defprefix)
    {
        globalcp->prefix=(byte**)copylist(prefixList, nprefixList);
        defprefix=1;
    }

    return 0;
}

// returns -1 for bad option, -2 for setprop error, 0 for ok, 1 for ok with return value
static int sql_set(duk_context *ctx, DB_HANDLE *hcache, char *errbuf)
{
    LPSTMT lpstmt;
    DDIC *ddic=NULL;
    TEXIS *tx;
    const char *val="";
    char pbuf[msgbufsz];
    int added_ret_obj=0, ret=0;
    char *rlsts[]={"noiseList","suffixList","suffixEquivsList","prefixList"};

    clearmsgbuf();


    duk_rp_log_error(ctx, "");

    if(!setprop_tx)
    {
        setprop_tx = texis_open((char *)(hcache->db), "PUBLIC", "");
    }
    tx=setprop_tx;

    if(!tx)
    {
        msgtobuf(pbuf);
        sprintf(errbuf,"Texis setprop open failed: (fork:%d, db:%s)\n%s", thisfork, (char *)(hcache->db), pbuf);
        goto return_neg_two;
    }

    lpstmt = tx->hstmt;
    if(lpstmt && lpstmt->dbc && lpstmt->dbc->ddic)
            ddic = lpstmt->dbc->ddic;
    else
    {
        sprintf(errbuf,"sql open");
        goto return_neg_two;
    }

    if((ret=sql_defaults(ctx, hcache, errbuf)))
        return ret;

    /**** Reapply indextmplst and explst if it was modified */
    duk_push_this(ctx);
    if(duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("indlist")))
    {
        int i=0, len = duk_get_length(ctx, -1);
        const char *val;

        for (i=0;i<len;i++)
        {
            duk_get_prop_index(ctx, -1, (duk_uarridx_t)i);
            val = duk_get_string(ctx, -1);
            if(setprop(ddic, "addindextmp", (char*)val )==-1)
            {
                sprintf(errbuf, "sql set");
                goto return_neg_two;
            }
            duk_pop(ctx);
        }
    }
    duk_pop(ctx);//list or undef

    if(duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("explist")))
    {
        int i=0, len = duk_get_length(ctx, -1);
        const char *val;

        /* delete first entry for an empty list */
        if(setprop(ddic, "delexp", "0" )==-1)
        {
            sprintf(errbuf, "sql set");
            goto return_neg_two;
        }

        for (i=0;i<len;i++)
        {
            duk_get_prop_index(ctx, -1, (duk_uarridx_t)i);
            val = duk_get_string(ctx, -1);
            if(setprop(ddic, "addexp", (char*)val )==-1)
            {
                sprintf(errbuf, "sql set");
                goto return_neg_two;
            }
            duk_pop(ctx);
        }
    }
    duk_pop_2(ctx);// list and this

    // apply all settings in object
    duk_enum(ctx, -1, 0);
    while (duk_next(ctx, -1, 1))
    {
/*
        int retlisttype=-1, setlisttype=-1, i=0;
        char propa[64], *prop=&propa[0];
        duk_size_t sz;
        const char *dprop=duk_get_lstring(ctx, -2, &sz);

        if(sz>63)
        {
            sprintf(errbuf, "sql.set - '%s' - unknown/invalid property", dprop);
            goto return_neg_one;
        }

        sql_normalize_prop(prop, dprop);
*/
        const char *prop=duk_get_string(ctx, -2);
        int retlisttype=-1, setlisttype=-1;

        if (!strcmp ("lstnoise", prop))
            retlisttype=0;
        else if (!strcmp ("lstsuffix", prop))
            retlisttype=1;
        else if (!strcmp ("lstsuffixeqivs", prop))
            retlisttype=2;
        else if (!strcmp ("lstprefix", prop))
            retlisttype=3;
        else if (!strcmp ("noiselst", prop))
            setlisttype=0;
        else if (!strcmp ("suffixlst", prop))
            setlisttype=1;
        else if (!strcmp ("suffixeqivslst", prop))
            setlisttype=2;
        else if (!strcmp ("suffixeqlst", prop))
            setlisttype=2;
        else if (!strcmp ("prefixlst", prop))
            setlisttype=3;

        if( (!strcmp(prop, "lstexp")||!strcmp(prop, "lstindextmp")))
        {
            char **lst;
            int arryi=0;
            if(duk_is_boolean(ctx, -1))
            {
                if(!duk_get_boolean(ctx, -1))
                    goto propnext;
            }
            else
                RP_THROW(ctx, "sql.set - property '%s' requires a Boolean", prop);

            if(!added_ret_obj)
            {
                duk_push_object(ctx);
                duk_insert(ctx, 0);
                added_ret_obj=1;
            }

            duk_push_array(ctx);

            if (!strcmp(prop, "lstexp"))
                lst=TXgetglobalexp();
            else
                lst=TXgetglobalindextmp();

            while (lst[arryi] && strlen(lst[arryi]))
            {
                duk_push_string(ctx, lst[arryi]);
                duk_put_prop_index(ctx, -2, (duk_uarridx_t)arryi);
                arryi++;
            }

            duk_put_prop_string(ctx, 0,
                (
                    strcmp(prop, "lstindextmp")?"expressionsList":"indexTempList"
                )
            );

            goto propnext;
        }

        if(retlisttype>-1)
        {
            byte *nw;
            byte **lsts[]={globalcp->noise,globalcp->suffix,globalcp->suffixeq,globalcp->prefix};
            char *rprop=rlsts[retlisttype];
            byte **lst=lsts[retlisttype];
            int i=0;

            /* skip if false */
            if(duk_is_boolean(ctx, -1))
            {
                if(!duk_get_boolean(ctx, -1))
                    goto propnext;
            }
            else
                RP_THROW(ctx, "sql.set - property '%s' requires a Boolean", prop);

            if(!added_ret_obj)
            {
                duk_push_object(ctx);
                duk_insert(ctx, 0);
                added_ret_obj=1;
            }

            duk_push_array(ctx);
            while ( (nw=lst[i]) && *nw != '\0' )
            {
                duk_push_string(ctx, (const char *) nw);
                duk_put_prop_index(ctx, -2, i++);
            }

            duk_put_prop_string(ctx, 0, rprop);

            goto propnext;
        }

        if(setlisttype>-1)
        {
            char **nl=NULL; /* the list to be populated */
            /* set the new list up, then free and replace current list *
             * should be null or an array of strings ONLY              */
            if(duk_is_null(ctx, -1))
            {
                REMALLOC(nl, sizeof(char*) * 1);
                nl[0]=strdup("");
            }
            else if(duk_is_array(ctx, -1))
            {
                int len=duk_get_length(ctx, -1), i=0;

                REMALLOC(nl, sizeof(char*) * (len + 1));

                while (i<len)
                {
                    duk_get_prop_index(ctx, -1, i);
                    if(!(duk_is_string(ctx, -1)))
                    {
                        /* note that the RP_THROW below might be caught in js, so we need to clean up *
                         * terminate what we have so far, then free it                                */
                        nl[i]=strdup("");
                        free_list((char**)nl);
                        sprintf(errbuf, "sql.set: %s must be an array of strings", rlsts[setlisttype] );
                        goto return_neg_one;
                    }
                    nl[i]=strdup(duk_get_string(ctx, -1));
                    duk_pop(ctx);
                    i++;
                }
                nl[i]=strdup("");
            }
            else
            {
                sprintf(errbuf, "sql.set: %s must be an array of strings", rlsts[setlisttype] );
                goto return_neg_one;
            }
            switch(setlisttype)
            {
                case 0: free_list((char**)globalcp->noise);
                        globalcp->noise=(byte**)nl;
                        defnoise=0;
                        break;

                case 1: free_list((char**)globalcp->suffix);
                        globalcp->suffix=(byte**)nl;
                        defsuffix=0;
                        break;

                case 2: free_list((char**)globalcp->suffixeq);
                        globalcp->suffixeq=(byte**)nl;
                        defsuffixeq=0;
                        break;

                case 3: free_list((char**)globalcp->prefix);
                        globalcp->prefix=(byte**)nl;
                        defprefix=0;
                        break;
            }

            goto propnext;
        }

        /* addexp, delexp and addindextmp take one at a time, but may take multiple
           so handle arrays here
        */
        if
        (
            duk_is_array(ctx, -1) &&
            (
                !strcmp(prop,"addexp") |
                !strcmp(prop,"delexp") |
                !strcmp(prop,"delindextmp") |
                !strcmp(prop,"addindextmp")
            )
        )
        {
            int ptype=0;

            if(!strcmp(prop,"delexp"))
                ptype=1;
            else if (!strcmp(prop,"addindextmp"))
                ptype=2;
            else if (!strcmp(prop,"delindextmp"))
                ptype=3;

            duk_enum(ctx, -1, DUK_ENUM_ARRAY_INDICES_ONLY);
            while(duk_next(ctx, -1, 1))
            {
                const char *aval=NULL;

                if (ptype==1)
                {
                    if(duk_is_number(ctx, -1))
                    {
                        duk_to_string(ctx, -1);
                        aval=duk_get_string(ctx, -1);
                    }
                    else
                    {
                        aval=get_exp(ctx, -1);

                        if(!aval)
                        {
                            sprintf(errbuf, "sql.set: deleteExpressions - array must be an array of strings, expressions or numbers (expressions or expression index)\n");
                            goto return_neg_one;
                        }
                    }
                }
                else if (ptype==3)
                {
                    if(duk_is_number(ctx, -1))
                    {
                        duk_to_string(ctx, -1);
                        aval=duk_get_string(ctx, -1);
                    }
                    else if (duk_is_string(ctx, -1))
                    {
                        aval=duk_get_string(ctx, -1);
                    }
                    else
                    {
                        sprintf(errbuf, "sql.set: deleteIndexTemp - array must be an array of strings or numbers\n");
                        goto return_neg_one;
                    }
                }
                else if (ptype==0)
                {
                    aval=get_exp(ctx, -1);

                    if(!aval)
                    {
                        sprintf(errbuf, "sql.set: addExpressions - array must be an array of strings or expressions\n");
                        goto return_neg_one;
                    }
                }
                else
                {
                    if(!duk_is_string(ctx, -1))
                    {
                        sprintf(errbuf, "sql.set: addIndexTemp - array must be an array of strings\n");
                        goto return_neg_one;
                    }
                    aval=duk_get_string(ctx, -1);
                }
                if(setprop(ddic, (char*)prop, (char*)aval )==-1)
                {
                    sprintf(errbuf, "sql set");
                    goto return_neg_two;
                }
                duk_pop_2(ctx);
            }
            duk_pop(ctx);
        }
        else
        {
            if(duk_is_number(ctx, -1))
                duk_to_string(ctx, -1);
            if(duk_is_boolean(ctx, -1))
            {
                if(duk_get_boolean(ctx, -1))
                    val="1";
                else
                    val="0";
            }
            else
            {
                if(!(duk_is_string(ctx, -1)))
                {
                    sprintf(errbuf, "invalid value '%s'", duk_safe_to_string(ctx, -1));
                    goto return_neg_one;
                }
                val=duk_get_string(ctx, -1);
            }

/*
            if(!strcmp(prop,"querydefaults") || !strcmp(prop,"querydefault"))
            {
                if(!duk_get_boolean_default(ctx, -1, 1))
                    goto propnext;
                prop="querysettings";
                val="defaults";
            }
*/
            if(setprop(ddic, (char*)prop, (char*)val )==-1)
            {
                sprintf(errbuf, "sql set");
                goto return_neg_two;
            }
        }

        /* save the altered list for reapplication after reset */
        if( !strcmp(prop, "addexp")||!strcmp(prop, "addindextmp") ||
            !strcmp(prop, "delexp")||!strcmp(prop, "delindextmp")
          )
        {
            char **lst;
            int arryi=0;
            char type = prop[3]; // i for index, e for expression

            duk_push_this(ctx);
            duk_push_array(ctx);
            if (type == 'e')
                lst=TXgetglobalexp();
            else
                lst=TXgetglobalindextmp();

            while (lst[arryi] && strlen(lst[arryi]))
            {
                duk_push_string(ctx, lst[arryi]);
                duk_put_prop_index(ctx, -2, (duk_uarridx_t)arryi);
                arryi++;
            }
            if (type == 'e')
                duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("explist"));
            else
                duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("indlist"));

            duk_pop(ctx);//this
        }

        propnext:
        duk_pop_2(ctx);
        /* capture and throw errors here *
        msgtobuf(pbuf);
        if(*pbuf!='\0')
        {
            sprintf(errbuf, "sql.set(): %s", pbuf+4);
            goto return_neg_one;
        }
*/
    }
    duk_pop(ctx);//enum

    duk_rp_log_tx_error(ctx,hcache,pbuf); /* log any non fatal errors to this.errMsg */

    //tx=texis_close(tx);

    if(added_ret_obj)
    {
        duk_pull(ctx, 0);
        return 1;
    }
    return 0;

    return_neg_two:
        //tx=texis_close(tx);
        return -2;

    return_neg_one:
        //tx=texis_close(tx);
        return -1;
}

/*
static char *stringLower(const char *str)
{
    size_t len = strlen(str);
    char *lower = NULL;
    int i=0;

    REMALLOC(lower, len+1);
    for (; i < len; ++i) {
        lower[i] = tolower((unsigned char)str[i]);
    }
    lower[i] = '\0';
    return lower;
}
*/

// certain settings like lstexp and addexp should not remain in saved settings
static void clean_settings(duk_context *ctx)
{
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("sql_settings"));
    duk_remove(ctx, -2);
    duk_del_prop_string(ctx, -1, "lstexp");
    duk_del_prop_string(ctx, -1, "delexp");
    duk_del_prop_string(ctx, -1, "addexp");
    duk_del_prop_string(ctx, -1, "lstindextmp");
    duk_del_prop_string(ctx, -1, "delindextmp");
    duk_del_prop_string(ctx, -1, "addindextmp");
    duk_del_prop_string(ctx, -1, "lstnoise");
    duk_del_prop_string(ctx, -1, "lstsuffix");
    duk_del_prop_string(ctx, -1, "lstsuffixeqivs");
    duk_del_prop_string(ctx, -1, "lstprefix");
    duk_pop(ctx);//the settings list object
}

duk_ret_t duk_texis_set(duk_context *ctx)
{
    const char *db;
    DB_HANDLE *hcache = NULL;
    int ret = 0, handle_no=0;
    char errbuf[1024];
    char propa[64], *prop=&propa[0];

    duk_push_this(ctx); //idx == 1
    // this should always be true
    if( !duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("handle_no")) )
        RP_THROW(ctx, "internal error getting handle id");
    handle_no=duk_get_int(ctx, -1);
    duk_pop(ctx);

    if (!duk_get_prop_string(ctx, -1, "db"))
        RP_THROW(ctx, "no database is open");

    db = duk_get_string(ctx, -1);
    duk_pop(ctx);

    hcache = h_open(db, fromContext);

    if(!hcache || (!hcache->tx && !hcache->needs_fork) )
    {
        h_close(hcache);
        throw_tx_error(ctx,hcache,"sql open");
    }

    reset_tx_default(ctx, hcache, -1);

    duk_rp_log_error(ctx, "");

    if(!duk_is_object(ctx, 0) || duk_is_array(ctx, 0) || duk_is_function(ctx, 0) )
        RP_THROW(ctx, "sql.set() - object with {prop:value} expected as parameter - got '%s'",duk_safe_to_string(ctx, 0));

    //stack = [ settings_obj, this ]
    //get any previous settings from this
    if(! duk_get_prop_string(ctx, 1, DUK_HIDDEN_SYMBOL("sql_settings")) )
    {
        duk_pop(ctx);   // pop undefined,
        duk_push_object(ctx); // [ settings_obj, this, new_empty_old_settings ]
    }
                                              // [ settings_obj, this, old_settings ]
    /* copy properties, renamed as lowercase, into saved old settings */
    duk_enum(ctx, 0, 0);                      // [ settings_obj, this, old_settings, enum_obj ]
    while (duk_next(ctx, -1, 1))
    {
        //  inside loop -                        [ settings_obj, this , old_settings, enum_obj, key, val ]
        duk_size_t sz;
        const char *dprop=duk_get_lstring(ctx, -2, &sz);

        if(sz>63)
            RP_THROW(ctx, "sql.set - '%s' - unknown/invalid property", dprop);

        sql_normalize_prop(prop, dprop);

        // put val into old settings, overwriting old if exists
        duk_put_prop_string(ctx, 2, prop);    // [ settings_obj, this , old_settings, enum_obj, key ]
        duk_pop(ctx);                         // [ settings_obj, this , old_settings, enum_obj ]
    }
    duk_pop(ctx);                             // [ settings_obj, this, combined_settings ]
    duk_remove(ctx, 0);                       // [ this , combined_settings ]

    duk_dup(ctx, -1);                         // [ this , combined_settings, combined_settings ]
    duk_put_prop_string(ctx, 0,
      DUK_HIDDEN_SYMBOL("sql_settings"));     // [ this, combined_settings ]
    duk_remove(ctx, 0);                       // [ combined_settings ]

    /* going to a child proc */
    if(hcache->needs_fork)
    {
        // regular expressions in addexp don't survive cbor,
        // so get the source expression and replace in array
        // before sending to child
        duk_enum(ctx, -1, 0);
        while (duk_next(ctx, -1, 1))
        {
            const char *k = duk_get_string(ctx, -2);
            if(
                !strcasecmp("addexp",k)           ||
                !strcasecmp("addexpressions",k)   ||
                !strcmp("delexpressions", k)      ||
                !strcmp("deleteexpressions", k)   ||
                !strcasecmp("delexp",k)
            )
            {
                duk_uarridx_t ix=0, len;

                if(!duk_is_array(ctx, -1))
                    RP_THROW(ctx, "sql.set: %s - array must be an array of strings or expressions\n", k);

                len=duk_get_length(ctx, -1);
                while (ix < len)
                {
                    duk_get_prop_index(ctx, -1, ix);
                    if(duk_is_object(ctx,-1) && duk_has_prop_string(ctx,-1,"source") )
                    {
                        duk_get_prop_string(ctx, -1,"source");
                        duk_put_prop_index(ctx, -3, ix);
                    }
                    duk_pop(ctx);
                    ix++;
                }
            }
            duk_pop_2(ctx);
        }
        duk_pop(ctx);
        ret = fork_set(ctx, hcache, errbuf);
    }
    else // handled by this proc
    {
        ret = sql_set(ctx, hcache, errbuf);
    }

    h_close(hcache);

    if(ret == -1)
        RP_THROW(ctx, "%s", errbuf);
    else if (ret ==-2)
        throw_tx_error(ctx, hcache, errbuf);

    //store which sql object last made a settings change
    duk_push_int(ctx, handle_no);
    duk_put_global_string(ctx, DUK_HIDDEN_SYMBOL("sql_last_handle_no"));

    clean_settings(ctx);
    return (duk_ret_t) ret;
}

// create db lock, in case a server callback function opens a new
// texis db with create option, we don't want two threads doing this at once
#define CRLOCK RP_PTLOCK(&tx_create_lock);
#define CRUNLOCK RP_PTUNLOCK(&tx_create_lock);


/* **************************************************
   Sql("/database/path") constructor:

   var sql=new Sql("/database/path");
   var sql=new Sql("/database/path",true); //create db if not exists

   There are x handle caches, one for each thread
   There is one handle cache for all new sql.exec() calls
   in each thread regardless of how many dbs will be opened.

   Calling new Sql() only stores the name of the db path
   And there is only one database per new Sql("/db/path");

   Here we only check to see that the database exists and
   construct the js object.  Actual opening and caching
   of handles to the db is done in exec()
   ************************************************** */
duk_ret_t duk_rp_sql_constructor(duk_context *ctx)
{
    int sql_handle_no = 0;
    const char *db = REQUIRE_STRING(ctx, 0, "new Sql - first parameter must be a string (database path)");
    char pbuf[msgbufsz];
    DB_HANDLE *h;

    /* allow call to Sql() with "new Sql()" only */
    if (!duk_is_constructor_call(ctx))
    {
        return DUK_RET_TYPE_ERROR;
    }
    g_hcache_pid = getpid();

    /* with require_string above, this shouldn't happen */
    if(!db)
        RP_THROW(ctx,"new Sql - db is null\n");

    /* check for db first */
    h = h_open( (char*)db, fromContext);
    if (!h || (h->tx == NULL && !h->needs_fork) )
    {
        /*
         if sql=new Sql("/db/path",true), we will
         create the db if it does not exist
        */
        if (duk_is_boolean(ctx, 1) && duk_get_boolean(ctx, 1) != 0)
        {
            CRLOCK
            clearmsgbuf();
            if (!createdb(db))
            {
                duk_rp_log_tx_error(ctx,h,pbuf);
                CRUNLOCK
                RP_THROW(ctx, "cannot create database at '%s' - root path not found, lacking permission or other error\n%s)", db, pbuf);
            }
            CRUNLOCK
        }
        else
        {
            duk_rp_log_tx_error(ctx,h,pbuf);
            RP_THROW(ctx, "cannot open database at '%s'\n%s", db, pbuf);
        }
    }
    duk_rp_log_tx_error(ctx,h,pbuf); /* log any non fatal errors to this.errMsg */
    h_close(h);

    /* save the name of the database in 'this' */
    duk_push_this(ctx); /* -> stack: [ db this ] */
    duk_push_string(ctx, db);
    duk_put_prop_string(ctx, -2, "db");
    duk_push_number(ctx, RESMAX_DEFAULT);
    duk_put_prop_string(ctx, -2, "selectMaxRows");

    /* make a unique id for this sql handle in this ctx/thread.
     * Used to restore global texis settings previously set via
     * sql.set() so they act as if they are tied to the sql handle */
    if (duk_get_global_string(ctx, DUK_HIDDEN_SYMBOL("sql_handle_counter") ) )
    {
        sql_handle_no=duk_get_int(ctx, -1);
    }
    duk_pop(ctx);

    sql_handle_no++;

    // attach number to this
    duk_push_int(ctx, sql_handle_no);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("handle_no") );

    //update last number
    duk_push_int(ctx, sql_handle_no);
    duk_put_global_string(ctx, DUK_HIDDEN_SYMBOL("sql_handle_counter"));
    //end unique id for sql handle

    //currently unused, probably can be removed:
    SET_THREAD_UNSAFE(ctx);

    TXunneededRexEscapeWarning = 0; //silence rex escape warnings

    return 0;
}

/* **************************************************
   Initialize Sql module
   ************************************************** */
char install_dir[PATH_MAX+21];
duk_ret_t duk_rp_exec(duk_context *ctx);

duk_ret_t duk_open_module(duk_context *ctx)
{
    /* Set up locks:
     * this will be run once per new duk_context/thread in server.c
     * but needs to be done only once for all threads
     */

    CTXLOCK;
    if (!db_is_init)
    {
        char *TexisArgv[2];

        RP_PTINIT(&tx_handle_lock);
        RP_PTINIT(&tx_create_lock);

        REMALLOC(errmap, sizeof(char*));
        errmap[0]=NULL;
        //if thread 0 was to fork, we'd do this.
        //errmap[0] = mmap(NULL, msgbufsz, PROT_READ|PROT_WRITE, MAP_ANON|MAP_SHARED, -1, 0);;
        //but currently it does not, so we do this:
        REMALLOC(errmap[0], msgbufsz);
        mmsgfh = fmemopen(errmap[0], msgbufsz, "w+");

        TexisArgv[0]=rampart_exec;

        // To help find texislockd -- which might be only in the same dir
        // as 'rampart' executable.
        strcpy (install_dir, "--install-dir-force=");
        strcat (install_dir, rampart_bin);
        TexisArgv[1]=install_dir;

        if( TXinitapp(NULL, NULL, 2, TexisArgv, NULL, NULL) )
        {
            CTXUNLOCK;
            RP_THROW(ctx, "Failed to initialize rampart-sql in TXinitapp");
        }
        db_is_init = 1;
    }
    CTXUNLOCK;

    duk_push_object(ctx); // the return object

    duk_push_c_function(ctx, duk_rp_sql_constructor, 3 /*nargs*/);

    /* Push proto object that will be Sql.init.prototype. */
    duk_push_object(ctx); /* -> stack: [ {}, Sql protoObj ] */

    /* Set Sql.init.prototype.exec. */
    duk_push_c_function(ctx, duk_rp_sql_exec, 6 /*nargs*/);   /* [ {}, Sql protoObj fn_exe ] */
    duk_put_prop_string(ctx, -2, "exec");                    /* [ {}, Sql protoObj-->{exe:fn_exe} ] */

    /* set Sql.init.prototype.eval */
    duk_push_c_function(ctx, duk_rp_sql_eval, 4 /*nargs*/);  /*[ {}, Sql protoObj-->{exe:fn_exe} fn_eval ]*/
    duk_put_prop_string(ctx, -2, "eval");                    /*[ {}, Sql protoObj-->{exe:fn_exe,eval:fn_eval} ]*/

    /* set Sql.init.prototype.eval */
    duk_push_c_function(ctx, duk_rp_sql_one, 2 /*nargs*/);  /*[ {}, Sql protoObj-->{exe:fn_exe} fn_eval ]*/
    duk_put_prop_string(ctx, -2, "one");                    /*[ {}, Sql protoObj-->{exe:fn_exe,eval:fn_eval} ]*/

    /* set Sql.init.prototype.close */
    duk_push_c_function(ctx, duk_rp_sql_close, 0 /*nargs*/); /* [ {}, Sql protoObj-->{exe:fn_exe,...} fn_close ] */
    duk_put_prop_string(ctx, -2, "close");                   /* [ {}, Sql protoObj-->{exe:fn_exe,query:fn_exe,close:fn_close} ] */

    /* set Sql.init.prototype.set */
    duk_push_c_function(ctx, duk_texis_set, 1 /*nargs*/);   /* [ {}, Sql protoObj-->{exe:fn_exe,...} fn_set ] */
    duk_put_prop_string(ctx, -2, "set");                    /* [ {}, Sql protoObj-->{exe:fn_exe,query:fn_exe,close:fn_close,set:fn_set} ] */

    /* set Sql.init.prototype.reset */
    duk_push_c_function(ctx, duk_texis_reset, 0);
    duk_put_prop_string(ctx, -2, "reset");

    /* set Sql.init.prototype.importCsvFile */
    duk_push_c_function(ctx, duk_rp_sql_import_csv_file, 4 /*nargs*/);
    duk_put_prop_string(ctx, -2, "importCsvFile");

    /* set Sql.init.prototype.importCsv */
    duk_push_c_function(ctx, duk_rp_sql_import_csv_str, 4 /*nargs*/);
    duk_put_prop_string(ctx, -2, "importCsv");

    /* Set Sql.init.prototype = protoObj */
    duk_put_prop_string(ctx, -2, "prototype"); /* -> stack: [ {}, Sql-->[prototype-->{exe=fn_exe,...}] ] */
    duk_put_prop_string(ctx, -2, "init");      /* [ {init()} ] */

    duk_push_c_function(ctx, RPfunc_stringformat, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "stringFormat");

    duk_push_c_function(ctx, RPsqlFuncs_abstract, 5);
    duk_put_prop_string(ctx, -2, "abstract");

    duk_push_c_function(ctx, RPsqlFunc_sandr, 3);
    duk_put_prop_string(ctx, -2, "sandr");

    duk_push_c_function(ctx, RPsqlFunc_sandr2, 3);
    duk_put_prop_string(ctx, -2, "sandr2");

    /* rex|re2(
          expression,                     //string or array of strings
          searchItem,                     //string or buffer
          callback,                       // optional callback function
          options  -
            {
              exclude:                    // string: "none"      - return all hits
                                          //         "overlap"   - remove the shorter hit if matches overlap
                                          //         "duplicate" - current default - remove smaller if one hit entirely encompasses another
              submatches:		  true|false - include submatches in an array.
                                          if have callback function (true is default)
                                            - true  --  function(
                                                          match,
                                                          submatchinfo={expressionIndex:matchedExpressionNo,submatches:["array","of","submatches"]},{...}...]},
                                                          matchindex
                                                        )
                                            - false --  function(match,matchindex)
                                          if no function (false is default)
                                            - true  --  ret= [{match:"match1",expressionIndex:matchedExpressionNo,submatches:["array","of","submatches"]},{...}...]
                                            - false --  ret= ["match1","match2"...]
            }
        );
   return value is an array of matches.
   If callback is specified, return value is number of matches.
  */
    duk_push_c_function(ctx, RPdbFunc_rex, 4);
    duk_put_prop_string(ctx, -2, "rex");

    duk_push_c_function(ctx, RPdbFunc_re2, 4);
    duk_put_prop_string(ctx, -2, "re2");

    /* rexfile|re2file(
          expression,                     //string or array of strings
          filename,                       //file with text to be searched
          callback,                       // optional callback function
          options  -
            {
              exclude:                    // string: "none"      - return all hits
                                          //         "overlap"   - remove the shorter hit if matches overlap
                                          //         "duplicate" - current default - remove smaller if one hit entirely encompasses another
              submatches:		  true|false - include submatches in an array.
                                          if have callback function (true is default)
                                            - true  --  function(
                                                          match,
                                                          submatchinfo={expressionIndex:matchedExpressionNo,submatches:["array","of","submatches"]},{...}...]},
                                                          matchindex
                                                        )
                                            - false --  function(match,matchindex)
                                          if no function (false is default)
                                            - true  --  ret= [{match:"match1",expressionIndex:matchedExpressionNo,submatches:["array","of","submatches"]},{...}...]
                                            - false --  ret= ["match1","match2"...]
              delimiter:		  expression to match -- delimiter for end of buffer.  Default is "$" (end of line).  If your pattern crosses lines, specify
                                                                 a delimiter which will not do so and you will be guaranteed to match even if a match crosses internal read buffer boundry
            }
        );

   return value is an array of matches.
   If callback is specified, return value is number of matches.
  */
    duk_push_c_function(ctx, RPdbFunc_rexfile, 4);
    duk_put_prop_string(ctx, -2, "rexFile");

    duk_push_c_function(ctx, RPdbFunc_re2file, 4);
    duk_put_prop_string(ctx, -2, "re2File");

    duk_push_c_function(ctx, searchfile, 3);
    duk_put_prop_string(ctx, -2, "searchFile");

    add_exit_func(free_all_handles, NULL);

    return 1;
}
