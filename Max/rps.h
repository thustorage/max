#ifndef _LINUX_MAX_RPS
#define _LINUX_MAX_RPS

#include <linux/atomic.h>
#include <linux/rwsem.h>
#include <linux/percpu.h>
#include <linux/wait.h>
#include <linux/lockdep.h>

struct rps {
    int __percpu *highway_cnt;
    atomic_t lowway_cnt;
    atomic_t writers_cnt;
    wait_queue_head_t writers_wait_q;
    struct rw_semaphore rw_sem;
};

void rps_down_read(struct rps *);

int rps_down_read_try_lock(struct rps *rps);

void rps_up_read(struct rps *);

void rps_down_write(struct rps *);

void rps_up_write(struct rps *);

int __rps_init_rwsem(struct rps *,
                     const char *, struct lock_class_key *);

void rps_free_rwsem(struct rps *);

#define rps_init_rwsem(sem)    \
do {                                \
    static struct lock_class_key __key;            \
    __rps_init_rwsem(sem, #sem, &__key);        \
}while(0)

#endif
