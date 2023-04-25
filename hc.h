#ifndef _LINUX_HC_H
#define _LINUX_HC_H

#include <linux/timex.h>
#include <linux/workqueue.h>    /* for work queue */
#include <linux/slab.h>         /* for kmalloc() */

#define DEF_HC_THREAD_MIN_SLEEP_TIME	10000	/* milliseconds */
#define DEF_HC_THREAD_MAX_SLEEP_TIME	30000
#define DEF_HC_THREAD_NOHC_SLEEP_TIME	300000	/* wait 5 min */

#define DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD 1000000
#define DEF_HC_HOTNESS_ENTRY_SHRINK_NUM 100000

#define MIN(a, b) ((a) < (b)) ? a : b
#define MAX(a, b) ((a) < (b)) ? b : a

struct f2fs_hc_kthread {
	struct task_struct *f2fs_hc_task;
	wait_queue_head_t hc_wait_queue_head;

	/* for hc sleep time */
	unsigned int min_sleep_time;
	unsigned int max_sleep_time;
	unsigned int no_hc_sleep_time;
};

int insert_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, __u64 value, int type);
int update_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr_old, block_t blkaddr_new, __u64 value, int type_old, int type_new);
__u64 lookup_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, int* type);
void reduce_hotness_entry(struct f2fs_sb_info *sbi);
void save_hotness_entry(struct f2fs_sb_info *sbi);
void release_hotness_entry(struct f2fs_sb_info *sbi);

int hotness_decide(struct f2fs_io_info *fio, int *type_old_ptr, __u64 *value_ptr);
void hotness_maintain(struct f2fs_io_info *fio, int type_old, int type_new, __u64 value);

#endif
