/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
* raid7.c : MD RAID7 personality (variable parity)
*
* raid7 stores K data chunks and M parity chunks per stripe,
* distributed across N drives. The layout is:
*
* layout = (data_chunks << 8) | (parity_chunks)
*/

#include "raid5.h"
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/raid/md_u.h>
#include "md.h"
#include <linux/raid/pq.h>

struct r7conf {
	struct mddev *mddev;
	/// The number of data and parity chunks per stripe
	int data_chunks;
	int parity_chunks;
	int raid_disks;
	/// The number of sectors per chunk
	sector_t chunk_sectors;
	struct md_rdev **disks;
	spinlock_t lock;
};

static void r7_parse_layout(struct r7conf *conf, int layout)
{
	conf->parity_chunks = layout & 0xFF;
	conf->data_chunks = (layout >> 8) & 0xFF;
}

static bool raid7_make_request(struct mddev *mddev, struct bio *bio)
{
	struct md_rdev *rdev;

	if (unlikely(bio->bi_opf & REQ_PREFLUSH) &&
	    md_flush_request(mddev, bio))
		return true;

	if (unlikely(bio_op(bio) == REQ_OP_DISCARD)) {
		bio_endio(bio);
		return true;
	}

	rdev_for_each(rdev, mddev)
	{
		if (test_bit(Faulty, &rdev->flags))
			continue;
		bio_set_dev(bio, rdev->bdev);
		bio->bi_iter.bi_sector += rdev->data_offset;
		submit_bio_noacct(bio);
		return true;
	}

	bio_io_error(bio);
	return true;
}

static sector_t raid7_size(struct mddev *mddev, sector_t sectors,
			   int raid_disks)
{
	sector_t array_sectors = 0;
	struct r7conf *conf = mddev->private;

	if (!sectors)
		sectors = mddev->dev_sectors;
	if (!raid_disks)
		raid_disks = mddev->raid_disks;

	/// Round down to a multiple of chunk_sectors
	sectors &= ~(sector_t)(conf->chunk_sectors - 1);

	/// Multiply by the number of disks in the array, then divide by the total number of chunks (data + parity)
	array_sectors = sectors * raid_disks;
	sector_div(array_sectors, conf->data_chunks + conf->parity_chunks);

	return array_sectors;
}

static void raid7_status(struct seq_file *seq, struct mddev *mddev)
{
	seq_printf(seq, " %dk chunks", mddev->chunk_sectors / 2);
}

int raid5_calc_degraded(struct mddev *mddev);

/**
 * Count the number of faulty disks in the array.
 */
int raid7_calc_degraded(struct mddev *mddev)
{
	int degraded = 0;
	struct md_rdev *rdev;

	rdev_for_each(rdev, mddev)
	{
		if (test_bit(Faulty, &rdev->flags))
			degraded++;
	}

	return degraded;
}

static void raid7_error(struct mddev *mddev, struct md_rdev *rdev)
{
	struct r7conf *conf = mddev->private;
	unsigned long flags;

	pr_crit("raid7(%s): Disk failure on %pg detected, failing array.\n",
		mdname(mddev), rdev->bdev);

	spin_lock_irqsave(&conf->lock, flags);
	set_bit(Faulty, &rdev->flags);
	clear_bit(In_sync, &rdev->flags);

	mddev->degraded = raid7_calc_degraded(mddev);

	if (mddev->degraded > conf->parity_chunks) {
		pr_crit("raid7(%s): Too many failed disks, array is now dead.\n",
			mdname(mddev));
		set_bit(MD_BROKEN, &mddev->flags);
		set_bit(Blocked, &rdev->flags);
		set_mask_bits(&mddev->sb_flags, 0,
			      BIT(MD_SB_CHANGE_DEVS) |
				      BIT(MD_SB_CHANGE_PENDING));
	} else {
		pr_crit("raid7(%s): Array is degraded, but still operational.\n",
			mdname(mddev));
	}

	// TODO recovery thread

	spin_unlock_irqrestore(&conf->lock, flags);
}

static void raid7_quiesce(struct mddev *mddev, int quiesce)
{
}

static void free_conf(struct r7conf *conf)
{
	kfree(conf);
}

static void raid7_free(struct mddev *mddev, void *priv)
{
	struct r7conf *conf = priv;

	md_unregister_thread(mddev, &mddev->thread);
	free_conf(conf);
}

static struct r7conf *setup_conf(struct mddev *mddev)
{
	struct r7conf *conf;

	conf = kzalloc(sizeof(*conf), GFP_KERNEL);
	if (!conf)
		return ERR_PTR(-ENOMEM);

	conf->mddev = mddev;
	conf->raid_disks = mddev->raid_disks;
	conf->chunk_sectors = mddev->chunk_sectors;
	r7_parse_layout(conf, mddev->layout);

	mddev->private = conf;
	return conf;
}

static void raid7d(struct md_thread *thread)
{
	struct mddev *mddev = thread->mddev;

	md_check_recovery(mddev);
}

static int raid7_run(struct mddev *mddev)
{
	int ret;
	struct r7conf *conf;

	if (!mddev->private) {
		conf = setup_conf(mddev);

		if (IS_ERR(conf)) {
			pr_err("raid7: failed to setup configuration\n");
			ret = PTR_ERR(conf);
			goto abort;
		}
	} else {
		conf = mddev->private;
	}
	char pers_name[6];

	sprintf(pers_name, "raid%d", mddev->new_level);
	rcu_assign_pointer(mddev->thread,
			   md_register_thread(raid7d, mddev, pers_name));
	if (!mddev->thread) {
		pr_warn("md/raid:%s: couldn't allocate thread.\n",
			mdname(mddev));
		ret = -ENOMEM;
		goto abort;
	}

	return 0;

abort:

	if (!IS_ERR(conf) && conf)
		free_conf(conf);
	return ret;
}

static struct md_personality raid7_personality = {

	.head = {
		.type = MD_PERSONALITY,
		.id = ID_RAID7,
		.name = "raid7",
		.owner = THIS_MODULE,
	},
	.make_request	= raid7_make_request,
	.run		= raid7_run,
	.free		= raid7_free,
	.status		= raid7_status,
	.error_handler	= raid7_error,
	.size		= raid7_size,
	.quiesce	= raid7_quiesce,
	// FIXME add .hot_remove_disk As this is missing failed array will panic
};

static int __init raid7_init(void)
{
	return register_md_submodule(&raid7_personality.head);
}

static void __exit raid7_exit(void)
{
	unregister_md_submodule(&raid7_personality.head);
}

module_init(raid7_init);
module_exit(raid7_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RAID-7 (variable parity) personality for MD");
MODULE_ALIAS("md-personality-7"); /* RAID7 */
MODULE_ALIAS("md-raid7");
MODULE_ALIAS("md-level-7");
