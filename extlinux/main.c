/* ----------------------------------------------------------------------- *
 *
 *   Copyright 1998-2008 H. Peter Anvin - All Rights Reserved
 *   Copyright 2009-2010 Intel Corporation; author: H. Peter Anvin
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 53 Temple Place Ste 330,
 *   Boston MA 02111-1307, USA; either version 2 of the License, or
 *   (at your option) any later version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

/*
 * extlinux.c
 *
 * Install the extlinux boot block on an fat, ext2/3/4 and btrfs filesystem
 */

#define  _GNU_SOURCE		/* Enable everything */
#include <inttypes.h>
/* This is needed to deal with the kernel headers imported into glibc 3.3.3. */
typedef uint64_t u64;
#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#ifndef __KLIBC__
#include <mntent.h>
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sysexits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/vfs.h>

#include "linuxioctl.h"

#include "btrfs.h"
#include "fat.h"
#include "../version.h"
#include "syslxint.h"
#include "syslxcom.h" /* common functions shared with extlinux and syslinux */
#include "setadv.h"
#include "syslxopt.h" /* unified options */

#ifdef DEBUG
# define dprintf printf
#else
# define dprintf(...) ((void)0)
#endif

#if defined(__linux__) && !defined(BLKGETSIZE64)
/* This takes a u64, but the size field says size_t.  Someone screwed big. */
# define BLKGETSIZE64 _IOR(0x12,114,size_t)
#endif

#ifndef EXT2_SUPER_OFFSET
#define EXT2_SUPER_OFFSET 1024
#endif

/* the btrfs partition first 64K blank area is used to store boot sector and
   boot image, the boot sector is from 0~512, the boot image starts at 2K */
#define BTRFS_EXTLINUX_OFFSET (2*1024)
#define BTRFS_SUBVOL_OPT "subvol="
#define BTRFS_SUBVOL_MAX 256	/* By btrfs specification */
static char subvol[BTRFS_SUBVOL_MAX];

/*
 * Boot block
 */
extern unsigned char extlinux_bootsect[];
extern unsigned int extlinux_bootsect_len;
#undef  boot_block
#undef  boot_block_len
#define boot_block	extlinux_bootsect
#define boot_block_len  extlinux_bootsect_len

/*
 * Image file
 */
extern unsigned char extlinux_image[];
extern unsigned int extlinux_image_len;
#undef  boot_image
#undef  boot_image_len
#define boot_image	extlinux_image
#define boot_image_len  extlinux_image_len

#define BTRFS_ADV_OFFSET (BTRFS_EXTLINUX_OFFSET + boot_image_len)

/*
 * Get the size of a block device
 */
uint64_t get_size(int devfd)
{
    uint64_t bytes;
    uint32_t sects;
    struct stat st;

#ifdef BLKGETSIZE64
    if (!ioctl(devfd, BLKGETSIZE64, &bytes))
	return bytes;
#endif
    if (!ioctl(devfd, BLKGETSIZE, &sects))
	return (uint64_t) sects << 9;
    else if (!fstat(devfd, &st) && st.st_size)
	return st.st_size;
    else
	return 0;
}

/*
 * Get device geometry and partition offset
 */
struct geometry_table {
    uint64_t bytes;
    struct hd_geometry g;
};

/* Standard floppy disk geometries, plus LS-120.  Zipdisk geometry
   (x/64/32) is the final fallback.  I don't know what LS-240 has
   as its geometry, since I don't have one and don't know anyone that does,
   and Google wasn't helpful... */
static const struct geometry_table standard_geometries[] = {
    {360 * 1024, {2, 9, 40, 0}},
    {720 * 1024, {2, 9, 80, 0}},
    {1200 * 1024, {2, 15, 80, 0}},
    {1440 * 1024, {2, 18, 80, 0}},
    {1680 * 1024, {2, 21, 80, 0}},
    {1722 * 1024, {2, 21, 80, 0}},
    {2880 * 1024, {2, 36, 80, 0}},
    {3840 * 1024, {2, 48, 80, 0}},
    {123264 * 1024, {8, 32, 963, 0}},	/* LS120 */
    {0, {0, 0, 0, 0}}
};

int get_geometry(int devfd, uint64_t totalbytes, struct hd_geometry *geo)
{
    struct floppy_struct fd_str;
    const struct geometry_table *gp;

    memset(geo, 0, sizeof *geo);

    if (!ioctl(devfd, HDIO_GETGEO, &geo)) {
	return 0;
    } else if (!ioctl(devfd, FDGETPRM, &fd_str)) {
	geo->heads = fd_str.head;
	geo->sectors = fd_str.sect;
	geo->cylinders = fd_str.track;
	geo->start = 0;
	return 0;
    }

    /* Didn't work.  Let's see if this is one of the standard geometries */
    for (gp = standard_geometries; gp->bytes; gp++) {
	if (gp->bytes == totalbytes) {
	    memcpy(geo, &gp->g, sizeof *geo);
	    return 0;
	}
    }

    /* Didn't work either... assign a geometry of 64 heads, 32 sectors; this is
       what zipdisks use, so this would help if someone has a USB key that
       they're booting in USB-ZIP mode. */

    geo->heads = opt.heads ? : 64;
    geo->sectors = opt.sectors ? : 32;
    geo->cylinders = totalbytes / (geo->heads * geo->sectors << SECTOR_SHIFT);
    geo->start = 0;

    if (!opt.sectors && !opt.heads)
	fprintf(stderr,
		"Warning: unable to obtain device geometry (defaulting to %d heads, %d sectors)\n"
		"         (on hard disks, this is usually harmless.)\n",
		geo->heads, geo->sectors);

    return 1;
}

/*
 * Generate sector extents
 */
static void generate_extents(struct syslinux_extent *ex, int nptrs,
			     const sector_t *sectp, int nsect)
{
    uint32_t addr = 0x7c00 + 2*SECTOR_SIZE;
    uint32_t base;
    sector_t sect, lba;
    unsigned int len;

    len = lba = base = 0;

    memset(ex, 0, nptrs * sizeof *ex);

    while (nsect) {
	sect = *sectp++;

	if (len && sect == lba + len &&
	    ((addr ^ (base + len * SECTOR_SIZE)) & 0xffff0000) == 0) {
	    /* We can add to the current extent */
	    len++;
	    goto next;
	}

	if (len) {
	    set_64_sl(&ex->lba, lba);
	    set_16_sl(&ex->len, len);
	    ex++;
	}

	base = addr;
	lba  = sect;
	len  = 1;

    next:
	addr += SECTOR_SIZE;
	nsect--;
    }

    if (len) {
	set_64_sl(&ex->lba, lba);
	set_16_sl(&ex->len, len);
	ex++;
    }
}

/*
 * Form a pointer based on a 16-bit patcharea/epa field
 */
static inline void *ptr(void *img, uint16_t *offset_p)
{
    return (char *)img + get_16_sl(offset_p);
}

/*
 * Query the device geometry and put it into the boot sector.
 * Map the file and put the map in the boot sector and file.
 * Stick the "current directory" inode number into the file.
 *
 * Returns the number of modified bytes in the boot file.
 */
int patch_file_and_bootblock(int fd, const char *dir, int devfd)
{
    struct stat dirst, xdst;
    struct hd_geometry geo;
    sector_t *sectp;
    uint64_t totalbytes, totalsectors;
    int nsect;
    uint32_t *wp;
    struct boot_sector *sbs;
    struct patch_area *patcharea;
    struct ext_patch_area *epa;
    struct syslinux_extent *ex;
    int i, dw, nptrs;
    uint32_t csum;
    char *dirpath, *subpath, *xdirpath, *xsubpath;
    uint64_t *advptrs;

    dirpath = realpath(dir, NULL);
    if (!dirpath || stat(dir, &dirst)) {
	perror("accessing install directory");
	exit(255);		/* This should never happen */
    }

    if (lstat(dirpath, &xdst) ||
	dirst.st_ino != xdst.st_ino ||
	dirst.st_dev != xdst.st_dev) {
	perror("realpath returned nonsense");
	exit(255);
    }

    subpath = strchr(dirpath, '\0');
    for (;;) {
	if (*subpath == '/') {
	    if (subpath > dirpath) {
		*subpath = '\0';
		xsubpath = subpath+1;
		xdirpath = dirpath;
	    } else {
		xsubpath = subpath;
		xdirpath = "/";
	    }
	    if (lstat(xdirpath, &xdst) || dirst.st_dev != xdst.st_dev) {
		subpath = strchr(subpath+1, '/');
		if (!subpath)
		    subpath = "/"; /* It's the root of the filesystem */
		break;
	    }
	    *subpath = '/';
	}

	if (subpath == dirpath)
	    break;

	subpath--;
    }

    /* Now subpath should contain the path relative to the fs base */
    dprintf("subpath = %s\n", subpath);

    totalbytes = get_size(devfd);
    get_geometry(devfd, totalbytes, &geo);

    if (opt.heads)
	geo.heads = opt.heads;
    if (opt.sectors)
	geo.sectors = opt.sectors;

    /* Patch this into a fake FAT superblock.  This isn't because
       FAT is a good format in any way, it's because it lets the
       early bootstrap share code with the FAT version. */
    dprintf("heads = %u, sect = %u\n", geo.heads, geo.sectors);

    sbs = (struct boot_sector *)boot_block;

    totalsectors = totalbytes >> SECTOR_SHIFT;
    if (totalsectors >= 65536) {
	set_16(&sbs->bsSectors, 0);
    } else {
	set_16(&sbs->bsSectors, totalsectors);
    }
    set_32(&sbs->bsHugeSectors, totalsectors);

    set_16(&sbs->bsBytesPerSec, SECTOR_SIZE);
    set_16(&sbs->bsSecPerTrack, geo.sectors);
    set_16(&sbs->bsHeads, geo.heads);
    set_32(&sbs->bsHiddenSecs, geo.start);

    /* Construct the boot file */

    dprintf("directory inode = %lu\n", (unsigned long)dirst.st_ino);
    nsect = (boot_image_len + SECTOR_SIZE - 1) >> SECTOR_SHIFT;
    nsect += 2;			/* Two sectors for the ADV */
    sectp = alloca(sizeof(sector_t) * nsect);
    if (fs_type == EXT2 || fs_type == VFAT) {
	if (sectmap(fd, sectp, nsect)) {
		perror("bmap");
		exit(1);
	}
    } else if (fs_type == BTRFS) {
	int i;

	for (i = 0; i < nsect; i++)
		*(sectp + i) = BTRFS_EXTLINUX_OFFSET/SECTOR_SIZE + i;
    }

    /* Search for LDLINUX_MAGIC to find the patch area */
    for (wp = (uint32_t *) boot_image; get_32_sl(wp) != LDLINUX_MAGIC;
	 wp++)
	;
    patcharea = (struct patch_area *)wp;
    epa = ptr(boot_image, &patcharea->epaoffset);

    /* First sector need pointer in boot sector */
    set_32(ptr(sbs, &epa->sect1ptr0), sectp[0]);
    set_32(ptr(sbs, &epa->sect1ptr1), sectp[0] >> 32);
    sectp++;

    /* Handle RAID mode */
    if (opt.raid_mode) {
	/* Patch in INT 18h = CD 18 */
	set_16(ptr(sbs, &epa->raidpatch), 0x18CD);
    }

    /* Set up the totals */
    dw = boot_image_len >> 2;	/* COMPLETE dwords, excluding ADV */
    set_16_sl(&patcharea->data_sectors, nsect - 2); /* Not including ADVs */
    set_16_sl(&patcharea->adv_sectors, 2);	/* ADVs need 2 sectors */
    set_32_sl(&patcharea->dwords, dw);

    /* Stupid mode? */
    if (opt.stupid_mode) {
	/* Access only one sector at a time */
	set_16_sl(&patcharea->maxtransfer, 1);
    }

    /* Set the sector extents */
    ex = ptr(boot_image, &epa->secptroffset);
    nptrs = get_16_sl(&epa->secptrcnt);

    if (nsect > nptrs) {
	/* Not necessarily an error in this case, but a general problem */
	fprintf(stderr, "Insufficient extent space, build error!\n");
	exit(1);
    }

    /* -1 for the pointer in the boot sector, -2 for the two ADVs */
    generate_extents(ex, nptrs, sectp, nsect-1-2);

    /* ADV pointers */
    advptrs = ptr(boot_image, &epa->advptroffset);
    set_64_sl(&advptrs[0], sectp[nsect-1-2]);
    set_64_sl(&advptrs[1], sectp[nsect-1-1]);

    /* Poke in the base directory path */
    if (subpath) {
	int sublen = strlen(subpath) + 1;
	if (get_16_sl(&epa->dirlen) < sublen) {
	    fprintf(stderr, "Subdirectory path too long... aborting install!\n");
	    exit(1);
	}
	memcpy_to_sl(ptr(boot_image, &epa->diroffset), subpath, sublen);
    }
    free(dirpath);

    /* Poke in the subvolume information */
    if (1 /* subvol */) {
	int sublen = strlen(subvol) + 1;
	if (get_16_sl(&epa->subvollen) < sublen) {
	    fprintf(stderr, "Subvol name too long... aborting install!\n");
	    exit(1);
	}
	memcpy_to_sl(ptr(boot_image, &epa->subvoloffset), subvol, sublen);
    }

    /* Now produce a checksum */
    set_32_sl(&patcharea->checksum, 0);

    csum = LDLINUX_MAGIC;
    for (i = 0, wp = (uint32_t *) boot_image; i < dw; i++, wp++)
	csum -= get_32_sl(wp);	/* Negative checksum */

    set_32_sl(&patcharea->checksum, csum);

    /*
     * Assume all bytes modified.  This can be optimized at the expense
     * of keeping track of what the highest modified address ever was.
     */
    return dw << 2;
}

/*
 * Make any user-specified ADV modifications
 */
int modify_adv(void)
{
    int rv = 0;

    if (opt.set_once) {
	if (syslinux_setadv(ADV_BOOTONCE, strlen(opt.set_once), opt.set_once)) {
	    fprintf(stderr, "%s: not enough space for boot-once command\n",
		    program);
	    rv = -1;
	}
    }
    if (opt.menu_save) {
        if (syslinux_setadv(ADV_MENUSAVE, strlen(opt.menu_save), opt.menu_save)) {
	    fprintf(stderr, "%s: not enough space for menu-save label\n",
		    program);
	    rv = -1;
        }
    }

    return rv;
}

/*
 * Install the boot block on the specified device.
 * Must be run AFTER install_file()!
 */
int install_bootblock(int fd, const char *device)
{
    struct ext2_super_block sb;
    struct btrfs_super_block sb2;
    struct boot_sector sb3;
    bool ok = false;

    if (fs_type == EXT2) {
	if (xpread(fd, &sb, sizeof sb, EXT2_SUPER_OFFSET) != sizeof sb) {
		perror("reading superblock");
		return 1;
	}
	if (sb.s_magic == EXT2_SUPER_MAGIC)
		ok = true;
    } else if (fs_type == BTRFS) {
	if (xpread(fd, &sb2, sizeof sb2, BTRFS_SUPER_INFO_OFFSET)
			!= sizeof sb2) {
		perror("reading superblock");
		return 1;
	}
	if (sb2.magic == *(u64 *)BTRFS_MAGIC)
		ok = true;
    } else if (fs_type == VFAT) {
	if (xpread(fd, &sb3, sizeof sb3, 0) != sizeof sb3) {
		perror("reading fat superblock");
		return 1;
	}
	if (sb3.bsResSectors && sb3.bsFATs &&
	    (strstr(sb3.bs16.FileSysType, "FAT") ||
	     strstr(sb3.bs32.FileSysType, "FAT")))
		ok = true;
    }
    if (!ok) {
	fprintf(stderr, "no fat, ext2/3/4 or btrfs superblock found on %s\n",
			device);
	return 1;
    }
    if (fs_type == VFAT) {
	struct boot_sector *sbs = (struct boot_sector *)extlinux_bootsect;
        if (xpwrite(fd, &sbs->bsHead, bsHeadLen, 0) != bsHeadLen ||
	    xpwrite(fd, &sbs->bsCode, bsCodeLen,
		    offsetof(struct boot_sector, bsCode)) != bsCodeLen) {
	    perror("writing fat bootblock");
	    return 1;
	}
    } else {
	if (xpwrite(fd, boot_block, boot_block_len, 0) != boot_block_len) {
	    perror("writing bootblock");
	    return 1;
	}
    }

    return 0;
}

int ext2_fat_install_file(const char *path, int devfd, struct stat *rst)
{
    char *file;
    int fd = -1, dirfd = -1;
    int modbytes;

    asprintf(&file, "%s%sextlinux.sys",
	     path, path[0] && path[strlen(path) - 1] == '/' ? "" : "/");
    if (!file) {
	perror(program);
	return 1;
    }

    dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
	perror(path);
	goto bail;
    }

    fd = open(file, O_RDONLY);
    if (fd < 0) {
	if (errno != ENOENT) {
	    perror(file);
	    goto bail;
	}
    } else {
	clear_attributes(fd);
    }
    close(fd);

    fd = open(file, O_WRONLY | O_TRUNC | O_CREAT | O_SYNC,
	      S_IRUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
	perror(file);
	goto bail;
    }

    /* Write it the first time */
    if (xpwrite(fd, boot_image, boot_image_len, 0) != boot_image_len ||
	xpwrite(fd, syslinux_adv, 2 * ADV_SIZE,
		boot_image_len) != 2 * ADV_SIZE) {
	fprintf(stderr, "%s: write failure on %s\n", program, file);
	goto bail;
    }

    /* Map the file, and patch the initial sector accordingly */
    modbytes = patch_file_and_bootblock(fd, path, devfd);

    /* Write the patch area again - this relies on the file being
       overwritten in place! */
    if (xpwrite(fd, boot_image, modbytes, 0) != modbytes) {
	fprintf(stderr, "%s: write failure on %s\n", program, file);
	goto bail;
    }

    /* Attempt to set immutable flag and remove all write access */
    /* Only set immutable flag if file is owned by root */
    set_attributes(fd);

    if (fstat(fd, rst)) {
	perror(file);
	goto bail;
    }

    close(dirfd);
    close(fd);
    return 0;

bail:
    if (dirfd >= 0)
	close(dirfd);
    if (fd >= 0)
	close(fd);

    return 1;
}

/* btrfs has to install the extlinux.sys in the first 64K blank area, which
   is not managered by btrfs tree, so actually this is not installed as files.
   since the cow feature of btrfs will move the extlinux.sys every where */
int btrfs_install_file(const char *path, int devfd, struct stat *rst)
{
    patch_file_and_bootblock(-1, path, devfd);
    if (xpwrite(devfd, boot_image, boot_image_len, BTRFS_EXTLINUX_OFFSET)
		!= boot_image_len) {
	perror("writing bootblock");
	return 1;
    }
    printf("write boot_image to 0x%x\n", BTRFS_EXTLINUX_OFFSET);
    if (xpwrite(devfd, syslinux_adv, 2 * ADV_SIZE,
		BTRFS_EXTLINUX_OFFSET + boot_image_len) != 2 * ADV_SIZE) {
	perror("writing adv");
	return 1;
    }
    printf("write adv to 0x%x\n", BTRFS_EXTLINUX_OFFSET + boot_image_len);
    if (stat(path, rst)) {
	perror(path);
	return 1;
    }
    return 0;
}

int install_file(const char *path, int devfd, struct stat *rst)
{
	if (fs_type == EXT2 || fs_type == VFAT)
		return ext2_fat_install_file(path, devfd, rst);
	else if (fs_type == BTRFS)
		return btrfs_install_file(path, devfd, rst);
	return 1;
}

/* EXTLINUX installs the string 'EXTLINUX' at offset 3 in the boot
   sector; this is consistent with FAT filesystems. */
int already_installed(int devfd)
{
    char buffer[8];

    xpread(devfd, buffer, 8, 3);
    return !memcmp(buffer, "EXTLINUX", 8);
}

#ifdef __KLIBC__
static char devname_buf[64];

static void device_cleanup(void)
{
    unlink(devname_buf);
}
#endif

/* Verify that a device fd and a pathname agree.
   Return 0 on valid, -1 on error. */
static int validate_device(const char *path, int devfd)
{
    struct stat pst, dst;
    struct statfs sfs;

    if (stat(path, &pst) || fstat(devfd, &dst) || statfs(path, &sfs))
	return -1;
    /* btrfs st_dev is not matched with mnt st_rdev, it is a known issue */
    if (fs_type == BTRFS && sfs.f_type == BTRFS_SUPER_MAGIC)
	return 0;
    return (pst.st_dev == dst.st_rdev) ? 0 : -1;
}

#ifndef __KLIBC__
static const char *find_device(const char *mtab_file, dev_t dev)
{
    struct mntent *mnt;
    struct stat dst;
    FILE *mtab;
    const char *devname = NULL;
    bool done;

    mtab = setmntent(mtab_file, "r");
    if (!mtab)
	return NULL;

    done = false;
    while ((mnt = getmntent(mtab))) {
	/* btrfs st_dev is not matched with mnt st_rdev, it is a known issue */
	switch (fs_type) {
	case BTRFS:
		if (!strcmp(mnt->mnt_type, "btrfs") &&
		    !stat(mnt->mnt_dir, &dst) &&
		    dst.st_dev == dev) {
		    char *opt = strstr(mnt->mnt_opts, BTRFS_SUBVOL_OPT);

		    if (opt) {
			if (!subvol[0]) {
			    char *tmp;

			    strcpy(subvol, opt + sizeof(BTRFS_SUBVOL_OPT) - 1);
			    tmp = strchr(subvol, 32);
			    if (tmp)
				*tmp = '\0';
			}
			break; /* should break and let upper layer try again */
		    } else
			done = true;
		}
		break;
	case EXT2:
		if ((!strcmp(mnt->mnt_type, "ext2") ||
		     !strcmp(mnt->mnt_type, "ext3") ||
		     !strcmp(mnt->mnt_type, "ext4")) &&
		    !stat(mnt->mnt_fsname, &dst) &&
		    dst.st_rdev == dev) {
		    done = true;
		    break;
		}
	case VFAT:
		if ((!strcmp(mnt->mnt_type, "vfat")) &&
		    !stat(mnt->mnt_fsname, &dst) &&
		    dst.st_rdev == dev) {
		    done = true;
		    break;
		}
	case NONE:
	    break;
	}
	if (done) {
		devname = strdup(mnt->mnt_fsname);
		break;
	}
    }
    endmntent(mtab);

    return devname;
}
#endif

static const char *get_devname(const char *path)
{
    const char *devname = NULL;
    struct stat st;
    struct statfs sfs;

    if (stat(path, &st) || !S_ISDIR(st.st_mode)) {
	fprintf(stderr, "%s: Not a directory: %s\n", program, path);
	return devname;
    }
    if (statfs(path, &sfs)) {
	fprintf(stderr, "%s: statfs %s: %s\n", program, path, strerror(errno));
	return devname;
    }
#ifdef __KLIBC__

    /* klibc doesn't have getmntent and friends; instead, just create
       a new device with the appropriate device type */
    snprintf(devname_buf, sizeof devname_buf, "/tmp/dev-%u:%u",
	     major(st.st_dev), minor(st.st_dev));

    if (mknod(devname_buf, S_IFBLK | 0600, st.st_dev)) {
	fprintf(stderr, "%s: cannot create device %s\n", program, devname);
	return devname;
    }

    atexit(device_cleanup);	/* unlink the device node on exit */
    devname = devname_buf;

#else

    /* check /etc/mtab first, since btrfs subvol info is only in here */
    devname = find_device("/etc/mtab", st.st_dev);
    if (subvol[0] && !devname) { /* we just find it is a btrfs subvol */
	char parent[256];
	char *tmp;

	strcpy(parent, path);
	tmp = strrchr(parent, '/');
	if (tmp) {
	    *tmp = '\0';
	    fprintf(stderr, "%s is subvol, try its parent dir %s\n", path, parent);
	    devname = get_devname(parent);
	} else
	    devname = NULL;
    }
    if (!devname) {
	/* Didn't find it in /etc/mtab, try /proc/mounts */
	devname = find_device("/proc/mounts", st.st_dev);
    }
    if (!devname) {
	fprintf(stderr, "%s: cannot find device for path %s\n", program, path);
	return devname;
    }

    fprintf(stderr, "%s is device %s\n", path, devname);
#endif
    return devname;
}

static int open_device(const char *path, struct stat *st, const char **_devname)
{
    int devfd;
    const char *devname = NULL;
    struct statfs sfs;

    if (st)
	if (stat(path, st) || !S_ISDIR(st->st_mode)) {
		fprintf(stderr, "%s: Not a directory: %s\n", program, path);
		return -1;
	}

    if (statfs(path, &sfs)) {
	fprintf(stderr, "%s: statfs %s: %s\n", program, path, strerror(errno));
	return -1;
    }
    if (sfs.f_type == EXT2_SUPER_MAGIC)
	fs_type = EXT2;
    else if (sfs.f_type == BTRFS_SUPER_MAGIC)
	fs_type = BTRFS;
    else if (sfs.f_type == MSDOS_SUPER_MAGIC)
	fs_type = VFAT;

    if (!fs_type) {
	fprintf(stderr, "%s: not a fat, ext2/3/4 or btrfs filesystem: %s\n",
		program, path);
	return -1;
    }

    devfd = -1;
    devname = get_devname(path);
    if (_devname)
	*_devname = devname;

    if ((devfd = open(devname, O_RDWR | O_SYNC)) < 0) {
	fprintf(stderr, "%s: cannot open device %s\n", program, devname);
	return -1;
    }

    /* Verify that the device we opened is the device intended */
    if (validate_device(path, devfd)) {
	fprintf(stderr, "%s: path %s doesn't match device %s\n",
		program, path, devname);
	close(devfd);
	return -1;
    }
    return devfd;
}

static int ext_read_adv(const char *path, const char *cfg, int devfd)
{
    if (fs_type == BTRFS) { /* btrfs "extlinux.sys" is in 64k blank area */
	if (xpread(devfd, syslinux_adv, 2 * ADV_SIZE,
		BTRFS_ADV_OFFSET) != 2 * ADV_SIZE) {
		perror("btrfs writing adv");
		return 1;
	}
	return 0;
    }
    return read_adv(path, cfg);
}

static int ext_write_adv(const char *path, const char *cfg, int devfd)
{
    if (fs_type == BTRFS) { /* btrfs "extlinux.sys" is in 64k blank area */
	if (xpwrite(devfd, syslinux_adv, 2 * ADV_SIZE,
		BTRFS_ADV_OFFSET) != 2 * ADV_SIZE) {
		perror("writing adv");
		return 1;
	}
	return 0;
    }
    return write_adv(path, cfg);
}

int install_loader(const char *path, int update_only)
{
    struct stat st, fst;
    int devfd, rv;
    const char *devname;

    devfd = open_device(path, &st, &devname);
    if (devfd < 0)
	return 1;

    if (update_only && !already_installed(devfd)) {
	fprintf(stderr, "%s: no previous extlinux boot sector found\n",
		program);
	close(devfd);
	return 1;
    }

    /* Read a pre-existing ADV, if already installed */
    if (opt.reset_adv)
	syslinux_reset_adv(syslinux_adv);
    else if (ext_read_adv(path, "extlinux.sys", devfd) < 0) {
	close(devfd);
	return 1;
    }
    if (modify_adv() < 0) {
	close(devfd);
	return 1;
    }

    /* Install extlinux.sys */
    if (install_file(path, devfd, &fst)) {
	close(devfd);
	return 1;
    }
    if (fst.st_dev != st.st_dev) {
	fprintf(stderr, "%s: file system changed under us - aborting!\n",
		program);
	close(devfd);
	return 1;
    }

    sync();
    rv = install_bootblock(devfd, devname);
    close(devfd);
    sync();

    return rv;
}

/*
 * Modify the ADV of an existing installation
 */
int modify_existing_adv(const char *path)
{
    int devfd;

    devfd = open_device(path, NULL, NULL);
    if (devfd < 0)
	return 1;

    if (opt.reset_adv)
	syslinux_reset_adv(syslinux_adv);
    else if (ext_read_adv(path, "extlinux.sys", devfd) < 0) {
	close(devfd);
	return 1;
    }
    if (modify_adv() < 0) {
	close(devfd);
	return 1;
    }
    if (ext_write_adv(path, "extlinux.sys", devfd) < 0) {
	close(devfd);
	return 1;
    }
    close(devfd);
    return 0;
}

int main(int argc, char *argv[])
{
    parse_options(argc, argv, MODE_EXTLINUX);

    if (!opt.directory)
	usage(EX_USAGE, 0);

    if (opt.update_only == -1) {
	if (opt.reset_adv || opt.set_once || opt.menu_save)
	    return modify_existing_adv(opt.directory);
	else
	    usage(EX_USAGE, MODE_EXTLINUX);
    }

    return install_loader(opt.directory, opt.update_only);
}
