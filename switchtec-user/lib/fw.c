/*
 * Microsemi Switchtec(tm) PCIe Management Library
 * Copyright (c) 2017, Microsemi Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "switchtec_priv.h"
#include "switchtec/switchtec.h"
#include "switchtec/errors.h"

#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/switchtec_ioctl.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

int switchtec_fw_dlstatus(struct switchtec_dev *dev,
			  enum switchtec_fw_dlstatus *status,
			  enum mrpc_bg_status *bgstatus)
{
	uint32_t subcmd = MRPC_FWDNLD_GET_STATUS;
	struct {
		uint8_t dlstatus;
		uint8_t bgstatus;
		uint16_t reserved;
	} result;
	int ret;

	ret = switchtec_cmd(dev, MRPC_FWDNLD, &subcmd, sizeof(subcmd),
			    &result, sizeof(result));

	if (ret < 0)
		return ret;

	if (status != NULL)
		*status = result.dlstatus;

	if (bgstatus != NULL)
		*bgstatus = result.bgstatus;

	return 0;
}

int switchtec_fw_wait(struct switchtec_dev *dev,
		      enum switchtec_fw_dlstatus *status)
{
	enum mrpc_bg_status bgstatus;
	int ret;

	do {
		// Delay slightly to avoid interrupting the firmware too much
		usleep(5000);

		ret = switchtec_fw_dlstatus(dev, status, &bgstatus);
		if (ret < 0)
			return ret;
		if (bgstatus == MRPC_BG_STAT_ERROR)
			return SWITCHTEC_DLSTAT_HARDWARE_ERR;

	} while (bgstatus == MRPC_BG_STAT_INPROGRESS);

	return 0;
}

int switchtec_fw_toggle_active_partition(struct switchtec_dev *dev,
					 int toggle_fw, int toggle_cfg)
{
	struct {
		uint8_t subcmd;
		uint8_t toggle_fw;
		uint8_t toggle_cfg;
	} cmd;

	cmd.subcmd = MRPC_FWDNLD_TOGGLE;
	cmd.toggle_fw = !!toggle_fw;
	cmd.toggle_cfg = !!toggle_cfg;

	return switchtec_cmd(dev, MRPC_FWDNLD, &cmd, sizeof(cmd),
			     NULL, 0);
}

struct cmd_fwdl {
	struct cmd_fwdl_hdr {
		uint8_t subcmd;
		uint8_t dont_activate;
		uint8_t reserved[2];
		uint32_t offset;
		uint32_t img_length;
		uint32_t blk_length;
	} hdr;
	uint8_t data[MRPC_MAX_DATA_LEN - sizeof(struct cmd_fwdl_hdr)];
};

int switchtec_fw_write_fd(struct switchtec_dev *dev, int img_fd,
			  int dont_activate, int force,
			  void (*progress_callback)(int cur, int tot))
{
	enum switchtec_fw_dlstatus status;
	enum mrpc_bg_status bgstatus;
	ssize_t image_size, offset = 0;
	int ret;
	struct cmd_fwdl cmd = {};

	image_size = lseek(img_fd, 0, SEEK_END);
	if (image_size < 0)
		return -errno;
	lseek(img_fd, 0, SEEK_SET);

	switchtec_fw_dlstatus(dev, &status, &bgstatus);

	if (!force && status == SWITCHTEC_DLSTAT_INPROGRESS) {
		errno = EBUSY;
		return -EBUSY;
	}

	if (bgstatus == MRPC_BG_STAT_INPROGRESS) {
		errno = EBUSY;
		return -EBUSY;
	}

	cmd.hdr.subcmd = MRPC_FWDNLD_DOWNLOAD;
	cmd.hdr.dont_activate = !!dont_activate;
	cmd.hdr.img_length = htole32(image_size);

	while (offset < image_size) {
		ssize_t blklen = read(img_fd, &cmd.data,
				      sizeof(cmd.data));

		if (blklen == -EAGAIN || blklen == -EWOULDBLOCK)
			continue;

		if (blklen < 0)
			return -errno;

		if (blklen == 0)
			break;

		cmd.hdr.offset = htole32(offset);
		cmd.hdr.blk_length = htole32(blklen);

		ret = switchtec_cmd(dev, MRPC_FWDNLD, &cmd, sizeof(cmd),
				    NULL, 0);

		if (ret < 0)
			return ret;

		ret = switchtec_fw_wait(dev, &status);
		if (ret < 0)
		    return ret;

		offset += cmd.hdr.blk_length;

		if (progress_callback)
			progress_callback(offset, image_size);

	}

	if (status == SWITCHTEC_DLSTAT_COMPLETES)
		return 0;

	if (status == SWITCHTEC_DLSTAT_SUCCESS_FIRM_ACT)
		return 0;

	if (status == SWITCHTEC_DLSTAT_SUCCESS_DATA_ACT)
		return 0;

	if (status == 0)
		return SWITCHTEC_DLSTAT_HARDWARE_ERR;

	return status;
}

int switchtec_fw_write_file(struct switchtec_dev *dev, FILE *fimg,
			    int dont_activate, int force,
			    void (*progress_callback)(int cur, int tot))
{
	enum switchtec_fw_dlstatus status;
	enum mrpc_bg_status bgstatus;
	ssize_t image_size, offset = 0;
	int ret;
	struct cmd_fwdl cmd = {};

	ret = fseek(fimg, 0, SEEK_END);
	if (ret)
		return -errno;
	image_size = ftell(fimg);
	if (image_size < 0)
		return -errno;
	ret = fseek(fimg, 0, SEEK_SET);
	if (ret)
		return -errno;

	switchtec_fw_dlstatus(dev, &status, &bgstatus);

	if (!force && status == SWITCHTEC_DLSTAT_INPROGRESS) {
		errno = EBUSY;
		return -EBUSY;
	}

	if (bgstatus == MRPC_BG_STAT_INPROGRESS) {
		errno = EBUSY;
		return -EBUSY;
	}

	cmd.hdr.subcmd = MRPC_FWDNLD_DOWNLOAD;
	cmd.hdr.dont_activate = !!dont_activate;
	cmd.hdr.img_length = htole32(image_size);

	while (offset < image_size) {
		ssize_t blklen = fread(&cmd.data, 1, sizeof(cmd.data), fimg);

		if (blklen == 0) {
			ret = ferror(fimg);
			if (ret)
				return ret;
			break;
		}

		cmd.hdr.offset = htole32(offset);
		cmd.hdr.blk_length = htole32(blklen);

		ret = switchtec_cmd(dev, MRPC_FWDNLD, &cmd, sizeof(cmd),
				    NULL, 0);

		if (ret < 0)
			return ret;

		ret = switchtec_fw_wait(dev, &status);
		if (ret < 0)
			return ret;

		offset += cmd.hdr.blk_length;

		if (progress_callback)
			progress_callback(offset, image_size);
	}

	if (status == SWITCHTEC_DLSTAT_COMPLETES)
		return 0;

	if (status == SWITCHTEC_DLSTAT_SUCCESS_FIRM_ACT)
		return 0;

	if (status == SWITCHTEC_DLSTAT_SUCCESS_DATA_ACT)
		return 0;

	if (status == 0)
		return SWITCHTEC_DLSTAT_HARDWARE_ERR;

	return status;
}

void switchtec_fw_perror(const char *s, int ret)
{
	const char *msg;

	if (ret <= 0) {
		perror(s);
		return;
	}

	switch(ret) {
	case SWITCHTEC_DLSTAT_HEADER_INCORRECT:
		msg = "Header incorrect";  break;
	case SWITCHTEC_DLSTAT_OFFSET_INCORRECT:
		msg = "Offset incorrect";  break;
	case SWITCHTEC_DLSTAT_CRC_INCORRECT:
		msg = "CRC incorrect";  break;
	case SWITCHTEC_DLSTAT_LENGTH_INCORRECT:
		msg = "Length incorrect";  break;
	case SWITCHTEC_DLSTAT_HARDWARE_ERR:
		msg = "Hardware Error";  break;
	default:
		fprintf(stderr, "%s: Unknown Error (%d)\n", s, ret);
		return;
	}

	fprintf(stderr, "%s: %s\n", s, msg);
}

struct fw_image_header {
	char magic[4];
	uint32_t image_len;
	uint32_t type;
	uint32_t load_addr;
	uint32_t version;
	uint32_t rsvd[9];
	uint32_t header_crc;
	uint32_t image_crc;
};


unsigned long switch_convertu8tou32(UINT8 *p, int size)
{
    unsigned long tmp,data;
    int i;

    tmp=0;
	data = 0;

	if(size > 4)
	{
	    return -1;
    }

	for(i=0;i<size;i++)
	{
       tmp = p[i];
	   data |= (tmp << (i*8));
	}

	return data;
}
int switchtec_fw_file_info(int fd, struct switchtec_fw_image_info *info)
{
	int ret;
	struct fwdl_file_hdr_struct tri_hdr;
    	UINT8  tri_vend[5] = {"MSCC"};
    	UINT8 file_tri;
    	UINT32 ver_num;
	if (info == NULL)
		return 0;
        
        lseek(fd, 0, SEEK_SET);
	ret = read(fd, &tri_hdr, sizeof(tri_hdr));
	
	if (ret != sizeof(tri_hdr))
	{	
            printf("\r\ntrident file ret=%d",ret);
            goto invalid_file;   
	}
        file_tri = 0;
        for(int i=0; i<4;i++)
	{
	    if(tri_hdr.vendor_id[i]==tri_vend[i])
	    {
		file_tri++; /*trident*/ 
	    } 
	}		

        if(file_tri == 4)
        {
		info->type = (enum switchtec_fw_image_type)switch_convertu8tou32(tri_hdr.part_type,4);
		info->crc = switch_convertu8tou32(tri_hdr.img_crc,4);	
		ver_num = switch_convertu8tou32(tri_hdr.version,4);
		version_to_string(ver_num, info->version, sizeof(info->version));
		info->image_addr = (size_t)switch_convertu8tou32(tri_hdr.load_addr,4);
		info->image_len = (size_t)switch_convertu8tou32(tri_hdr.img_length,4);	    

	}
	else
	{
            goto invalid_file;
	}
	return 0;

invalid_file:
	errno = ENOEXEC;
	return -errno;

}

const char *switchtec_fw_image_type(const struct switchtec_fw_image_info *info)
{
	switch((unsigned long)info->type) {
	case SWITCHTEC_FW_TYPE_BOOT: return "BOOT";
	case SWITCHTEC_FW_TYPE_MAP0: return "MAP";
	case SWITCHTEC_FW_TYPE_MAP1: return "MAP";
	case SWITCHTEC_FW_TYPE_IMG0: return "IMG";
	case SWITCHTEC_FW_TYPE_IMG1: return "IMG";
	case SWITCHTEC_FW_TYPE_DAT0: return "DAT";
	case SWITCHTEC_FW_TYPE_DAT1: return "DAT";
	case SWITCHTEC_FW_TYPE_NVLOG: return "NVLOG";
	case SWITCHTEC_FW_TYPE_SEEPROM: return "SEEPROM";

	//Legacy
	case 0xa8000000: return "BOOT (LEGACY)";
	case 0xa8020000: return "MAP (LEGACY)";
	case 0xa8060000: return "IMG (LEGACY)";
	case 0xa8210000: return "DAT (LEGACY)";

	default: return "UNKNOWN";
	}
}

static int get_part(struct switchtec_dev *dev,
		    struct switchtec_fw_image_info *info, int part)
{
	struct switchtec_ioctl_flash_part_info ioctl_info = {0};
	int ret;

	ioctl_info.flash_partition = part;

	ret = ioctl(dev->fd, SWITCHTEC_IOCTL_FLASH_PART_INFO, &ioctl_info);
	if (ret)
		return ret;

	info->image_addr = ioctl_info.address;
	info->image_len = ioctl_info.length;
	info->active = ioctl_info.active;
	return 0;
}

int switchtec_fw_part_info_tri(struct switchtec_dev *dev, int nr_info,
			   struct switchtec_fw_image_info *info)
{
	int ret;
	int i;
	struct fwdl_meta ftr;

	if (info == NULL || nr_info == 0)
		return -EINVAL;

	for (i = 0; i < nr_info; i++) {
		struct switchtec_fw_image_info *inf = &info[i];

		switch(info[i].type) {
	   case SWITCHTEC_FW_TYPE_BL20: 		   
		   ret = get_part(dev, inf, SWITCHTEC_IOCTL_PART_BL20);
	   
		   break;
	   case SWITCHTEC_FW_TYPE_BL21: 		   
		   ret = get_part(dev, inf, SWITCHTEC_IOCTL_PART_BL21);
		   
		   break;
		case SWITCHTEC_FW_TYPE_IMG0:
			ret = get_part(dev, inf, SWITCHTEC_IOCTL_PART_IMG0);
			break;
		case SWITCHTEC_FW_TYPE_IMG1:
			ret = get_part(dev, inf, SWITCHTEC_IOCTL_PART_IMG1);
			break;
		case SWITCHTEC_FW_TYPE_DAT0:
			ret = get_part(dev, inf, SWITCHTEC_IOCTL_PART_CFG0);
			break;
		case SWITCHTEC_FW_TYPE_DAT1:
			ret = get_part(dev, inf, SWITCHTEC_IOCTL_PART_CFG1);
			break;
		case SWITCHTEC_FW_TYPE_NVLOG:
			ret = get_part(dev, inf, SWITCHTEC_IOCTL_PART_NVLOG);
			if (ret)
				return ret;

			info[i].version[0] = 0;
			info[i].crc = 0;

			continue;
		default:
			return -EINVAL;
		}

		if (ret)
			return ret;

		ret = switchtec_fw_read_meta(dev, inf->image_addr,
					       inf->image_len, &ftr,
					       inf->version,
					       sizeof(inf->version));
		if (ret < 0) {
			inf->version[0] = 0;
			inf->crc = 0xFFFFFFFF;
		} else {
			inf->crc = ftr.img_crc;
		}
	}

	return nr_info;
}

static long multicfg_subcmd(struct switchtec_dev *dev, uint32_t subcmd,
			    uint8_t index)
{
	int ret;
	uint32_t result;

	subcmd |= index << 8;

	ret = switchtec_cmd(dev, MRPC_MULTI_CFG, &subcmd, sizeof(subcmd),
			    &result, sizeof(result));
	if (ret)
		return -1;

	return result;
}

static int get_multicfg(struct switchtec_dev *dev,
			struct switchtec_fw_image_info *info,
			int *nr_mult)
{
	int ret;
	int i;

	ret = multicfg_subcmd(dev, MRPC_MULTI_CFG_SUPPORTED, 0);
	if (ret < 0)
		return ret;

	if (!ret) {
		*nr_mult = 0;
		return 0;
	}

	ret = multicfg_subcmd(dev, MRPC_MULTI_CFG_COUNT, 0);
	if (ret < 0)
		return ret;

	if (*nr_mult > ret)
		*nr_mult = ret;

	for (i = 0; i < *nr_mult; i++) {
		info[i].image_addr = multicfg_subcmd(dev,
						     MRPC_MULTI_CFG_START_ADDR,
						     i);
		info[i].image_len = multicfg_subcmd(dev,
						    MRPC_MULTI_CFG_LENGTH, i);
		strcpy(info[i].version, "");
		info[i].crc = 0;
		info[i].active = 0;
	}

	ret = multicfg_subcmd(dev, MRPC_MULTI_CFG_ACTIVE, 0);
	if (ret < 0)
		return ret;

	if (ret < *nr_mult)
		info[ret].active = 1;

	return 0;
}

int switchtec_fw_cfg_info(struct switchtec_dev *dev,
			  struct switchtec_fw_image_info *act_cfg,
			  struct switchtec_fw_image_info *inact_cfg,
			  struct switchtec_fw_image_info *mult_cfg,
			  int *nr_mult)
{
	int ret;
	struct switchtec_fw_image_info info[2];

	info[0].type = SWITCHTEC_FW_TYPE_DAT0;
	info[1].type = SWITCHTEC_FW_TYPE_DAT1;

	ret = switchtec_fw_part_info_tri(dev, sizeof(info) / sizeof(*info),
				     info);
	if (ret < 0)
		return ret;

	if (switchtec_fw_active(&info[0])) {
		if (act_cfg)
			memcpy(act_cfg, &info[0], sizeof(*act_cfg));
		if (inact_cfg)
			memcpy(inact_cfg, &info[1], sizeof(*inact_cfg));
	} else {
		if (act_cfg)
			memcpy(act_cfg, &info[1], sizeof(*act_cfg));
		if (inact_cfg)
			memcpy(inact_cfg, &info[0], sizeof(*inact_cfg));
	}

	if (!nr_mult || !mult_cfg || *nr_mult == 0)
		return 0;

	return get_multicfg(dev, mult_cfg, nr_mult);
}

int switchtec_fw_img_info(struct switchtec_dev *dev,
			  struct switchtec_fw_image_info *act_img,
			  struct switchtec_fw_image_info *inact_img)
{
	int ret;
	struct switchtec_fw_image_info info[2];

	info[0].type = SWITCHTEC_FW_TYPE_IMG0;
	info[1].type = SWITCHTEC_FW_TYPE_IMG1;

	ret = switchtec_fw_part_info_tri(dev, sizeof(info) / sizeof(*info),info);
	if (ret < 0)
		return ret;

	if (switchtec_fw_active(&info[0])) {
		if (act_img)
			memcpy(act_img, &info[0], sizeof(*act_img));
		if (inact_img)
			memcpy(inact_img, &info[1], sizeof(*inact_img));
	} else {
		if (act_img)
			memcpy(act_img, &info[1], sizeof(*act_img));
		if (inact_img)
			memcpy(inact_img, &info[0], sizeof(*inact_img));
	}

	return 0;
}
int switchtec_bl2_img_info(struct switchtec_dev *dev,
				struct switchtec_fw_image_info *act_img,
				struct switchtec_fw_image_info *inact_img)
{
    int ret;
    struct switchtec_fw_image_info info[2];

    info[0].type = SWITCHTEC_FW_TYPE_BL20;
    info[1].type = SWITCHTEC_FW_TYPE_BL21;

    ret = switchtec_fw_part_info_tri(dev, sizeof(info) / sizeof(*info), info);

    if (ret < 0)
	{     return ret;    }

	if (switchtec_fw_active(&info[0])) {
		if (act_img)
			memcpy(act_img, &info[0], sizeof(*act_img));
		if (inact_img)
			memcpy(inact_img, &info[1], sizeof(*inact_img));
	} else {
		if (act_img)
			memcpy(act_img, &info[1], sizeof(*act_img));
		if (inact_img)
			memcpy(inact_img, &info[0], sizeof(*inact_img));
	}

	return 0;
}

int switchtec_fw_read(struct switchtec_dev *dev, unsigned long addr,
		      size_t len, void *buf)
{
	int ret;
	struct {
		uint32_t addr;
		uint32_t length;
	} cmd;
	unsigned char *cbuf = buf;
	size_t read = 0;

	while(len) {
		size_t chunk_len = len;
		if (chunk_len > MRPC_MAX_DATA_LEN-8)
			chunk_len = MRPC_MAX_DATA_LEN-8;

		cmd.addr = htole32(addr);
		cmd.length = htole32(chunk_len);

		ret = switchtec_cmd(dev, MRPC_RD_FLASH, &cmd, sizeof(cmd),
				    cbuf, chunk_len);
		if (ret)
			return -1;

		addr += chunk_len;
		len -= chunk_len;
		read += chunk_len;
		cbuf += chunk_len;
	}

	return read;
}

int switchtec_fw_read_fd(struct switchtec_dev *dev, int fd,
			 unsigned long addr, size_t len,
			 void (*progress_callback)(int cur, int tot))
{
	int ret;
	unsigned char buf[(MRPC_MAX_DATA_LEN-8)*4];
	size_t read = 0;
	size_t total_len = len;
	size_t total_wrote;
	ssize_t wrote;

	while(len) {
		size_t chunk_len = len;
		if (chunk_len > sizeof(buf))
			chunk_len = sizeof(buf);

		ret = switchtec_fw_read(dev, addr, chunk_len, buf);
		if (ret < 0)
			return ret;

		total_wrote = 0;
		while (total_wrote < ret) {
			wrote = write(fd, &buf[total_wrote],
				      ret - total_wrote);
			if (wrote < 0)
				return -1;
			total_wrote += wrote;
		}

		read += ret;
		addr += ret;
		len -= ret;

		if (progress_callback)
			progress_callback(read, total_len);
	}

	return read;
}

int switchtec_fw_read_footer(struct switchtec_dev *dev,
			     unsigned long partition_start,
			     size_t partition_len,
			     struct switchtec_fw_footer *ftr,
			     char *version, size_t version_len)
{
	int ret;
	unsigned long addr = partition_start + partition_len -
		sizeof(struct switchtec_fw_footer);

	if (!ftr)
		return -EINVAL;

	ret = switchtec_fw_read(dev, addr, sizeof(struct switchtec_fw_footer),
				ftr);
	if (ret < 0)
		return ret;

	if (strcmp(ftr->magic, "PMC") != 0) {
		errno = ENOEXEC;
		return -errno;
	}

	if (version)
		version_to_string(ftr->version, version, version_len);

	return 0;
}
int switchtec_fw_read_meta(struct switchtec_dev *dev,
				  unsigned long partition_start,
				  size_t partition_len,
				  struct fwdl_meta *ftr,
				  char *version, size_t version_len)
{
    int ret;
    //UINT8 vend[5]={'M','S','C','C'};

    unsigned long addr = partition_start + partition_len - sizeof(struct fwdl_meta);

	if (!ftr)
	{	return -EINVAL;   }
    
    ret = switchtec_fw_read(dev, addr, sizeof(struct fwdl_meta),ftr);
    if (ret < 0)
	{   return ret; }



    if (version)
	{    version_to_string(ftr->version, version, version_len);  }

    return 0;
}
int switchtec_fw_img_write_hdr_tri(int fd, struct fwdl_meta *ftr,
				  enum switchtec_fw_image_type type)
{
   enum switchtec_fw_image_type imgtype;
   struct fwdl_file_hdr_struct tri_hdr = {};
   
   imgtype = type;
   memcpy(tri_hdr.vendor_id, ftr->vendor_id, sizeof(tri_hdr.vendor_id));
   memcpy(tri_hdr.img_length,&(ftr->img_length), sizeof(tri_hdr.img_length));
   memcpy(tri_hdr.part_type,&imgtype, sizeof(tri_hdr.part_type));
   memcpy(tri_hdr.load_addr,&(ftr->load_addr), sizeof(tri_hdr.load_addr));
   memcpy(tri_hdr.version,&(ftr->version), sizeof(tri_hdr.version));
   memcpy(tri_hdr.hdr_crc,&(ftr->hdr_crc), sizeof(tri_hdr.hdr_crc));
   memcpy(tri_hdr.img_crc,&(ftr->img_crc), sizeof(tri_hdr.img_crc));

   return write(fd, &tri_hdr, sizeof(tri_hdr));
}

struct switchtec_boot_ro {
	uint8_t subcmd;
	uint8_t set_get;
	uint8_t status;
	uint8_t reserved;
};

int switchtec_fw_is_boot_ro(struct switchtec_dev *dev)
{
	struct switchtec_boot_ro subcmd = {
		.subcmd = MRPC_FWDNLD_BOOT_RO,
		.set_get = 0,
	};

	struct {
		uint8_t status;
		uint8_t reserved[3];
	} result;

	int ret;

	ret = switchtec_cmd(dev, MRPC_FWDNLD, &subcmd, sizeof(subcmd),
			    &result, sizeof(result));

	if (ret == ERR_SUBCMD_INVALID) {
		errno = 0;
		return 0;
	}

	if (ret)
		return ret;

	return result.status;
}

int switchtec_fw_set_boot_ro(struct switchtec_dev *dev,
			     enum switchtec_fw_ro ro)
{
	struct switchtec_boot_ro subcmd = {
		.subcmd = MRPC_FWDNLD_BOOT_RO,
		.set_get = 1,
		.status = ro,
	};

	return switchtec_cmd(dev, MRPC_FWDNLD, &subcmd, sizeof(subcmd),
			     NULL, 0);
}
