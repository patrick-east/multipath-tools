/*
 * Soft:        multipath device mapper target autoconfig
 *
 * Version:     $Id: main.h,v 0.0.1 2003/09/18 15:13:38 cvaroqui Exp $
 *
 * Author:      Christophe Varoqui
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (c) 2003, 2004, 2005 Christophe Varoqui
 * Copyright (c) 2005 Benjamin Marzinski, Redhat
 * Copyright (c) 2005 Kiyoshi Ueda, NEC
 * Copyright (c) 2005 Patrick Caulfield, Redhat
 * Copyright (c) 2005 Edward Goggin, EMC
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <libudev.h>
#include <syslog.h>
#include <fcntl.h>

#include "checkers.h"
#include "prio.h"
#include "vector.h"
#include "memory.h"
#include <libdevmapper.h>
#include "devmapper.h"
#include "util.h"
#include "defaults.h"
#include "config.h"
#include "structs.h"
#include "structs_vec.h"
#include "dmparser.h"
#include "sysfs.h"
#include "blacklist.h"
#include "discovery.h"
#include "debug.h"
#include "switchgroup.h"
#include "dm-generic.h"
#include "print.h"
#include "alias.h"
#include "configure.h"
#include "pgpolicies.h"
#include "version.h"
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "wwids.h"
#include "uxsock.h"
#include "mpath_cmd.h"
#include "foreign.h"
#include "propsel.h"
#include "time-util.h"
#include "file.h"

int logsink;
struct udev *udev;
struct config *multipath_conf;

struct config *get_multipath_config(void)
{
	return multipath_conf;
}

void put_multipath_config(void *arg)
{
	/* Noop for now */
}

static int
dump_config (struct config *conf, vector hwes, vector mpvec)
{
	char * reply = snprint_config(conf, NULL, hwes, mpvec);

	if (reply != NULL) {
		printf("%s", reply);
		FREE(reply);
		return 0;
	} else
		return 1;
}

void rcu_register_thread_memb(void) {}

void rcu_unregister_thread_memb(void) {}

static int
filter_pathvec (vector pathvec, char * refwwid)
{
	int i;
	struct path * pp;

	if (!refwwid || !strlen(refwwid))
		return 0;

	vector_foreach_slot (pathvec, pp, i) {
		if (strncmp(pp->wwid, refwwid, WWID_SIZE) != 0) {
			condlog(3, "skip path %s : out of scope", pp->dev);
			free_path(pp);
			vector_del_slot(pathvec, i);
			i--;
		}
	}
	return 0;
}

static void
usage (char * progname)
{
	fprintf (stderr, VERSION_STRING);
	fprintf (stderr, "Usage:\n");
	fprintf (stderr, "  %s [-a|-c|-w|-W] [-d] [-r] [-i] [-v lvl] [-p pol] [-b fil] [-q] [dev]\n", progname);
	fprintf (stderr, "  %s -l|-ll|-f [-v lvl] [-b fil] [-R num] [dev]\n", progname);
	fprintf (stderr, "  %s -F [-v lvl] [-R num]\n", progname);
	fprintf (stderr, "  %s [-t|-T]\n", progname);
	fprintf (stderr, "  %s -h\n", progname);
	fprintf (stderr,
		"\n"
		"Where:\n"
		"  -h      print this usage text\n"
		"  -l      show multipath topology (sysfs and DM info)\n"
		"  -ll     show multipath topology (maximum info)\n"
		"  -f      flush a multipath device map\n"
		"  -F      flush all multipath device maps\n"
		"  -a      add a device wwid to the wwids file\n"
		"  -c      check if a device should be a path in a multipath device\n"
		"  -C      check if a multipath device has usable paths\n"
		"  -q      allow queue_if_no_path when multipathd is not running\n"
		"  -d      dry run, do not create or update devmaps\n"
		"  -t      display the currently used multipathd configuration\n"
		"  -T      display the multipathd configuration without builtin defaults\n"
		"  -r      force devmap reload\n"
		"  -i      ignore wwids file\n"
		"  -B      treat the bindings file as read only\n"
		"  -b fil  bindings file location\n"
		"  -w      remove a device from the wwids file\n"
		"  -W      reset the wwids file include only the current devices\n"
		"  -p pol  force all maps to specified path grouping policy :\n"
		"          . failover            one path per priority group\n"
		"          . multibus            all paths in one priority group\n"
		"          . group_by_serial     one priority group per serial\n"
		"          . group_by_prio       one priority group per priority lvl\n"
		"          . group_by_node_name  one priority group per target node\n"
		"  -v lvl  verbosity level\n"
		"          . 0 no output\n"
		"          . 1 print created devmap names only\n"
		"          . 2 default verbosity\n"
		"          . 3 print debug information\n"
		"  -R num  number of times to retry removes of in-use devices\n"
		"  dev     action limited to:\n"
		"          . multipath named 'dev' (ex: mpath0) or\n"
		"          . multipath whose wwid is 'dev' (ex: 60051..)\n"
		"          . multipath including the path named 'dev' (ex: /dev/sda)\n"
		"          . multipath including the path with maj:min 'dev' (ex: 8:0)\n"
		);

}

static int
update_paths (struct multipath * mpp, int quick)
{
	int i, j;
	struct pathgroup * pgp;
	struct path * pp;
	struct config *conf;

	if (!mpp->pg)
		return 0;

	vector_foreach_slot (mpp->pg, pgp, i) {
		if (!pgp->paths)
			continue;

		vector_foreach_slot (pgp->paths, pp, j) {
			if (!strlen(pp->dev)) {
				if (devt2devname(pp->dev, FILE_NAME_SIZE,
						 pp->dev_t)) {
					/*
					 * path is not in sysfs anymore
					 */
					pp->chkrstate = pp->state = PATH_DOWN;
					pp->offline = 1;
					continue;
				}
				pp->mpp = mpp;
				if (quick)
					continue;
				conf = get_multipath_config();
				if (pathinfo(pp, conf, DI_ALL))
					pp->state = PATH_UNCHECKED;
				put_multipath_config(conf);
				continue;
			}
			pp->mpp = mpp;
			if (quick)
				continue;
			if (pp->state == PATH_UNCHECKED ||
			    pp->state == PATH_WILD) {
				conf = get_multipath_config();
				if (pathinfo(pp, conf, DI_CHECKER))
					pp->state = PATH_UNCHECKED;
				put_multipath_config(conf);
			}

			if (pp->priority == PRIO_UNDEF) {
				conf = get_multipath_config();
				if (pathinfo(pp, conf, DI_PRIO))
					pp->priority = PRIO_UNDEF;
				put_multipath_config(conf);
			}
		}
	}
	return 0;
}

static int
get_dm_mpvec (enum mpath_cmds cmd, vector curmp, vector pathvec, char * refwwid)
{
	int i;
	struct multipath * mpp;
	char params[PARAMS_SIZE], status[PARAMS_SIZE];

	if (dm_get_maps(curmp))
		return 1;

	vector_foreach_slot (curmp, mpp, i) {
		/*
		 * discard out of scope maps
		 */
		if (refwwid && strlen(refwwid) &&
		    strncmp(mpp->wwid, refwwid, WWID_SIZE)) {
			condlog(3, "skip map %s: out of scope", mpp->alias);
			free_multipath(mpp, KEEP_PATHS);
			vector_del_slot(curmp, i);
			i--;
			continue;
		}

		if (cmd == CMD_VALID_PATH)
			continue;

		dm_get_map(mpp->alias, &mpp->size, params);
		condlog(3, "params = %s", params);
		dm_get_status(mpp->alias, status);
		condlog(3, "status = %s", status);

		disassemble_map(pathvec, params, mpp, 0);

		/*
		 * disassemble_map() can add new paths to pathvec.
		 * If not in "fast list mode", we need to fetch information
		 * about them
		 */
		update_paths(mpp, (cmd == CMD_LIST_SHORT));

		if (cmd == CMD_LIST_LONG)
			mpp->bestpg = select_path_group(mpp);

		disassemble_status(status, mpp);

		if (cmd == CMD_LIST_SHORT ||
		    cmd == CMD_LIST_LONG) {
			struct config *conf = get_multipath_config();
			print_multipath_topology(mpp, conf->verbosity);
			put_multipath_config(conf);
		}

		if (cmd == CMD_CREATE)
			reinstate_paths(mpp);
	}

	if (cmd == CMD_LIST_SHORT || cmd == CMD_LIST_LONG) {
		struct config *conf = get_multipath_config();

		print_foreign_topology(conf->verbosity);
		put_multipath_config(conf);
	}

	return 0;
}

static int check_usable_paths(struct config *conf,
			      const char *devpath, enum devtypes dev_type)
{
	struct udev_device *ud = NULL;
	struct multipath *mpp = NULL;
	struct pathgroup *pg;
	struct path *pp;
	char *mapname;
	vector pathvec = NULL;
	char params[PARAMS_SIZE], status[PARAMS_SIZE];
	dev_t devt;
	int r = 1, i, j;

	ud = get_udev_device(devpath, dev_type);
	if (ud == NULL)
		return r;

	devt = udev_device_get_devnum(ud);
	if (!dm_is_dm_major(major(devt))) {
		condlog(1, "%s is not a dm device", devpath);
		goto out;
	}

	mapname = dm_mapname(major(devt), minor(devt));
	if (mapname == NULL) {
		condlog(1, "dm device not found: %s", devpath);
		goto out;
	}

	if (!dm_is_mpath(mapname)) {
		condlog(1, "%s is not a multipath map", devpath);
		goto free;
	}

	/* pathvec is needed for disassemble_map */
	pathvec = vector_alloc();
	if (pathvec == NULL)
		goto free;

	mpp = dm_get_multipath(mapname);
	if (mpp == NULL)
		goto free;

	dm_get_map(mpp->alias, &mpp->size, params);
	dm_get_status(mpp->alias, status);
	disassemble_map(pathvec, params, mpp, 0);
	disassemble_status(status, mpp);

	vector_foreach_slot (mpp->pg, pg, i) {
		vector_foreach_slot (pg->paths, pp, j) {
			pp->udev = get_udev_device(pp->dev_t, DEV_DEVT);
			if (pp->udev == NULL)
				continue;
			if (pathinfo(pp, conf, DI_SYSFS|DI_NOIO) != PATHINFO_OK)
				continue;

			if (pp->state == PATH_UP &&
			    pp->dmstate == PSTATE_ACTIVE) {
				condlog(3, "%s: path %s is usable",
					devpath, pp->dev);
				r = 0;
				goto found;
			}
		}
	}
found:
	condlog(r == 0 ? 3 : 2, "%s:%s usable paths found",
		devpath, r == 0 ? "" : " no");
free:
	FREE(mapname);
	free_multipath(mpp, FREE_PATHS);
	vector_free(pathvec);
out:
	udev_device_unref(ud);
	return r;
}

enum {
	FIND_MULTIPATHS_WAIT_DONE = 0,
	FIND_MULTIPATHS_WAITING = 1,
	FIND_MULTIPATHS_ERROR = -1,
	FIND_MULTIPATHS_NEVER = -2,
};

static const char shm_find_mp_dir[] = MULTIPATH_SHM_BASE "find_multipaths";
static void close_fd(void *arg)
{
	close((long)arg);
}

/**
 * find_multipaths_check_timeout(wwid, tmo)
 * Helper for "find_multipaths smart"
 *
 * @param[in] pp: path to check / record
 * @param[in] tmo: configured timeout for this WWID, or value <= 0 for checking
 * @param[out] until: timestamp until we must wait, CLOCK_REALTIME, if return
 *             value is FIND_MULTIPATHS_WAITING
 * @returns: FIND_MULTIPATHS_WAIT_DONE, if waiting has finished
 * @returns: FIND_MULTIPATHS_ERROR, if internal error occurred
 * @returns: FIND_MULTIPATHS_NEVER, if tmo is 0 and we didn't wait for this
 *           device
 * @returns: FIND_MULTIPATHS_WAITING, if timeout hasn't expired
 */
static int find_multipaths_check_timeout(const struct path *pp, long tmo,
					 struct timespec *until)
{
	char path[PATH_MAX];
	struct timespec now, ftimes[2], tdiff;
	struct stat st;
	long fd;
	int r, err, retries = 0;

	clock_gettime(CLOCK_REALTIME, &now);

	if (snprintf(path, sizeof(path), "%s/%s", shm_find_mp_dir, pp->dev_t)
	    >= sizeof(path)) {
		condlog(1, "%s: path name overflow", __func__);
		return FIND_MULTIPATHS_ERROR;
	}

	if (ensure_directories_exist(path, 0700)) {
		condlog(1, "%s: error creating directories", __func__);
		return FIND_MULTIPATHS_ERROR;
	}

retry:
	fd = open(path, O_RDONLY);
	if (fd != -1) {
		pthread_cleanup_push(close_fd, (void *)fd);
		r = fstat(fd, &st);
		if (r != 0)
			err = errno;
		pthread_cleanup_pop(1);

	} else if (tmo > 0) {
		if (errno == ENOENT)
			fd = open(path, O_RDWR|O_EXCL|O_CREAT, 0644);
		if (fd == -1) {
			if (errno == EEXIST && !retries++)
				/* We could have raced with another process */
				goto retry;
			condlog(1, "%s: error opening %s: %s",
				__func__, path, strerror(errno));
			return FIND_MULTIPATHS_ERROR;
		};

		pthread_cleanup_push(close_fd, (void *)fd);
		/*
		 * We just created the file. Set st_mtim to our desired
		 * expiry time.
		 */
		ftimes[0].tv_sec = 0;
		ftimes[0].tv_nsec = UTIME_OMIT;
		ftimes[1].tv_sec = now.tv_sec + tmo;
		ftimes[1].tv_nsec = now.tv_nsec;
		if (futimens(fd, ftimes) != 0) {
			condlog(1, "%s: error in futimens(%s): %s", __func__,
				path, strerror(errno));
		}
		r = fstat(fd, &st);
		if (r != 0)
			err = errno;
		pthread_cleanup_pop(1);
	} else
		return FIND_MULTIPATHS_NEVER;

	if (r != 0) {
		condlog(1, "%s: error in fstat for %s: %s", __func__,
			path, strerror(err));
		return FIND_MULTIPATHS_ERROR;
	}

	timespecsub(&st.st_mtim, &now, &tdiff);

	if (tdiff.tv_sec <= 0)
		return FIND_MULTIPATHS_WAIT_DONE;

	*until = tdiff;
	return FIND_MULTIPATHS_WAITING;
}

static int print_cmd_valid(int k, const vector pathvec,
			   struct config *conf)
{
	static const int vals[] = { 1, 0, 2 };
	int wait = FIND_MULTIPATHS_NEVER;
	struct timespec until;
	struct path *pp;

	if (k < 0 || k >= sizeof(vals))
		return 1;

	if (k == 2) {
		/*
		 * Caller ensures that pathvec[0] is the path to
		 * examine.
		 */
		pp = VECTOR_SLOT(pathvec, 0);
		select_find_multipaths_timeout(conf, pp);
		wait = find_multipaths_check_timeout(
			pp, pp->find_multipaths_timeout, &until);
		if (wait != FIND_MULTIPATHS_WAITING)
			k = 1;
	} else if (pathvec != NULL && (pp = VECTOR_SLOT(pathvec, 0)))
		wait = find_multipaths_check_timeout(pp, 0, &until);
	if (wait == FIND_MULTIPATHS_WAITING)
		printf("FIND_MULTIPATHS_WAIT_UNTIL=\"%ld.%06ld\"\n",
			       until.tv_sec, until.tv_nsec/1000);
	else if (wait == FIND_MULTIPATHS_WAIT_DONE)
		printf("FIND_MULTIPATHS_WAIT_UNTIL=\"0\"\n");
	printf("DM_MULTIPATH_DEVICE_PATH=\"%d\"\n", vals[k]);
	return k == 1;
}

/*
 * Returns true if this device has been handled before,
 * and released to systemd.
 *
 * This must be called before get_refwwid(),
 * otherwise udev_device_new_from_environment() will have
 * destroyed environ(7).
 */
static bool released_to_systemd(void)
{
	static const char dmdp[] = "DM_MULTIPATH_DEVICE_PATH";
	const char *dm_mp_dev_path = getenv(dmdp);
	bool ret;

	ret = dm_mp_dev_path != NULL && !strcmp(dm_mp_dev_path, "0");
	condlog(4, "%s: %s=%s -> %d", __func__, dmdp, dm_mp_dev_path, ret);
	return ret;
}

/*
 * Return value:
 *  -1: Retry
 *   0: Success
 *   1: Failure
 */
static int
configure (struct config *conf, enum mpath_cmds cmd,
	   enum devtypes dev_type, char *devpath)
{
	vector curmp = NULL;
	vector pathvec = NULL;
	struct vectors vecs;
	int r = 1;
	int di_flag = 0;
	char * refwwid = NULL;
	char * dev = NULL;
	bool released = released_to_systemd();

	/*
	 * allocate core vectors to store paths and multipaths
	 */
	curmp = vector_alloc();
	pathvec = vector_alloc();

	if (!curmp || !pathvec) {
		condlog(0, "can not allocate memory");
		goto out;
	}
	vecs.pathvec = pathvec;
	vecs.mpvec = curmp;

	dev = convert_dev(devpath, (dev_type == DEV_DEVNODE));

	/*
	 * if we have a blacklisted device parameter, exit early
	 */
	if (dev && (dev_type == DEV_DEVNODE ||
		    dev_type == DEV_UEVENT) &&
	    cmd != CMD_REMOVE_WWID &&
	    (filter_devnode(conf->blist_devnode,
			    conf->elist_devnode, dev) > 0)) {
		goto print_valid;
	}

	/*
	 * scope limiting must be translated into a wwid
	 * failing the translation is fatal (by policy)
	 */
	if (devpath) {
		int failed = get_refwwid(cmd, devpath, dev_type,
					 pathvec, &refwwid);
		if (!refwwid) {
			condlog(4, "%s: failed to get wwid", devpath);
			if (failed == 2 && cmd == CMD_VALID_PATH)
				goto print_valid;
			else
				condlog(3, "scope is null");
			goto out;
		}
		if (cmd == CMD_REMOVE_WWID) {
			r = remove_wwid(refwwid);
			if (r == 0)
				printf("wwid '%s' removed\n", refwwid);
			else if (r == 1) {
				printf("wwid '%s' not in wwids file\n",
					refwwid);
				r = 0;
			}
			goto out;
		}
		if (cmd == CMD_ADD_WWID) {
			r = remember_wwid(refwwid);
			if (r >= 0)
				printf("wwid '%s' added\n", refwwid);
			else
				printf("failed adding '%s' to wwids file\n",
				       refwwid);
			goto out;
		}
		condlog(3, "scope limited to %s", refwwid);
		/* If you are ignoring the wwids file and find_multipaths is
		 * set, you need to actually check if there are two available
		 * paths to determine if this path should be multipathed. To
		 * do this, we put off the check until after discovering all
		 * the paths.
		 * Paths listed in the wwids file are always considered valid.
		 */
		if (cmd == CMD_VALID_PATH) {
			if (is_failed_wwid(refwwid) == WWID_IS_FAILED) {
				r = 1;
				goto print_valid;
			}
			if ((!find_multipaths_on(conf) &&
				    ignore_wwids_on(conf)) ||
				   check_wwids_file(refwwid, 0) == 0)
				r = 0;
			if (!ignore_wwids_on(conf))
				goto print_valid;
			/* At this point, either r==0 or find_multipaths_on. */

			/*
			 * Shortcut for find_multipaths smart:
			 * Quick check if path is already multipathed.
			 */
			if (sysfs_is_multipathed(VECTOR_SLOT(pathvec, 0))) {
				r = 0;
				goto print_valid;
			}

			/*
			 * DM_MULTIPATH_DEVICE_PATH=="0" means that we have
			 * been called for this device already, and have
			 * released it to systemd. Unless the device is now
			 * already multipathed (see above), we can't try to
			 * grab it, because setting SYSTEMD_READY=0 would
			 * cause file systems to be unmounted.
			 * Leave DM_MULTIPATH_DEVICE_PATH="0".
			 */
			if (released) {
				r = 1;
				goto print_valid;
			}
			if (r == 0)
				goto print_valid;
			/* find_multipaths_on: Fall through to path detection */
		}
	}

	/*
	 * get a path list
	 */
	if (devpath)
		di_flag = DI_WWID;

	if (cmd == CMD_LIST_LONG)
		/* extended path info '-ll' */
		di_flag |= DI_SYSFS | DI_CHECKER | DI_SERIAL;
	else if (cmd == CMD_LIST_SHORT)
		/* minimum path info '-l' */
		di_flag |= DI_SYSFS;
	else
		/* maximum info */
		di_flag = DI_ALL;

	if (path_discovery(pathvec, di_flag) < 0)
		goto out;

	if (conf->verbosity > 2)
		print_all_paths(pathvec, 1);

	get_path_layout(pathvec, 0);
	foreign_path_layout();

	if (get_dm_mpvec(cmd, curmp, pathvec, refwwid))
		goto out;

	filter_pathvec(pathvec, refwwid);

	if (cmd == CMD_DUMP_CONFIG) {
		vector hwes = get_used_hwes(pathvec);

		dump_config(conf, hwes, curmp);
		vector_free(hwes);
		goto out;
	}

	if (cmd == CMD_VALID_PATH) {
		struct path *pp;
		int fd;

		/* This only happens if find_multipaths and
		 * ignore_wwids is set, and the path is not in WWIDs
		 * file, not currently multipathed, and has
		 * never been released to systemd.
		 * If there is currently a multipath device matching
		 * the refwwid, or there is more than one path matching
		 * the refwwid, then the path is valid */
		if (VECTOR_SIZE(curmp) != 0) {
			r = 0;
			goto print_valid;
		} else if (VECTOR_SIZE(pathvec) > 1)
			r = 0;
		else
			/* Use r=2 as an indication for "maybe" */
			r = 2;

		/*
		 * If opening the path with O_EXCL fails, the path
		 * is in use (e.g. mounted during initramfs processing).
		 * We know that it's not used by dm-multipath.
		 * We may not set SYSTEMD_READY=0 on such devices, it
		 * might cause systemd to umount the device.
		 * Use O_RDONLY, because udevd would trigger another
		 * uevent for close-after-write.
		 *
		 * The O_EXCL check is potentially dangerous, because it may
		 * race with other tasks trying to access the device. Therefore
		 * this code is only executed if the path hasn't been released
		 * to systemd earlier (see above).
		 *
		 * get_refwwid() above stores the path we examine in slot 0.
		 */
		pp = VECTOR_SLOT(pathvec, 0);
		fd = open(udev_device_get_devnode(pp->udev),
			  O_RDONLY|O_EXCL);
		if (fd >= 0)
			close(fd);
		else {
			condlog(3, "%s: path %s is in use: %s",
				__func__, pp->dev,
				strerror(errno));
			/*
			 * Check if we raced with multipathd
			 */
			r = !sysfs_is_multipathed(VECTOR_SLOT(pathvec, 0));
		}
		goto print_valid;
	}

	if (cmd != CMD_CREATE && cmd != CMD_DRY_RUN) {
		r = 0;
		goto out;
	}

	/*
	 * core logic entry point
	 */
	r = coalesce_paths(&vecs, NULL, refwwid,
			   conf->force_reload, cmd);

print_valid:
	if (cmd == CMD_VALID_PATH)
		r = print_cmd_valid(r, pathvec, conf);

out:
	if (refwwid)
		FREE(refwwid);

	free_multipathvec(curmp, KEEP_PATHS);
	free_pathvec(pathvec, FREE_PATHS);

	return r;
}

static int
get_dev_type(char *dev) {
	struct stat buf;
	int i;

	if (stat(dev, &buf) == 0 && S_ISBLK(buf.st_mode)) {
		if (dm_is_dm_major(major(buf.st_rdev)))
			return DEV_DEVMAP;
		return DEV_DEVNODE;
	}
	else if (sscanf(dev, "%d:%d", &i, &i) == 2)
		return DEV_DEVT;
	else if (valid_alias(dev))
		return DEV_DEVMAP;
	return DEV_NONE;
}

/*
 * Some multipath commands are dangerous to run while multipathd is running.
 * For example, "multipath -r" may apply a modified configuration to the kernel,
 * while multipathd is still using the old configuration, leading to
 * inconsistent state.
 *
 * It is safer to use equivalent multipathd client commands instead.
 */
enum {
	DELEGATE_OK = 0,
	DELEGATE_ERROR = -1,
	NOT_DELEGATED = 1,
};

int delegate_to_multipathd(enum mpath_cmds cmd, const char *dev,
			   enum devtypes dev_type, const struct config *conf)
{
	int fd;
	char command[1024], *p, *reply = NULL;
	int n, r = DELEGATE_ERROR;

	p = command;
	*p = '\0';
	n = sizeof(command);

	if (cmd == CMD_CREATE && conf->force_reload == FORCE_RELOAD_YES) {
		p += snprintf(p, n, "reconfigure");
	}
	/* Add other translations here */

	if (strlen(command) == 0)
		/* No command found, no need to delegate */
		return NOT_DELEGATED;

	fd = mpath_connect();
	if (fd == -1)
		return NOT_DELEGATED;

	if (p >= command + sizeof(command)) {
		condlog(0, "internal error - command buffer overflow");
		goto out;
	}

	condlog(3, "delegating command to multipathd");

	if (mpath_process_cmd(fd, command, &reply, conf->uxsock_timeout)
	    == -1) {
		condlog(1, "error in multipath command %s: %s",
			command, strerror(errno));
		goto out;
	}

	if (reply != NULL && *reply != '\0' && strcmp(reply, "ok\n"))
		printf("%s", reply);
	r = DELEGATE_OK;

out:
	FREE(reply);
	close(fd);
	return r;
}

int
main (int argc, char *argv[])
{
	int arg;
	extern char *optarg;
	extern int optind;
	int r = 1;
	enum mpath_cmds cmd = CMD_CREATE;
	enum devtypes dev_type = DEV_NONE;
	char *dev = NULL;
	struct config *conf;
	int retries = -1;

	udev = udev_new();
	logsink = 0;
	conf = load_config(DEFAULT_CONFIGFILE);
	if (!conf)
		exit(1);
	multipath_conf = conf;
	conf->retrigger_tries = 0;
	while ((arg = getopt(argc, argv, ":adcChl::FfM:v:p:b:BrR:itTquUwW")) != EOF ) {
		switch(arg) {
		case 1: printf("optarg : %s\n",optarg);
			break;
		case 'v':
			if (sizeof(optarg) > sizeof(char *) ||
			    !isdigit(optarg[0])) {
				usage (argv[0]);
				exit(1);
			}

			conf->verbosity = atoi(optarg);
			break;
		case 'b':
			conf->bindings_file = strdup(optarg);
			break;
		case 'B':
			conf->bindings_read_only = 1;
			break;
		case 'q':
			conf->allow_queueing = 1;
			break;
		case 'c':
			cmd = CMD_VALID_PATH;
			break;
		case 'C':
			cmd = CMD_USABLE_PATHS;
			break;
		case 'd':
			if (cmd == CMD_CREATE)
				cmd = CMD_DRY_RUN;
			break;
		case 'f':
			conf->remove = FLUSH_ONE;
			break;
		case 'F':
			conf->remove = FLUSH_ALL;
			break;
		case 'l':
			if (optarg && !strncmp(optarg, "l", 1))
				cmd = CMD_LIST_LONG;
			else
				cmd = CMD_LIST_SHORT;

			break;
		case 'M':
#if _DEBUG_
			debug = atoi(optarg);
#endif
			break;
		case 'p':
			conf->pgpolicy_flag = get_pgpolicy_id(optarg);
			if (conf->pgpolicy_flag == IOPOLICY_UNDEF) {
				printf("'%s' is not a valid policy\n", optarg);
				usage(argv[0]);
				exit(1);
			}
			break;
		case 'r':
			conf->force_reload = FORCE_RELOAD_YES;
			break;
		case 'i':
			conf->find_multipaths |= _FIND_MULTIPATHS_I;
			break;
		case 't':
			r = dump_config(conf, NULL, NULL);
			goto out_free_config;
		case 'T':
			cmd = CMD_DUMP_CONFIG;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		case 'u':
			cmd = CMD_VALID_PATH;
			dev_type = DEV_UEVENT;
			break;
		case 'U':
			cmd = CMD_USABLE_PATHS;
			dev_type = DEV_UEVENT;
			break;
		case 'w':
			cmd = CMD_REMOVE_WWID;
			break;
		case 'W':
			cmd = CMD_RESET_WWIDS;
			break;
		case 'a':
			cmd = CMD_ADD_WWID;
			break;
		case 'R':
			retries = atoi(optarg);
			break;
		case ':':
			fprintf(stderr, "Missing option argument\n");
			usage(argv[0]);
			exit(1);
		case '?':
			fprintf(stderr, "Unknown switch: %s\n", optarg);
			usage(argv[0]);
			exit(1);
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (getuid() != 0) {
		fprintf(stderr, "need to be root\n");
		exit(1);
	}

	if (optind < argc) {
		dev = MALLOC(FILE_NAME_SIZE);

		if (!dev)
			goto out;

		strncpy(dev, argv[optind], FILE_NAME_SIZE);
		if (dev_type != DEV_UEVENT)
			dev_type = get_dev_type(dev);
		if (dev_type == DEV_NONE) {
			condlog(0, "'%s' is not a valid argument\n", dev);
			goto out;
		}
	}
	if (dev_type == DEV_UEVENT) {
		openlog("multipath", 0, LOG_DAEMON);
		setlogmask(LOG_UPTO(conf->verbosity + 3));
		logsink = 1;
	}

	if (conf->max_fds) {
		struct rlimit fd_limit;

		fd_limit.rlim_cur = conf->max_fds;
		fd_limit.rlim_max = conf->max_fds;
		if (setrlimit(RLIMIT_NOFILE, &fd_limit) < 0)
			condlog(0, "can't set open fds limit to %d : %s",
				conf->max_fds, strerror(errno));
	}

	libmp_udev_set_sync_support(1);

	if (init_checkers(conf->multipath_dir)) {
		condlog(0, "failed to initialize checkers");
		goto out;
	}
	if (init_prio(conf->multipath_dir)) {
		condlog(0, "failed to initialize prioritizers");
		goto out;
	}
	/* Failing here is non-fatal */
	init_foreign(conf->multipath_dir);
	if (cmd == CMD_USABLE_PATHS) {
		r = check_usable_paths(conf, dev, dev_type);
		goto out;
	}
	if (cmd == CMD_VALID_PATH &&
	    (!dev || dev_type == DEV_DEVMAP)) {
		condlog(0, "the -c option requires a path to check");
		goto out;
	}
	if (cmd == CMD_VALID_PATH &&
	    dev_type == DEV_UEVENT) {
		int fd;

		fd = mpath_connect();
		if (fd == -1) {
			condlog(3, "%s: daemon is not running", dev);
			if (!systemd_service_enabled(dev)) {
				r = print_cmd_valid(1, NULL, conf);
				goto out;
			}
		} else
			mpath_disconnect(fd);
	}

	if (cmd == CMD_REMOVE_WWID && !dev) {
		condlog(0, "the -w option requires a device");
		goto out;
	}

	switch(delegate_to_multipathd(cmd, dev, dev_type, conf)) {
	case DELEGATE_OK:
		exit(0);
	case DELEGATE_ERROR:
		exit(1);
	case NOT_DELEGATED:
		break;
	}

	if (cmd == CMD_RESET_WWIDS) {
		struct multipath * mpp;
		int i;
		vector curmp;

		curmp = vector_alloc();
		if (!curmp) {
			condlog(0, "can't allocate memory for mp list");
			goto out;
		}
		if (dm_get_maps(curmp) == 0)
			r = replace_wwids(curmp);
		if (r == 0)
			printf("successfully reset wwids\n");
		vector_foreach_slot_backwards(curmp, mpp, i) {
			vector_del_slot(curmp, i);
			free_multipath(mpp, KEEP_PATHS);
		}
		vector_free(curmp);
		goto out;
	}
	if (retries < 0)
		retries = conf->remove_retries;
	if (conf->remove == FLUSH_ONE) {
		if (dev_type == DEV_DEVMAP) {
			r = dm_suspend_and_flush_map(dev, retries);
		} else
			condlog(0, "must provide a map name to remove");

		goto out;
	}
	else if (conf->remove == FLUSH_ALL) {
		r = dm_flush_maps(retries);
		goto out;
	}
	while ((r = configure(conf, cmd, dev_type, dev)) < 0)
		condlog(3, "restart multipath configuration process");

out:
	dm_lib_release();
	dm_lib_exit();

	cleanup_foreign();
	cleanup_prio();
	cleanup_checkers();

	/*
	 * multipath -u must exit with status 0, otherwise udev won't
	 * import its output.
	 */
	if (cmd == CMD_VALID_PATH && dev_type == DEV_UEVENT && r == 1)
		r = 0;

	if (dev_type == DEV_UEVENT)
		closelog();

out_free_config:
	/*
	 * Freeing config must be done after dm_lib_exit(), because
	 * the logging function (dm_write_log()), which is called there,
	 * references the config.
	 */
	free_config(conf);
	conf = NULL;
	udev_unref(udev);
	if (dev)
		FREE(dev);
#ifdef _DEBUG_
	dbg_free_final(NULL);
#endif
	return r;
}
