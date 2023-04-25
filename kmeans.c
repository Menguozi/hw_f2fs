#include <linux/fs.h>

#include <linux/module.h>
#include <linux/f2fs_fs.h>
#include <linux/random.h>
#include <linux/radix-tree.h>
#include <linux/timex.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "hc.h"
#include "kmeans.h"

#define diff(a, b) (a) < (b) ? ((b) - (a)) : ((a) - (b))
#define MIN_3(a, b, c) ((a) < (b)) ? (((a) < (c)) ? CURSEG_HOT_DATA : CURSEG_COLD_DATA) : (((c) > (b)) ? CURSEG_WARM_DATA : CURSEG_COLD_DATA)
#define MIN_2(a, b) ((a) < (b)) ? CURSEG_HOT_DATA : CURSEG_WARM_DATA
#define MAX_LOOP_NUM 1000
#define RANDOM_SEED 0  // 0为kmeans++播种，1为随机播种

static void add_to_nearest_set(unsigned int data, long long *mass_center, int center_num);
static int find_initial_cluster(unsigned int *data, int data_num, long long *mass_center, int center_num, int init_random);
static unsigned long long random(void);
static void bubble_sort(unsigned int *x, int num);

struct timespec64 ts_start, ts_end;
struct timespec64 ts_delta;

int f2fs_hc(struct f2fs_sb_info *sbi)
{
    struct radix_tree_iter iter;
	void __rcu **slot;
	__u64 value;
	__u32 IRR;
	block_t blk_addr;
    int type;
    int center_num;
    unsigned int *data;
    long long *mass_center;
    int data_num;
    int i, flag, loop_count, j;
    int ret = 0;

    ktime_get_boottime_ts64(&ts_start);

    printk("Doing f2fs_hc, count = %u.\n", sbi->hi->count);
    if (sbi->hi->count == 0 || sbi->hi->count > 100000000) {
        printk("In function %s, sbi->hi->count is out of valid range(1~100000000).\n", __func__);
        return -1;
    }

    center_num = 2;
    sbi->centers[2] = __UINT32_MAX__ >> 2;

    data = vmalloc(sizeof(unsigned int) * sbi->hi->count);
    if (!data) {
        printk("In %s: data == NULL, count = %u.\n", __func__, sbi->hi->count);
        return -1;
    }
    mass_center = kmalloc(sizeof(long long) * center_num * 3, GFP_KERNEL); //存放质心，平均值，集合元素数
    if (!mass_center) {
        printk("In %s: mass_center == NULL.\n", __func__);
        return -1;
    }
    data_num = 0;
    
    for (type = 0; type < 3; type++) {
        radix_tree_for_each_slot(slot, &sbi->hi->hotness_rt_array[type], &iter, 0) {
			blk_addr = iter.index;
			value = (__u64) radix_tree_lookup(&sbi->hi->hotness_rt_array[type], blk_addr);
    		IRR = (value & 0xffffffff) >> 2;
            if(IRR && (IRR != (__UINT32_MAX__ >> 2)))
            {
                data[data_num++] = IRR;
            }
        }
    }

    printk("In function %s, data_num = %d.\n", __func__, data_num);
    if (data_num == 0) {
        printk("In %s: data_num == 0.\n", __func__);
        ret = -1;
        goto out;
    }
    if (find_initial_cluster(data, data_num, mass_center, center_num, RANDOM_SEED)) {
        printk("In %s: find_initial_cluster error.\n", __func__);
        return -1;
    }
    flag = 1;
    loop_count = 0;
    while (flag == 1 && loop_count < MAX_LOOP_NUM)
    {
        flag = 0;
        ++loop_count;

        for (i = 0; i < center_num; ++i)
        {
            mass_center[i * 3 + 1] = 0;
            mass_center[i * 3 + 2] = 0;
        }
        for (j = 0; j < data_num; ++j)
            add_to_nearest_set(data[j], mass_center, center_num);
        for (i = 0; i < center_num; ++i)
        {
            if (mass_center[i * 3 + 2] == 0)
                continue;
            if (mass_center[i * 3] != mass_center[i * 3 + 1] / mass_center[i * 3 + 2])
            {
                flag = 1;
                mass_center[i * 3] = mass_center[i * 3 + 1] / mass_center[i * 3 + 2];
            }
        }
    }
    for (i = 0; i < center_num; ++i)
        sbi->centers[i] = (unsigned int)mass_center[i * 3];
    bubble_sort(sbi->centers, center_num);

    if (center_num == 3) 
        printk("centers: %u, %u, %u\n", sbi->centers[0], sbi->centers[1], sbi->centers[2]);
    else if (center_num == 2)
        printk("centers: %u, %u\n", sbi->centers[0], sbi->centers[1]);
    else
        printk("center num is error!\n");

out:
    vfree(data);
    kfree(mass_center);

    ktime_get_boottime_ts64(&ts_end);
    ts_delta = timespec64_sub(ts_end, ts_start);
    printk("[f2fs] time consumed: %lld (ns)\n",timespec64_to_ns(&ts_delta));

    return ret;
}

int kmeans_get_type(struct f2fs_io_info *fio, __u32 IRR)
{
    int type;
    if(fio->sbi->n_clusters == 3) {
        type = MIN_3(diff(IRR, fio->sbi->centers[0]),
                     diff(IRR, fio->sbi->centers[1]),
                     diff(IRR, fio->sbi->centers[2]));
    } else {
        type = MIN_2(diff(IRR, fio->sbi->centers[0]),
                     diff(IRR, fio->sbi->centers[1]));
    }
    
    return type;
}

static int find_initial_cluster(unsigned int *data, int data_num, long long *mass_center, int center_num, int init_random)
{
    int i, j, k;
    unsigned int *distance;
    unsigned long long total_distance;
    unsigned long long threshold;
    unsigned long long distance_sum;
    //随机播种s
    if (init_random == 1)
    {
random_seed:
        for (i = 0; i < center_num; ++i)
            mass_center[i * 3] = data[(int)(random() % data_num)];
        return 0;
    }
    // kmeans++播种
    mass_center[0] = data[(int)(random() % data_num)];
    distance = vmalloc(sizeof(unsigned int) * data_num);
    if (!distance) {
        printk("In %s: distance == NULL, data_num = %d.\n", __func__, data_num);
        return -1;
    }
    for (k = 1; k < center_num; ++k)
    {
        total_distance = 0;
        //求每一个元素到当前所有质心的距离
        for (j = 0; j < data_num; ++j)
        {
            distance[j] = 0;
            for (i = 0; i < k; i++)
                distance[j] += diff(mass_center[i * 3], data[j]);
            total_distance += distance[j];
        }
        //距离当前质心越远的元素更有可能被选为质心
        if (total_distance == 0) goto random_seed;
        threshold = random() % total_distance;
        distance_sum = 0;
        for (j = 0; j < data_num; ++j)
        {
            distance_sum += distance[j];
            if (distance_sum >= threshold)
                break;
        }
        //产生了新的质心
        mass_center[k * 3] = data[j];
    }
    vfree(distance);
    return 0;
}

static unsigned long long random(void)
{
    unsigned long long x;
    get_random_bytes(&x, sizeof(x));
    return x;
}

static void add_to_nearest_set(unsigned int data, long long *mass_center, int center_num)
{
    /*
     * 将输入的参数点寻找最近的质心，并加入质心的函数中
     */
    unsigned int min = diff(mass_center[0], data);
    int position = 0, i;
    for (i = 1; i < center_num; i++)
    {
        unsigned int temp = diff(mass_center[i * 3], data);
        if (temp < min)
        {
            min = temp;
            position = i;
        }
    }
    mass_center[position * 3 + 1] += data;
    ++mass_center[position * 3 + 2];
}

static void bubble_sort(unsigned int *x, int num)
{
    int temp, i, j;
    for (i = 0; i < num - 1; ++i)
        for (j = 0; j < num - 1 - i; ++j)
            if (x[j] > x[j + 1])
            {
                temp = x[j + 1];
                x[j + 1] = x[j];
                x[j] = temp;
            }
    return;
}
