#include <linux/fs.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/freezer.h>
#include <linux/sched/signal.h>
#include <linux/slab_def.h>
#include <linux/random.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "hc.h"
#include "kmeans.h"

static DEFINE_MUTEX(mutex_reduce_he);

int insert_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, __u64 value, int type)
{
	radix_tree_insert(&sbi->hi->hotness_rt_array[type], blkaddr, (void *) value);
	sbi->hi->count++;
	sbi->hi->new_blk_cnt++;
	return 0;
}

int update_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr_old, block_t blkaddr_new, __u64 value, int type_old, int type_new)
{
	radix_tree_delete(&sbi->hi->hotness_rt_array[type_old], blkaddr_old);
	radix_tree_insert(&sbi->hi->hotness_rt_array[type_new], blkaddr_new, (void *) value);

	sbi->hi->upd_blk_cnt++;
	if (blkaddr_old != blkaddr_new) {
		sbi->hi->opu_blk_cnt++;
	} else {
		sbi->hi->ipu_blk_cnt++;
	}
	return 0;
}

__u64 lookup_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, int* type)
{
	void *value;
	value = radix_tree_lookup(&sbi->hi->hotness_rt_array[0], blkaddr);
	if (value) {
		*type = CURSEG_HOT_DATA;
		goto found;
	}
	value = radix_tree_lookup(&sbi->hi->hotness_rt_array[1], blkaddr);
	if (value) {
		*type = CURSEG_WARM_DATA;
		goto found;
	}
	value = radix_tree_lookup(&sbi->hi->hotness_rt_array[2], blkaddr);
	if (value) {
		*type = CURSEG_COLD_DATA;
		goto found;
	}
	// not_found
	*type = -1;
	return 0;
found:
	return (__u64) value;
}

void reduce_hotness_entry(struct f2fs_sb_info *sbi) {
	struct radix_tree_iter iter;
	void __rcu **slot;
	unsigned int count = 0;
	radix_tree_for_each_slot(slot, &sbi->hi->hotness_rt_array[2], &iter, 0) {
		if (count >= DEF_HC_HOTNESS_ENTRY_SHRINK_NUM) 
			break;
		radix_tree_delete(&sbi->hi->hotness_rt_array[2], iter.index);
		sbi->hi->count--;
		count++;
	}
	sbi->hi->rmv_blk_cnt += count;
	mutex_unlock(&mutex_reduce_he);
}

int hotness_decide(struct f2fs_io_info *fio, int *type_old_ptr, __u64 *value_ptr)
{
	__u64 value, LWS;
	__u32 IRR, IRR1;
	int type_new, type_old;
	enum temp_type temp;
	__u64 LWS_old = 0;
	type_old = -1;
	LWS = fio->sbi->total_writed_block_count;
	if (fio->old_blkaddr != __UINT32_MAX__) {
		value = lookup_hotness_entry(fio->sbi, fio->old_blkaddr, &type_old);
	}
	if (type_old == -1) { // 不存在
		IRR = __UINT32_MAX__ >> 2;
		IRR1 = IRR << 2;
		value = (LWS << 32) + IRR1;
		type_new = CURSEG_COLD_DATA;
		fio->temp = COLD;
		temp = fio->temp;
		fio->sbi->hi->counts[temp]++;
	} else {
		LWS_old = value >> 32;
		IRR = LWS - LWS_old;
		IRR1 = IRR << 2;
		value = (LWS << 32) + IRR1;
		if (fio->sbi->centers_valid) {
			type_new = kmeans_get_type(fio, IRR);
		} else {
			type_new = type_old;
		}
		if (IS_HOT(type_new))
			fio->temp = HOT;
		else if (IS_WARM(type_new))
			fio->temp = WARM;
		else
			fio->temp = COLD;
		temp = fio->temp;
		fio->sbi->hi->counts[temp]++;
		fio->sbi->hi->IRR_min[temp] = MIN(fio->sbi->hi->IRR_min[temp], IRR);
		fio->sbi->hi->IRR_max[temp] = MAX(fio->sbi->hi->IRR_max[temp], IRR);
	}
	fio->sbi->total_writed_block_count++;
	*type_old_ptr = type_old;
	*value_ptr = value;
	return type_new;
}

void hotness_maintain(struct f2fs_io_info *fio, int type_old, int type_new, __u64 value)
{
	if (type_old == -1) { /* 不存在 */
		insert_hotness_entry(fio->sbi, fio->new_blkaddr, value, type_new);
	} else { // 存在
		update_hotness_entry(fio->sbi, fio->old_blkaddr, fio->new_blkaddr, value, type_old, type_new);
	}

	mutex_lock(&mutex_reduce_he);
	if (fio->sbi->hi->count < DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD) {
		mutex_unlock(&mutex_reduce_he);
		return;
	}
	reduce_hotness_entry(fio->sbi);
}

static void init_hc_management(struct f2fs_sb_info *sbi)
{
	struct file *fp;
	loff_t pos = 0;
	unsigned int n_clusters;
	unsigned int i;
	unsigned int *centers;

	sbi->hi = f2fs_kmalloc(sbi, sizeof(struct hotness_info), GFP_KERNEL);
	
	INIT_RADIX_TREE(&sbi->hi->hotness_rt_array[0], GFP_NOFS);
	INIT_RADIX_TREE(&sbi->hi->hotness_rt_array[1], GFP_NOFS);
	INIT_RADIX_TREE(&sbi->hi->hotness_rt_array[2], GFP_NOFS);
	
	for(i = 0; i < TEMP_TYPE_NUM; i++){
		sbi->hi->IRR_min[i] = __UINT32_MAX__ >> 2;
	}

	centers = kmalloc(sizeof(unsigned int) * sbi->n_clusters, GFP_KERNEL);

	fp = filp_open("/tmp/f2fs_hotness_no", O_RDWR, 0644);
	if (IS_ERR(fp)) {
		printk("failed to open /tmp/f2fs_hotness.\n");
		sbi->total_writed_block_count = 0;
		sbi->n_clusters = N_CLUSTERS;
		sbi->centers = kmalloc(sizeof(unsigned int) * sbi->n_clusters, GFP_KERNEL);
		sbi->centers_valid = 0;
		goto out;
	}

	kernel_read(fp, &n_clusters, sizeof(n_clusters), &pos);
	sbi->n_clusters = n_clusters;

	// read centers
	for(i = 0; i < n_clusters; ++i) {
		kernel_read(fp, &centers[i], sizeof(centers[i]), &pos);
	}
	sbi->centers = centers;
	sbi->centers_valid = 1;

	filp_close(fp, NULL);
out:
	return;
}

void f2fs_build_hc_manager(struct f2fs_sb_info *sbi)
{
	init_hc_management(sbi);
}

static int kmeans_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct f2fs_hc_kthread *hc_th = sbi->hc_thread;
	wait_queue_head_t *wq = &sbi->hc_thread->hc_wait_queue_head;
	unsigned int wait_ms;
	int err;

	wait_ms = hc_th->min_sleep_time;

	set_freezable();
	do {
		wait_event_interruptible_timeout(*wq, kthread_should_stop() || freezing(current), msecs_to_jiffies(wait_ms));
		err = f2fs_hc(sbi);
		if (!err) sbi->centers_valid = 1;
	} while (!kthread_should_stop());
	return 0;
}

int f2fs_start_hc_thread(struct f2fs_sb_info *sbi)
{
    struct f2fs_hc_kthread *hc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	int err = 0;

	hc_th = f2fs_kmalloc(sbi, sizeof(struct f2fs_hc_kthread), GFP_KERNEL);
	if (!hc_th) {
		err = -ENOMEM;
		goto out;
	}

	hc_th->min_sleep_time = DEF_HC_THREAD_MIN_SLEEP_TIME;
	hc_th->max_sleep_time = DEF_HC_THREAD_MAX_SLEEP_TIME;
	hc_th->no_hc_sleep_time = DEF_HC_THREAD_NOHC_SLEEP_TIME;

    sbi->hc_thread = hc_th;
	init_waitqueue_head(&sbi->hc_thread->hc_wait_queue_head);
    sbi->hc_thread->f2fs_hc_task = kthread_run(kmeans_thread_func, sbi,
			"f2fs_hc-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(hc_th->f2fs_hc_task)) {
		err = PTR_ERR(hc_th->f2fs_hc_task);
		kfree(hc_th);
		sbi->hc_thread = NULL;
	}
out:
	return err;
}

void f2fs_stop_hc_thread(struct f2fs_sb_info *sbi) 
{
	struct f2fs_hc_kthread *hc_th = sbi->hc_thread;
	
	if (!hc_th)
		return;
	kthread_stop(hc_th->f2fs_hc_task);
	kfree(hc_th);
	sbi->hc_thread = NULL;
}

void save_hotness_entry(struct f2fs_sb_info *sbi)
{
	struct file *fp;
	loff_t pos = 0;
	unsigned int i;

	fp = filp_open("/tmp/f2fs_hotness", O_RDWR|O_CREAT, 0644);
	if (IS_ERR(fp)) goto out;

	// save n_clusters
	kernel_write(fp, &sbi->n_clusters, sizeof(sbi->n_clusters), &pos);
	// save centers
	for(i = 0; i < sbi->n_clusters; i++) {
		kernel_write(fp, &sbi->centers[i], sizeof(sbi->centers[i]), &pos);
	}
	filp_close(fp, NULL);
out:
	return;
}

void release_hotness_entry(struct f2fs_sb_info *sbi)
{
	struct radix_tree_iter iter;
	void __rcu **slot;
	int type;

	if (sbi->centers) kfree(sbi->centers);
	if (sbi->hi->count == 0) return;
	for (type = 0; type < 3; type++) {
		radix_tree_for_each_slot(slot, &sbi->hi->hotness_rt_array[type], &iter, 0) {
			radix_tree_delete(&sbi->hi->hotness_rt_array[type], iter.index);
		}
	}
}

unsigned int get_segment_hotness_avg(struct f2fs_sb_info *sbi, unsigned int segno)
{
	int off;
	block_t blk_addr;
	__u64 value;
	__u32 IRR;
	int type;
	unsigned int valid = 0;
	block_t start_addr = START_BLOCK(sbi, segno);
	unsigned int usable_blks_in_seg = sbi->blocks_per_seg;
	__u64 IRR_sum = 0;
	for (off = 0; off < usable_blks_in_seg; off++) {
		blk_addr = start_addr + off;
		value = lookup_hotness_entry(sbi, blk_addr, &type);
		if (type != -1)	{
    		IRR = (value & 0xffffffff) >> 2;
			IRR_sum += IRR;
			valid++;
		}
	}
	if (valid == 0) return __UINT32_MAX__ >> 2; 
	else return IRR_sum / valid;
}

bool hc_can_inplace_update(struct f2fs_io_info *fio)
{
	unsigned int segno;
	int type_blk, type_seg;
	unsigned int IRR_blk, IRR_seg;
	__u64 value;
	if (fio->type == DATA && fio->old_blkaddr != __UINT32_MAX__) {
		value = lookup_hotness_entry(fio->sbi, fio->old_blkaddr, &type_blk);
	}
	if (type_blk != -1 && fio->sbi->centers_valid) {
		IRR_blk = (value & 0xffffffff) >> 2;
		segno = GET_SEGNO(fio->sbi, fio->old_blkaddr);
		IRR_seg = get_segment_hotness_avg(fio->sbi, segno);
		type_seg = kmeans_get_type(fio, IRR_seg);

		if (type_blk == type_seg)	return true;
		else	return false;
	} else {
		return true;
	}
}
