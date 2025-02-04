/*
 * (C) Copyright 2022 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */

#include <common.h>
#include <malloc.h>
#include <mmc.h>
#include <dm/device.h>

/*
 * Script file example:
 **************************
 * > # script file start
 * > echo "hello world"
 * > % script file end
 **************************
 */
#define SCRIPT_FILE_MAX_SIZE	(12 * 1024)
#define SCRIPT_FILE_COMMENT	'#'
#define SCRIPT_FILE_END		'%'
#define IS_COMMENT(x)		(SCRIPT_FILE_COMMENT == (x))
#define IS_FILE_END(x)		(SCRIPT_FILE_END == (x))
#define IS_LINE_END(x)		('\r' == (x) || '\n' == (x))
#define MAX_LINE_SIZE		8000

static char *script_next_line(char **line_buf_ptr)
{
	char *line_buf = (*line_buf_ptr);
	char *next_line;
	int i = 0;

	/* strip '\r', '\n' and comment */
	while (1) {
		/* strip '\r' & '\n' */
		if (IS_LINE_END(line_buf[0])) {
			line_buf++;
		/* strip comment */
		} else if (IS_COMMENT(line_buf[0])) {
			for (i = 0; !IS_LINE_END(line_buf[0]) && i <= MAX_LINE_SIZE; i++)
				line_buf++;

			if (i > MAX_LINE_SIZE) {
				line_buf[0] = SCRIPT_FILE_END;
				printf("Error: max line length is %d!!!\n", MAX_LINE_SIZE);
				break;
			}
		} else {
			break;
		}
	}

	/* get next line */
	if (IS_FILE_END(line_buf[0])) {
		next_line = NULL;
	} else {
		next_line = line_buf;
		for (i = 0; !IS_LINE_END(line_buf[0]) && i <= MAX_LINE_SIZE; i++)
			line_buf++;

		if (i > MAX_LINE_SIZE) {
			next_line = NULL;
			printf("Error: max line length is %d!!!\n", MAX_LINE_SIZE);
		} else {
			line_buf[0] = '\0';
			*line_buf_ptr = line_buf + 1;
		}
	}

	return next_line;
}

static int do_script(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	char *next_line, *script, *buf;
	ulong addr;
	int ret = CMD_RET_SUCCESS;

#ifdef CONFIG_FIT_SIGNATURE
	printf("Verify-boot: forbit no-signed script\n");
	return CMD_RET_FAILURE;
#endif

	if (argc != 2 || !argv[1])
		return CMD_RET_USAGE;

	addr = simple_strtoul(argv[1], NULL, 16);
	if (!addr)
		return CMD_RET_USAGE;

	buf = calloc(SCRIPT_FILE_MAX_SIZE, 1);
	if (!buf)
		return CMD_RET_FAILURE;

	script = buf;
	memcpy(buf, (char *)addr, SCRIPT_FILE_MAX_SIZE);
	while ((next_line = script_next_line(&script)) != NULL) {
		printf("\n$ %s\n", next_line);
		ret = run_command(next_line, 0);
		if (ret)
			break;	/* fail */
	}
	free(buf);

	return ret;
}

static int do_sd_update(cmd_tbl_t *cmdtp, int flag,
			int argc, char * const argv[])
{
	struct blk_desc *desc;
	int part_type;
	char cmd[128];
	char *buf;
	int ret;

	printf("## retrieving sd_update.txt ...\n");

#if defined(CONFIG_MMC_DW_ROCKCHIP)
	/* If not boot from mmc devices, init mmc devices first. */
	ret = mmc_initialize(gd->bd);
	if (ret) {
		printf("Could not initialize mmc. error: %d\n", ret);
		return ret;
	}
#endif

	desc = blk_get_devnum_by_type(IF_TYPE_MMC, 1);
	if (!desc)
		return CMD_RET_FAILURE;

	buf = memalign(ARCH_DMA_MINALIGN, SCRIPT_FILE_MAX_SIZE * 2);
	if (!buf)
		return CMD_RET_FAILURE;

	part_type = desc->part_type;
	desc->part_type = PART_TYPE_DOS;

	snprintf(cmd, sizeof(cmd),
		 "fatload mmc 1 0x%08lx sd_update.txt", (ulong)buf);
	ret = run_command(cmd, 0);
	if (!ret) {
		snprintf(cmd, sizeof(cmd), "script 0x%08lx", (ulong)buf);
		ret = run_command(cmd, 0);
	}
	free(buf);

	desc->part_type = part_type;

	return ret;
}

static int do_usb_update(cmd_tbl_t *cmdtp, int flag,
			 int argc, char * const argv[])
{
	struct blk_desc *desc;
	struct udevice *dev;
	int devnum = -1;
	int part_type;
	char cmd[128];
	char *buf;
	int ret;

	printf("## retrieving usb_update.txt ...\n");

	if (run_command("usb reset", 0))
		return CMD_RET_FAILURE;

	for (blk_first_device(IF_TYPE_USB, &dev);
	     dev;
	     blk_next_device(&dev)) {
		desc = dev_get_uclass_platdata(dev);
		if (desc->type == DEV_TYPE_UNKNOWN)
			continue;

		if (desc->lba > 0L && desc->blksz > 0L) {
			devnum = desc->devnum;
			break;
		}
	}
	if (devnum < 0) {
		printf("No available udisk\n");
		return CMD_RET_FAILURE;
	}

	desc = blk_get_devnum_by_type(IF_TYPE_USB, devnum);
	if (!desc) {
		printf("No usb %d found\n", devnum);
		return CMD_RET_FAILURE;
	}

	buf = memalign(ARCH_DMA_MINALIGN, SCRIPT_FILE_MAX_SIZE * 2);
	if (!buf)
		return CMD_RET_FAILURE;

	part_type = desc->part_type;
	desc->part_type = PART_TYPE_DOS;

	printf("## scanning usb %d\n", devnum);
	snprintf(cmd, sizeof(cmd), "fatload usb %d 0x%08lx usb_update.txt",
		 devnum, (ulong)buf);
	ret = run_command(cmd, 0);
	if (!ret) {
		snprintf(cmd, sizeof(cmd), "script 0x%08lx", (ulong)buf);
		ret = run_command(cmd, 0);
	}
	free(buf);
	desc->part_type = part_type;

	return ret;
}

static int do_tftp_update(cmd_tbl_t *cmdtp, int flag,
			  int argc, char * const argv[])
{
	char cmd[128];
	char *buf;
	int dhcp = 0;
	int ret;

	printf("## retrieving tftp_update.txt ...\n");

	if ((argc > 1) && !strcmp(argv[1], "-d"))
		dhcp = 1;

	buf = memalign(ARCH_DMA_MINALIGN, SCRIPT_FILE_MAX_SIZE * 2);
	if (!buf)
		return CMD_RET_FAILURE;

	if (dhcp)
		run_command("dhcp", 0);

	snprintf(cmd, sizeof(cmd), "tftp 0x%08lx tftp_update.txt", (ulong)buf);
	ret = run_command(cmd, 0);
	if (!ret) {
		snprintf(cmd, sizeof(cmd), "script 0x%08lx", (ulong)buf);
		ret = run_command(cmd, 0);
	}
	free(buf);

	return ret;
}

U_BOOT_CMD(
	script, 2, 1, do_script,
	"Run a script", "[file addr]"
);

U_BOOT_CMD(
	sd_update, 1, 1, do_sd_update,
	"sdcard auto upgrade", ""
);

U_BOOT_CMD(
	usb_update, 1, 1, do_usb_update,
	"usb auto upgrade", ""
);

U_BOOT_CMD(
	tftp_update, 2, 1, do_tftp_update,
	"tftp auto upgrade", "[-d]"
);

