/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:

#ident "$Id$"
#ident "Copyright (c) 2010 Tokutek Inc.  All rights reserved."

#include <config.h>

#include <pthread.h>
#include <toku_assert.h>
#include "toku_pthread.h"
#include "toku_fair_rwlock.h"

#include <stdio.h>
#include <memory.h>

struct toku_fair_rwlock_waiter_state {
    char is_read;
    struct toku_fair_rwlock_waiter_state *next;
    toku_cond_t cond;
};

#if defined(HAVE_GNU_TLS)
static __thread struct toku_fair_rwlock_waiter_state waitstate_var = {0, NULL, { PTHREAD_COND_INITIALIZER } };
#define GET_WAITSTATE(name) name = &waitstate_var
#else
static pthread_key_t waitstate_key;
static bool waitstate_key_initialized = false;

void
toku_rwlock_init(void)
{
    assert(!waitstate_key_initialized);
    int r = toku_pthread_key_create(&waitstate_key, toku_free);
    assert_zero(r);
    waitstate_key_initialized = true;
}

void
toku_rwlock_destroy(void)
{
    assert(waitstate_key_initialized);
    int r = toku_pthread_key_delete(waitstate_key);
    assert_zero(r);
    waitstate_key_initialized = false;
}

static struct toku_fair_rwlock_waiter_state *
get_waitstate(void)
{
    assert(waitstate_key_initialized);
    struct toku_fair_rwlock_waiter_state *waitstate = NULL;
    void *p = toku_pthread_getspecific(waitstate_key);
    if (!p) {
        p = toku_xmalloc(sizeof *waitstate);
        int r = toku_pthread_setspecific(waitstate_key, p);
        assert_zero(r);
    }
    waitstate = p;
    return waitstate;
}

#define GET_WAITSTATE(name) name = get_waitstate()
#endif

void toku_fair_rwlock_init (toku_fair_rwlock_t *rwlock) {
    rwlock->state=0LL;
    rwlock->waiters_head = NULL;
    rwlock->waiters_tail = NULL;
    toku_mutex_init(&rwlock->mutex, NULL);
}

void toku_fair_rwlock_destroy (toku_fair_rwlock_t *rwlock) {
    assert(rwlock->state==0); // no one can hold the mutex, and no one can hold any lock.
    toku_mutex_destroy(&rwlock->mutex);
}

#ifdef RW_DEBUG
static __thread int tid=-1;
static int next_tid=0;
static int get_tid (void) {
    if (tid==-1) {
	tid = __sync_fetch_and_add(&next_tid, 1);
    }
    return tid;
}
#define L(l) printf("t%02d %s:%d %s\n", get_tid(), __FILE__, __LINE__, #l)
#define LP(l,s) printf("t%02d %s:%d %s %lx (wlock=%d rcount=%d qcount=%d)\n", get_tid(), __FILE__, __LINE__, #l, s, s_get_wlock(s), s_get_rcount(s), s_get_qcount(s))
#else
#define L(l) ((void)0)
#define LP(l,s) ((void)s)
#endif

void foo (void);
void foo (void) {
    printf("%llx\n", RWS_QCOUNT_MASK|RWS_WLOCK_MASK);
}

int toku_fair_rwlock_rdlock_slow (toku_fair_rwlock_t *rwlock) {
    uint64_t s;
    struct toku_fair_rwlock_waiter_state *GET_WAITSTATE(waitstate);
    goto ML; // we start in the ML state.
 ML:
    toku_mutex_lock(&rwlock->mutex);
    goto R2;
 R2:
    s = rwlock->state;
    if (s_get_qcount(s)==0 && !s_get_wlock(s)) goto C2;
    else goto C3;
 C2:
    if (__sync_bool_compare_and_swap(&rwlock->state, s, s_incr_rcount(s))) goto MU;
    else goto R2;
 C3:
    if (__sync_bool_compare_and_swap(&rwlock->state, s, s_incr_qcount(s))) goto E;
    else goto R2;
 E:
    // Put me into the queue.
    if (rwlock->waiters_tail) {
	rwlock->waiters_tail->next = waitstate;
    } else {
	rwlock->waiters_head = waitstate;
    }
    rwlock->waiters_tail = waitstate;
    waitstate->next = NULL;
    waitstate->is_read = 1; 
    goto W;
 W:
    toku_cond_wait(&waitstate->cond, &rwlock->mutex);
    // must wait till we are at the head of the queue because of the possiblity of spurious wakeups.
    if (rwlock->waiters_head==waitstate) goto D; 
    else goto W;
 D:
    rwlock->waiters_head = waitstate->next;
    if (waitstate->next==NULL) {
	rwlock->waiters_tail = NULL;
    }
    goto WN;
 WN:
    // If the next guy is a reader then wake him up.
    if (waitstate->next!=NULL && waitstate->next->is_read) {
	toku_cond_signal(&rwlock->waiters_head->cond);
    }
    goto R4;
 R4:
    s = rwlock->state;
    goto C4;
 C4:
    if (__sync_bool_compare_and_swap(&rwlock->state, s, s_incr_rcount(s_decr_qcount(s)))) goto MU;
    else goto R4;
 MU:
    toku_mutex_unlock(&rwlock->mutex);
    goto DONE;
 DONE:
    return 0;
}

int toku_fair_rwlock_wrlock_slow (toku_fair_rwlock_t *rwlock) {
    uint64_t s;
    struct toku_fair_rwlock_waiter_state *GET_WAITSTATE(waitstate);
    goto ML;
 ML:
    L(ML);
    toku_mutex_lock(&rwlock->mutex);
    goto R2;
 R2:
    s = rwlock->state;
    LP(R2, s);
    if (s_get_qcount(s)==0 && !s_get_wlock(s) && s_get_rcount(s)==0) goto C2;
    else goto C3;
 C2:
    if (__sync_bool_compare_and_swap(&rwlock->state, s, s_set_wlock(s))) goto MU;
    else goto R2;
 C3:
    L(C3);
    if (__sync_bool_compare_and_swap(&rwlock->state, s, s_incr_qcount(s))) goto E;
    else goto R2;
 E:
    LP(E, rwlock->state);
    // Put me into the queue.
    if (rwlock->waiters_tail) {
	rwlock->waiters_tail->next = waitstate;
    } else {
	rwlock->waiters_head = waitstate;
    }
    rwlock->waiters_tail = waitstate;
    waitstate->next = NULL;
    waitstate->is_read = 0; 
    goto W;
 W:
    toku_cond_wait(&waitstate->cond, &rwlock->mutex);
    // must wait till we are at the head of the queue because of the possiblity of spurious wakeups.
    if (rwlock->waiters_head==waitstate) goto D; 
    else goto W;
 D:
    rwlock->waiters_head = waitstate->next;
    if (waitstate->next==NULL) {
	rwlock->waiters_tail = NULL;
    }
    goto R4;
 R4:
    s = rwlock->state;
    assert(!s_get_wlock(s));
    goto C4;
 C4:
    if (__sync_bool_compare_and_swap(&rwlock->state, s, s_set_wlock(s_decr_qcount(s)))) goto MU;
    else goto R4;
 MU:
    toku_mutex_unlock(&rwlock->mutex);
    goto DONE;
 DONE:
    return 0;
}

int toku_fair_rwlock_unlock_r_slow (toku_fair_rwlock_t *rwlock) {
    uint64_t s;
    goto ML;
 ML:
    toku_mutex_lock(&rwlock->mutex);
    goto R2;
 R2:
    s = rwlock->state;
    LP(R2, s);
    if (s_get_rcount(s)>1 || s_get_qcount(s)==0) goto C2;
    else goto C3;
 C2:
    if (__sync_bool_compare_and_swap(&rwlock->state, s, s_decr_rcount(s))) goto MU;
    else goto R2;
 C3:
    // rcount==1 and qcount>0
    if (__sync_bool_compare_and_swap(&rwlock->state, s, s_decr_rcount(s))) goto WN;
    else goto R2;
 WN:
    LP(WN, rwlock->state);
    toku_cond_signal(&rwlock->waiters_head->cond);
    goto MU;
 MU:
    toku_mutex_unlock(&rwlock->mutex);
    goto DONE;
 DONE:
    return 0;
}

int toku_fair_rwlock_unlock_w_slow (toku_fair_rwlock_t *rwlock) {
    uint64_t s;
    //assert(s_get_rcount(s)==0 && s_get_wlock(s));
    goto ML;
 ML:
    toku_mutex_lock(&rwlock->mutex);
    goto R2;
 R2:
    LP(R2, rwlock->state);
    s = rwlock->state;
    if (s_get_qcount(s)==0) goto C2;
    else goto C3;
 C2:
    if (__sync_bool_compare_and_swap(&rwlock->state, s, s_clear_wlock(s))) goto MU;
    else goto R2;
 C3:
    if (__sync_bool_compare_and_swap(&rwlock->state, s, s_clear_wlock(s))) goto WN;
    else goto R2;
 WN:
    LP(WN, rwlock->state);
    toku_cond_signal(&rwlock->waiters_head->cond);
    goto MU;
 MU:
    toku_mutex_unlock(&rwlock->mutex);
    goto DONE;
 DONE:
    return 0;
}

// This function is defined so we can measure the cost of a function call.
int fcall_nop (int i) {
    return i;
}