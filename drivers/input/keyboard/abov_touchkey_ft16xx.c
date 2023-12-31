/* abov_touchkey.c -- Linux driver for abov chip as touchkey
 *
 * Copyright (C) 2013 Samsung Electronics Co.Ltd
 * Author: Junkyeong Kim <jk0430.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <asm/unaligned.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>

#ifdef CONFIG_SEC_SYSFS
#include <linux/sec_class.h>
#else
extern struct class *sec_class;
#endif

#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif
#ifdef CONFIG_INPUT_BOOSTER
#include <linux/input/input_booster.h>
#endif

#define ABOV_TK_NAME "abov-touchkey"

/* registers */
#define ABOV_BTNSTATUS		0x00
#define ABOV_FW_VER		0x01
#define ABOV_PCB_VER		0x02
#define ABOV_COMMAND		0x03
#define ABOV_THRESHOLD		0x04
#define ABOV_SENS		0x05
#define ABOV_SETIDAC		0x06
#define ABOV_DIFFDATA		0x0A
#define ABOV_RAWDATA		0x0E
#define ABOV_VENDORID		0x12
#define ABOV_GLOVE		0x13
#define ABOV_KEYBOARD		0x13
#define ABOV_MD_VER		0x14
#define ABOV_FLIP		0x15

/* command */
#define CMD_LED_ON		0x10
#define CMD_LED_OFF		0x20
#define CMD_DATA_UPDATE		0x40
#define CMD_LED_CTRL_ON		0x60
#define CMD_LED_CTRL_OFF	0x70
#define CMD_STOP_MODE		0x80

#define CMD_GLOVE_OFF		0x10
#define CMD_GLOVE_ON		0x20

#define CMD_MOBILE_KBD_OFF	0x10
#define CMD_MOBILE_KBD_ON	0x20

#define CMD_FLIP_OFF		0x10
#define CMD_FLIP_ON		0x20

#define ABOV_BOOT_DELAY		45
#define ABOV_RESET_DELAY	150

/* Force FW update if module# is different */
#define FORCE_FW_UPDATE_DIFF_MODULE

/* Support Glove Mode */
#define GLOVE_MODE

/* Touchkey LED twinkle during booting in factory sw (in LCD detached status) */
#ifdef CONFIG_SEC_FACTORY
#define LED_TWINKLE_BOOTING
#endif

#define TK_FW_PATH_SDCARD "/sdcard/Firmware/TOUCHKEY/abov_fw.bin"

#ifdef LED_TWINKLE_BOOTING
static void led_twinkle_work(struct work_struct *work);
extern unsigned int lcdtype;
#endif

static void abov_tk_resume_work(struct work_struct *work);
static int abov_tk_resume(struct device *dev);

#define I2C_M_WR 0		/* for i2c */

enum {
	BUILT_IN = 0,
	SDCARD,
};

#ifdef CONFIG_SAMSUNG_LPM_MODE
extern int poweroff_charging;
#endif
extern unsigned int system_rev;
extern struct class *sec_class;
static int touchkey_keycode[] = { 0,
	KEY_RECENT, KEY_BACK,
};

struct abov_tk_info {
	struct device *sec_touchkey;
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct abov_touchkey_dt_data *dtdata;
	struct mutex lock;
	struct pinctrl *pinctrl;
	struct mutex device;
	struct delayed_work resume_work;

	const struct firmware *firm_data_bin;
	const u8 *firm_data_ums;
	char phys[32];
	long firm_size;
	int irq;
	u16 menu_s;
	u16 back_s;
	u16 menu_raw;
	u16 back_raw;
	int (*power) (bool on);
	int touchkey_count;
	u8 fw_update_state;
	u8 fw_ver;
	u8 md_ver;
	u8 checksum_h;
	u8 checksum_l;
	u8 fw_ver_bin;
	u8 md_ver_bin;
	u8 checksum_h_bin;
	u8 checksum_l_bin;
	bool enabled;
#ifdef GLOVE_MODE
	bool glovemode;
#endif
	bool keyboard_mode;
	bool flip_mode;
	bool probe_done;
#ifdef LED_TWINKLE_BOOTING
	struct delayed_work led_twinkle_work;
	bool led_twinkle_check;
#endif
#ifdef CONFIG_INPUT_BOOSTER
	struct input_booster *tkey_booster;
#endif
};

struct abov_touchkey_dt_data {
	unsigned long irq_flag;
	int gpio_int;
	int gpio_sda;
	int gpio_scl;
	int sub_det;
	int gpio_en;
	int gpio_en_flag;
	bool reg_boot_on;
	bool forced_update;
	struct regulator *vdd_led;
	struct regulator *vdd_io_vreg;
	struct regulator *avdd_vreg;
	int (*power) (struct abov_tk_info *data, bool on);
	const char *fw_name;
};

static int abov_tk_input_open(struct input_dev *dev);
static void abov_tk_input_close(struct input_dev *dev);

static int abov_tk_i2c_read_checksum(struct abov_tk_info *info);

static int abov_touchkey_led_status;
static int abov_touchled_cmd_reserved;

static int abov_mode_enable(struct i2c_client *client, u8 cmd_reg, u8 cmd)
{
	return i2c_smbus_write_byte_data(client, cmd_reg, cmd);
}

static int abov_tk_i2c_read(struct i2c_client *client,
			    u8 reg, u8 * val, unsigned int len)
{
	struct abov_tk_info *info = i2c_get_clientdata(client);
	struct i2c_msg msg;
	int ret;
	int retry = 3;

	mutex_lock(&info->lock);
	msg.addr = client->addr;
	msg.flags = I2C_M_WR;
	msg.len = 1;
	msg.buf = &reg;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0)
			break;

		input_err(true, &client->dev, "%s fail(address set)(%d)\n",
			  __func__, retry);
		usleep_range(10 * 1000, 10 * 1000);
	}
	if (ret < 0) {
		mutex_unlock(&info->lock);
		return ret;
	}
	retry = 3;
	msg.flags = 1;		/*I2C_M_RD */
	msg.len = len;
	msg.buf = val;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0) {
			mutex_unlock(&info->lock);
			return 0;
		}
		input_err(true, &client->dev, "%s fail(data read)(%d)\n",
			  __func__, retry);
		usleep_range(10 * 1000, 10 * 1000);
	}
	mutex_unlock(&info->lock);
	return ret;
}

static int abov_tk_i2c_write(struct i2c_client *client,
			     u8 reg, u8 * val, unsigned int len)
{
	struct abov_tk_info *info = i2c_get_clientdata(client);
	struct i2c_msg msg[1];
	unsigned char data[2];
	int ret;
	int retry = 3;

	mutex_lock(&info->lock);
	data[0] = reg;
	data[1] = *val;
	msg->addr = client->addr;
	msg->flags = I2C_M_WR;
	msg->len = 2;
	msg->buf = data;

	while (retry--) {
		ret = i2c_transfer(client->adapter, msg, 1);
		if (ret >= 0) {
			mutex_unlock(&info->lock);
			return 0;
		}
		input_err(true, &client->dev, "%s fail(%d)\n", __func__, retry);
		usleep_range(10 * 1000, 10 * 1000);
	}
	mutex_unlock(&info->lock);
	return ret;
}

static void release_all_fingers(struct abov_tk_info *info)
{
	struct i2c_client *client = info->client;
	int i;

	input_dbg(true, &client->dev, "[TK] %s\n", __func__);

	for (i = 1; i < info->touchkey_count; i++) {
		input_report_key(info->input_dev, touchkey_keycode[i], 0);
	}
	input_sync(info->input_dev);

#ifdef CONFIG_INPUT_BOOSTER
	if (info->tkey_booster && info->tkey_booster->dvfs_set)
		info->tkey_booster->dvfs_set(info->tkey_booster, 2);
#endif
}

static int abov_tk_reset_for_bootmode(struct abov_tk_info *info)
{
	info->dtdata->power(info, false);
	msleep(50);

	info->dtdata->power(info, true);

	return 0;
}

static void abov_tk_reset(struct abov_tk_info *info)
{
	struct i2c_client *client = info->client;

	if (info->enabled == false)
		return;

	input_info(true, &client->dev, "%s++\n", __func__);
	disable_irq_nosync(info->irq);

	info->enabled = false;

	release_all_fingers(info);

	abov_tk_reset_for_bootmode(info);
	msleep(ABOV_RESET_DELAY);

	if (info->flip_mode)
		abov_mode_enable(client, ABOV_FLIP, CMD_FLIP_ON);
#ifdef GLOVE_MODE
	else if (info->glovemode)
		abov_mode_enable(client, ABOV_GLOVE, CMD_GLOVE_ON);
#endif
	if (info->keyboard_mode)
		abov_mode_enable(client, ABOV_KEYBOARD, CMD_MOBILE_KBD_ON);

	info->enabled = true;

	enable_irq(info->irq);
	input_info(true, &client->dev, "%s--\n", __func__);
}

static irqreturn_t abov_tk_interrupt(int irq, void *dev_id)
{
	struct abov_tk_info *info = dev_id;
	struct i2c_client *client = info->client;
	int ret, retry;
	u8 buf;
	int menu_data;
	int back_data;
	u8 menu_press;
	u8 back_press;
#ifdef CONFIG_INPUT_BOOSTER
	bool press;
#endif

	ret = abov_tk_i2c_read(client, ABOV_BTNSTATUS, &buf, 1);
	if (ret < 0) {
		retry = 3;
		while (retry--) {
			input_err(true, &client->dev, "%s read fail(%d)\n",
				  __func__, retry);
			ret = abov_tk_i2c_read(client, ABOV_BTNSTATUS, &buf, 1);
			if (ret == 0)
				break;

			usleep_range(10 * 1000, 10 * 1000);
		}
		if (retry == 0) {
			abov_tk_reset(info);
			return IRQ_HANDLED;
		}
	}
	// added dual key mode concept for screen pinning(google)
	menu_data = buf & 0x03;
	back_data = (buf >> 2) & 0x03;
	menu_press = !(menu_data % 2);
	back_press = !(back_data % 2);

	if (menu_data)
		input_report_key(info->input_dev,
				 touchkey_keycode[1], menu_press);
	if (back_data)
		input_report_key(info->input_dev,
				 touchkey_keycode[2], back_press);

	input_sync(info->input_dev);

#ifdef CONFIG_SAMSUNG_PRODUCT_SHIP
	input_info(true, &client->dev,
		   "key %s%s ver0x%02x\n",
		   menu_data ? (menu_press ? "P" : "R") : "",
		   back_data ? (back_press ? "P" : "R") : "", info->fw_ver);
#else
	input_info(true, &client->dev,
		   "%s%s%x ver0x%02x\n",
		   menu_data ? (menu_press ? "menu P " : "menu R ") : "",
		   back_data ? (back_press ? "back P " : "back R ") : "",
		   buf, info->fw_ver);
#endif

#ifdef CONFIG_INPUT_BOOSTER
	press = (menu_data ? menu_press : 0) || (back_data ? back_press : 0);
	if (info->tkey_booster && info->tkey_booster->dvfs_set)
		info->tkey_booster->dvfs_set(info->tkey_booster, press);
#endif

	return IRQ_HANDLED;
}

#ifdef CONFIG_INPUT_BOOSTER
static ssize_t boost_level_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	int val, stage;
	struct dvfs value;

	if (!info->tkey_booster) {
		input_err(true, &info->client->dev,
			  "%s: booster is NULL\n", __func__);
		return count;
	}

	sscanf(buf, "%d", &val);

	stage = 1 << val;

	if (!(info->tkey_booster->dvfs_stage & stage)) {
		input_info(true, &info->client->dev,
			   "%s: wrong cmd %d\n", __func__, val);
		return count;
	}

	info->tkey_booster->dvfs_boost_mode = stage;
	input_info(true, &info->client->dev,
		   "%s: dvfs_boost_mode = 0x%X\n",
		   __func__, info->tkey_booster->dvfs_boost_mode);

	if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_NONE) {
		if (info->tkey_booster->dvfs_set)
			info->tkey_booster->dvfs_set(info->tkey_booster, 2);
	} else if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_SINGLE) {
		input_booster_get_default_setting("head", &value);
		info->tkey_booster->dvfs_freq = value.cpu_freq;
		input_info(true, &info->client->dev,
			   "%s: boost_mode SINGLE, dvfs_freq = %d\n",
			   __func__, info->tkey_booster->dvfs_freq);
	} else if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_DUAL) {
		input_booster_get_default_setting("tail", &value);
		info->tkey_booster->dvfs_freq = value.cpu_freq;
		input_info(true, &info->client->dev,
			   "%s: boost_mode DUAL, dvfs_freq = %d\n",
			   __func__, info->tkey_booster->dvfs_freq);
	}

	return count;
}
#endif

static int touchkey_led_set(struct abov_tk_info *info, int data)
{
	u8 cmd;
	int ret;

	if (data == 1)
		cmd = CMD_LED_ON;
	else
		cmd = CMD_LED_OFF;

	if (!info->enabled)
		goto out;

	ret = abov_tk_i2c_write(info->client, ABOV_BTNSTATUS, &cmd, 1);
	if (ret < 0) {
		input_err(true, &info->client->dev, "%s fail(%d)\n", __func__,
			  ret);
		goto out;
	}

	return 0;
out:
	abov_touchled_cmd_reserved = 1;
	abov_touchkey_led_status = cmd;
	return 1;
}

static ssize_t touchkey_led_control(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1) {
		input_err(true, &client->dev, "%s: cmd read err\n", __func__);
		return count;
	}

	if (!(data == 0 || data == 1)) {
		input_err(true, &client->dev, "%s: wrong command(%d)\n",
			  __func__, data);
		return count;
	}
#ifdef LED_TWINKLE_BOOTING
	if (info->led_twinkle_check == 1) {
		info->led_twinkle_check = 0;
		cancel_delayed_work(&info->led_twinkle_work);
	}
#endif

	if (touchkey_led_set(info, data))
		return count;

	msleep(20);

	abov_touchled_cmd_reserved = 0;
	input_info(true, &client->dev, "%s data(%d)\n", __func__, data);

	return count;
}

static ssize_t touchkey_threshold_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	u8 r_buf;
	int ret;

	ret = abov_tk_i2c_read(client, ABOV_THRESHOLD, &r_buf, 1);
	if (ret < 0) {
		input_err(true, &client->dev, "%s fail(%d)\n", __func__, ret);
		r_buf = 0;
	}
	return snprintf(buf, PAGE_SIZE, "%d\n", r_buf);
}

static void get_diff_data(struct abov_tk_info *info)
{
	struct i2c_client *client = info->client;
	u8 r_buf[4];
	int ret;

	ret = abov_tk_i2c_read(client, ABOV_DIFFDATA, r_buf, 4);
	if (ret < 0) {
		input_err(true, &client->dev, "%s fail(%d)\n", __func__, ret);
		info->menu_s = 0;
		info->back_s = 0;
		return;
	}

	info->menu_s = (r_buf[0] << 8) | r_buf[1];
	info->back_s = (r_buf[2] << 8) | r_buf[3];
}

static ssize_t touchkey_menu_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	get_diff_data(info);

	return sprintf(buf, "%d\n", info->menu_s);
}

static ssize_t touchkey_back_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	get_diff_data(info);

	return sprintf(buf, "%d\n", info->back_s);
}

static void get_raw_data(struct abov_tk_info *info)
{
	struct i2c_client *client = info->client;
	u8 r_buf[4];
	int ret;

	ret = abov_tk_i2c_read(client, ABOV_RAWDATA, r_buf, 4);
	if (ret < 0) {
		input_err(true, &client->dev, "%s fail(%d)\n", __func__, ret);
		info->menu_raw = 0;
		info->back_raw = 0;
		return;
	}

	info->menu_raw = (r_buf[0] << 8) | r_buf[1];
	info->back_raw = (r_buf[2] << 8) | r_buf[3];
}

static ssize_t touchkey_menu_raw_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	get_raw_data(info);

	return snprintf(buf, PAGE_SIZE, "%d\n", info->menu_raw);
}

static ssize_t touchkey_back_raw_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	get_raw_data(info);

	return snprintf(buf, PAGE_SIZE, "%d\n", info->back_raw);
}

static ssize_t bin_fw_ver(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;

	input_dbg(true, &client->dev, "fw version bin : 0x%x\n",
		  info->fw_ver_bin);

	return snprintf(buf, PAGE_SIZE, "0x%02x\n", info->fw_ver_bin);
}

static int get_tk_fw_version(struct abov_tk_info *info, bool bootmode)
{
	struct i2c_client *client = info->client;
	u8 buf;
	int ret;
	int retry = 3;

	ret = abov_tk_i2c_read(client, ABOV_FW_VER, &buf, 1);
	if (ret < 0) {
		while (retry--) {
			input_err(true, &client->dev, "%s read fail(%d)\n",
				  __func__, retry);
			if (!bootmode)
				abov_tk_reset(info);
			else
				return -1;
			ret = abov_tk_i2c_read(client, ABOV_FW_VER, &buf, 1);
			if (ret == 0)
				break;
		}
		if (retry == 0)
			return -1;
	}

	info->fw_ver = buf;

	retry = 3;
	ret = abov_tk_i2c_read(client, ABOV_MD_VER, &buf, 1);
	if (ret < 0) {
		while (retry--) {
			input_err(true, &client->dev, "%s read fail(%d)\n",
				  __func__, retry);
			if (!bootmode)
				abov_tk_reset(info);
			else
				return -1;
			ret = abov_tk_i2c_read(client, ABOV_MD_VER, &buf, 1);
			if (ret == 0)
				break;
		}
		if (retry == 0)
			return -1;
	}

	info->md_ver = buf;
	input_info(true, &client->dev, "%s : fw = 0x%x, md = 0x%x\n",
		   __func__, info->fw_ver, info->md_ver);
	return 0;
}

static ssize_t read_fw_ver(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int ret;

	ret = get_tk_fw_version(info, false);
	if (ret < 0) {
		input_err(true, &client->dev, "%s read fail\n", __func__);
		info->fw_ver = 0;
	}

	abov_tk_i2c_read_checksum(info);

	return snprintf(buf, PAGE_SIZE, "0x%02x\n", info->fw_ver);
}

static int abov_load_fw(struct abov_tk_info *info, u8 cmd)
{
	struct i2c_client *client = info->client;
	struct file *fp;
	mm_segment_t old_fs;
	long fsize, nread;
	int ret = 0;

	switch (cmd) {
	case BUILT_IN:
		ret = request_firmware(&info->firm_data_bin,
				       info->dtdata->fw_name, &client->dev);
		if (ret) {
			input_err(true, &client->dev,
				  "%s request_firmware fail. \n", __func__);
			return ret;
		}
		/* Header info
		 * 0x00 0x91 : model info,
		 * 0x00 0x00 : module info (Rev 0.0),
		 * 0x00 0xF3 : F/W
		 * 0x00 0x00 0x17 0x10 : checksum
		 * ~ 22byte 0x00 */
		info->fw_ver_bin = info->firm_data_bin->data[5];
		info->md_ver_bin = info->firm_data_bin->data[1];

		info->checksum_h_bin = info->firm_data_bin->data[8];
		info->checksum_l_bin = info->firm_data_bin->data[9];
		info->firm_size = info->firm_data_bin->size;

		input_info(true, &client->dev,
			   "%s : fw = 0x%x, md = 0x%x, crc = 0x%02x%02x\n",
			   __func__, info->fw_ver_bin, info->md_ver_bin,
			   info->checksum_h_bin, info->checksum_l_bin);
		break;

	case SDCARD:
		old_fs = get_fs();
		set_fs(get_ds());
		fp = filp_open(TK_FW_PATH_SDCARD, O_RDONLY, S_IRUSR);
		if (IS_ERR(fp)) {
			input_err(true, &client->dev,
				  "%s %s open error\n", __func__,
				  TK_FW_PATH_SDCARD);
			ret = -ENOENT;
			goto fail_sdcard_open;
		}

		fsize = fp->f_path.dentry->d_inode->i_size;
		info->firm_data_ums = kzalloc((size_t) fsize, GFP_KERNEL);
		if (!info->firm_data_ums) {
			input_err(true, &client->dev,
				  "%s fail to kzalloc for fw\n", __func__);
			ret = -ENOMEM;
			goto fail_sdcard_kzalloc;
		}

		nread = vfs_read(fp,
				 (char __user *)info->firm_data_ums, fsize,
				 &fp->f_pos);
		if (nread != fsize) {
			input_err(true, &client->dev,
				  "%s fail to vfs_read file\n", __func__);
			ret = -EINVAL;
			goto fail_sdcard_size;
		}
		filp_close(fp, current->files);
		set_fs(old_fs);
		info->firm_size = nread;
		break;

	default:
		ret = -1;
		break;
	}
	input_info(true, &client->dev, "fw_size : %lu\n", info->firm_size);
	input_info(true, &client->dev, "%s success\n", __func__);
	return ret;

fail_sdcard_size:
	kfree(info->firm_data_ums);
fail_sdcard_kzalloc:
	filp_close(fp, current->files);
fail_sdcard_open:
	set_fs(old_fs);
	return ret;
}

static int abov_tk_check_busy(struct abov_tk_info *info)
{
	int ret, count = 0;
	unsigned char val = 0x00;

	do {
		ret = i2c_master_recv(info->client, &val, sizeof(val));

		if (val & 0x01) {
			count++;
			if (count > 1000)
				input_err(true, &info->client->dev,
					  "%s: busy %d\n", __func__, count);
			return ret;
		} else {
			break;
		}

	} while (1);

	return ret;
}

static int abov_tk_i2c_read_checksum(struct abov_tk_info *info)
{
	unsigned char data[6] = { 0xAC, 0x9E, 0x10, 0x00, 0x3F, 0xFF };
	unsigned char checksum[5] = { 0, };
	int ret;
	unsigned char reg = 0x00;

	i2c_master_send(info->client, data, 6);

	usleep_range(5 * 1000, 5 * 1000);

	abov_tk_check_busy(info);

	ret = abov_tk_i2c_read(info->client, reg, checksum, 5);

	input_info(true, &info->client->dev,
		   "%s: ret:%d [%X][%X][%X][%X][%X]\n", __func__, ret,
		   checksum[0], checksum[1], checksum[2]
		   , checksum[3], checksum[4]);
	info->checksum_h = checksum[3];
	info->checksum_l = checksum[4];
	return 0;
}

static int abov_tk_fw_write(struct abov_tk_info *info, unsigned char *addrH,
			    unsigned char *addrL, unsigned char *val)
{
	int length = 36, ret = 0;
	unsigned char data[36];

	data[0] = 0xAC;
	data[1] = 0x7A;
	memcpy(&data[2], addrH, 1);
	memcpy(&data[3], addrL, 1);
	memcpy(&data[4], val, 32);

	ret = i2c_master_send(info->client, data, length);
	if (ret != length) {
		input_err(true, &info->client->dev,
			  "%s: write fail[%x%x], %d\n", __func__, *addrH,
			  *addrL, ret);
		return ret;
	}

	usleep_range(3 * 1000, 3 * 1000);

	abov_tk_check_busy(info);

	return 0;
}

static int abov_tk_fw_mode_enter(struct abov_tk_info *info)
{
	unsigned char data[3] = { 0xAC, 0x5B, 0x2D };
	int ret = 0;

	ret = i2c_master_send(info->client, data, 3);
	if (ret != 3) {
		input_err(true, &info->client->dev,
			  "%s: write fail\n", __func__);
		return -1;
	}

	return 0;

}

static int abov_tk_fw_update(struct abov_tk_info *info, u8 cmd)
{
	int ret, ii = 0;
	int count;
	unsigned short address;
	unsigned char addrH, addrL;
	unsigned char data[32] = { 0, };

	input_err(true, &info->client->dev, "%s: start\n", __func__);

	count = info->firm_size / 32;
	address = 0x1000;

	input_err(true, &info->client->dev, "%s: reset\n", __func__);
	abov_tk_reset_for_bootmode(info);
	msleep(ABOV_BOOT_DELAY);

	ret = abov_tk_fw_mode_enter(info);
	if (ret < 0) {
		input_err(true, &info->client->dev,
			  "%s: failed to enter fw_mode\n", __func__);
		return ret;
	}
	input_err(true, &info->client->dev, "%s: fw_mode_cmd sended\n",
		  __func__);

	msleep(1100);

	input_err(true, &info->client->dev, "%s: fw_write start\n", __func__);
	for (ii = 1; ii < count; ii++) {
		/* first 32byte is header */
		addrH = (unsigned char)((address >> 8) & 0xFF);
		addrL = (unsigned char)(address & 0xFF);
		if (cmd == BUILT_IN)
			memcpy(data, &info->firm_data_bin->data[ii * 32], 32);
		else if (cmd == SDCARD)
			memcpy(data, &info->firm_data_ums[ii * 32], 32);

		ret = abov_tk_fw_write(info, &addrH, &addrL, data);
		if (ret < 0) {
			input_err(true, &info->client->dev,
				  "%s: err, no device : %d\n", __func__, ret);
			return ret;
		}

		address += 0x20;

		memset(data, 0, 32);
	}
	input_err(true, &info->client->dev, "%s: fw_write end\n", __func__);

	ret = abov_tk_i2c_read_checksum(info);

	input_err(true, &info->client->dev, "%s: checksum readed\n", __func__);

	return ret;

}

static void abov_release_fw(struct abov_tk_info *info, u8 cmd)
{
	switch (cmd) {
	case BUILT_IN:
		release_firmware(info->firm_data_bin);
		break;

	case SDCARD:
		kfree(info->firm_data_ums);
		break;

	default:
		break;
	}
}

static int abov_flash_fw(struct abov_tk_info *info, bool probe, u8 cmd)
{
	struct i2c_client *client = info->client;
	int retry = 2;
	int ret;
	int block_count;
	int fw_ver, checksum_h, checksum_l;

	ret = get_tk_fw_version(info, probe);
	if (ret)
		info->fw_ver = 0;

	ret = abov_load_fw(info, cmd);
	if (ret) {
		input_err(true, &client->dev, "%s fw load fail\n", __func__);
		return ret;
	}

	if (cmd == BUILT_IN) {
		fw_ver = info->fw_ver_bin;
		checksum_h = info->checksum_h_bin;
		checksum_l = info->checksum_l_bin;
	} else if (cmd == SDCARD) {
		fw_ver = info->firm_data_ums[5];
		checksum_h = info->firm_data_ums[8];
		checksum_l = info->firm_data_ums[9];
	} else {
		ret = -1;
		goto out;
	}

	block_count = (int)(info->firm_size / 32);

	while (retry--) {
		ret = abov_tk_fw_update(info, cmd);
		if (ret < 0)
			break;

		if ((info->checksum_h != checksum_h) ||
		    (info->checksum_l != checksum_l)) {
			input_err(true, &client->dev,
				  "%s checksum fail.(0x%x,0x%x),(0x%x,0x%x) retry:%d\n",
				  __func__, info->checksum_h, info->checksum_l,
				  checksum_h, checksum_l, retry);
			ret = -1;
			continue;
		} else {
			input_info(true, &client->dev,
				   "%s checksum success, 0x%02x%02x\n",
				   __func__, info->checksum_h,
				   info->checksum_l);
		}

		abov_tk_reset_for_bootmode(info);
		msleep(ABOV_RESET_DELAY);
		ret = get_tk_fw_version(info, true);
		if (ret) {
			input_err(true, &client->dev,
				  "%s fw version read fail\n", __func__);
			ret = -1;
			continue;
		}

		if (info->fw_ver == 0) {
			input_err(true, &client->dev,
				  "%s fw version fail (0x%x)\n", __func__,
				  info->fw_ver);
			ret = -1;
			continue;
		}

		if (info->fw_ver != fw_ver) {
			input_err(true, &client->dev,
				  "%s fw version fail ic:0x%x, bin:0x%x\n",
				  __func__, info->fw_ver, fw_ver);
			ret = -1;
			continue;
		}
		ret = 0;
		break;
	}

out:
	abov_release_fw(info, cmd);

	return ret;
}

static ssize_t touchkey_fw_update(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int ret;
	u8 cmd;

	switch (*buf) {
	case 's':
	case 'S':
		cmd = BUILT_IN;
		break;
	case 'i':
	case 'I':
		cmd = SDCARD;
		break;
	default:
		info->fw_update_state = 2;
		goto touchkey_fw_update_out;
	}

	info->fw_update_state = 1;
	disable_irq(info->irq);
	info->enabled = false;

	ret = abov_flash_fw(info, false, cmd);
	if (ret) {
		input_err(true, &client->dev, "%s fail\n", __func__);
		/* info->fw_update_state = 2; */
		info->fw_update_state = 0;

	} else {
		input_info(true, &client->dev, "%s success\n", __func__);
		info->fw_update_state = 0;
	}

	if (info->flip_mode)
		abov_mode_enable(client, ABOV_FLIP, CMD_FLIP_ON);
#ifdef GLOVE_MODE
	else if (info->glovemode)
		abov_mode_enable(client, ABOV_GLOVE, CMD_GLOVE_ON);
#endif
	if (info->keyboard_mode)
		abov_mode_enable(client, ABOV_KEYBOARD, CMD_MOBILE_KBD_ON);

	info->enabled = true;
	enable_irq(info->irq);

touchkey_fw_update_out:
	input_dbg(true, &client->dev, "%s : %d\n", __func__,
		  info->fw_update_state);

	return count;
}

static ssize_t touchkey_fw_update_status(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int count = 0;

	input_info(true, &client->dev, "%s : %d\n", __func__,
		   info->fw_update_state);

	if (info->fw_update_state == 0)
		count = snprintf(buf, PAGE_SIZE, "PASS\n");
	else if (info->fw_update_state == 1)
		count = snprintf(buf, PAGE_SIZE, "Downloading\n");
	else if (info->fw_update_state == 2)
		count = snprintf(buf, PAGE_SIZE, "Fail\n");

	return count;
}

#ifdef GLOVE_MODE
static ssize_t abov_glove_mode(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int scan_buffer;
	int ret;
	u8 cmd;

	ret = sscanf(buf, "%d", &scan_buffer);
	if (ret != 1) {
		input_err(true, &client->dev, "%s: cmd read err\n", __func__);
		return count;
	}

	if (!(scan_buffer == 0 || scan_buffer == 1)) {
		input_err(true, &client->dev, "%s: wrong command(%d)\n",
			  __func__, scan_buffer);
		return count;
	}

	if (!info->enabled)
		return count;

	if ((scan_buffer == 0) && (info->keyboard_mode)) {
		input_err(true, &client->dev, "%s: keyboard mode is enabled\n",
			  __func__);
		info->glovemode = scan_buffer;
		return count;
	}

	if (info->glovemode == scan_buffer) {
		input_err(true, &client->dev, "%s same command(%d)\n",
			  __func__, scan_buffer);
		return count;
	}

	if (scan_buffer == 1) {
		input_info(true, &client->dev, "%s glove mode\n", __func__);
		cmd = CMD_GLOVE_ON;
	} else {
		input_info(true, &client->dev, "%s normal mode\n", __func__);
		cmd = CMD_GLOVE_OFF;
	}

	ret = abov_mode_enable(client, ABOV_GLOVE, cmd);
	if (ret < 0) {
		input_err(true, &client->dev, "%s fail(%d)\n", __func__, ret);
		return count;
	}

	info->glovemode = scan_buffer;

	return count;
}

static ssize_t abov_glove_mode_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", info->glovemode);
}
#endif

static ssize_t keyboard_cover_mode_enable(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int keyboard_mode_on;
	int ret;
	u8 cmd;

	input_dbg(true, &client->dev, "%s : Mobile KBD sysfs node called\n",
		  __func__);

	sscanf(buf, "%d", &keyboard_mode_on);
	input_info(true, &client->dev, "%s : %d\n", __func__, keyboard_mode_on);

	if (!(keyboard_mode_on == 0 || keyboard_mode_on == 1)) {
		input_err(true, &client->dev, "%s: wrong command(%d)\n",
			  __func__, keyboard_mode_on);
		return count;
	}

	if (!info->enabled)
		goto out;

#ifdef GLOVE_MODE
	if ((keyboard_mode_on == 0) && (info->glovemode)) {
		input_err(true, &client->dev, "%s: glove mode is enabled\n",
			  __func__);
		info->keyboard_mode = keyboard_mode_on;
		goto out;
	}
#endif

	if (info->keyboard_mode == keyboard_mode_on) {
		input_err(true, &client->dev, "%s same command(%d)\n",
			  __func__, keyboard_mode_on);
		goto out;
	}

	if (keyboard_mode_on == 1) {
		cmd = CMD_MOBILE_KBD_ON;
	} else {
		cmd = CMD_MOBILE_KBD_OFF;
	}

	/* mobile keyboard use same register with glove mode */
	ret = abov_mode_enable(client, ABOV_KEYBOARD, cmd);
	if (ret < 0) {
		input_err(true, &client->dev, "%s fail(%d)\n", __func__, ret);
		goto out;
	}

out:
	info->keyboard_mode = keyboard_mode_on;
	return count;
}

static ssize_t flip_cover_mode_enable(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct abov_tk_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int flip_mode_on;
	int ret;
	u8 cmd;

	sscanf(buf, "%d\n", &flip_mode_on);
	input_info(true, &client->dev, "%s : %d\n", __func__, flip_mode_on);

	if (!info->enabled)
		goto out;
	/* glove mode control */
	if (flip_mode_on) {
		cmd = CMD_FLIP_ON;
	} else {
#ifdef GLOVE_MODE
		if (info->glovemode)
			cmd = CMD_GLOVE_ON;
#endif
		cmd = CMD_FLIP_OFF;
	}

#ifdef GLOVE_MODE
	if (info->glovemode) {
		ret = abov_mode_enable(client, ABOV_GLOVE, cmd);
		if (ret < 0) {
			input_err(true, &client->dev,
				  "%s glove mode fail(%d)\n", __func__, ret);
			goto out;
		}
	} else
#endif
	{
		ret = abov_mode_enable(client, ABOV_FLIP, cmd);
		if (ret < 0) {
			input_err(true, &client->dev, "%s flip mode fail(%d)\n",
				  __func__, ret);
			goto out;
		}
	}

out:
	info->flip_mode = flip_mode_on;
	return count;
}

static DEVICE_ATTR(touchkey_threshold, S_IRUGO, touchkey_threshold_show, NULL);
static DEVICE_ATTR(brightness, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		   touchkey_led_control);
static DEVICE_ATTR(touchkey_recent, S_IRUGO, touchkey_menu_show, NULL);
static DEVICE_ATTR(touchkey_back, S_IRUGO, touchkey_back_show, NULL);
static DEVICE_ATTR(touchkey_recent_raw, S_IRUGO, touchkey_menu_raw_show, NULL);
static DEVICE_ATTR(touchkey_back_raw, S_IRUGO, touchkey_back_raw_show, NULL);
static DEVICE_ATTR(touchkey_firm_version_phone, S_IRUGO, bin_fw_ver, NULL);
static DEVICE_ATTR(touchkey_firm_version_panel, S_IRUGO, read_fw_ver, NULL);
static DEVICE_ATTR(touchkey_firm_update, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		   touchkey_fw_update);
static DEVICE_ATTR(touchkey_firm_update_status, S_IRUGO | S_IWUSR | S_IWGRP,
		   touchkey_fw_update_status, NULL);
#ifdef GLOVE_MODE
static DEVICE_ATTR(glove_mode, S_IRUGO | S_IWUSR | S_IWGRP,
		   abov_glove_mode_show, abov_glove_mode);
#endif
static DEVICE_ATTR(keyboard_mode, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		   keyboard_cover_mode_enable);
static DEVICE_ATTR(flip_mode, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		   flip_cover_mode_enable);
#ifdef CONFIG_INPUT_BOOSTER
static DEVICE_ATTR(boost_level, S_IWUSR | S_IWGRP, NULL, boost_level_store);
#endif

static struct attribute *sec_touchkey_attributes[] = {
	&dev_attr_touchkey_threshold.attr,
	&dev_attr_brightness.attr,
	&dev_attr_touchkey_recent.attr,
	&dev_attr_touchkey_back.attr,
	&dev_attr_touchkey_recent_raw.attr,
	&dev_attr_touchkey_back_raw.attr,
	&dev_attr_touchkey_firm_version_phone.attr,
	&dev_attr_touchkey_firm_version_panel.attr,
	&dev_attr_touchkey_firm_update.attr,
	&dev_attr_touchkey_firm_update_status.attr,
#ifdef GLOVE_MODE
	&dev_attr_glove_mode.attr,
#endif
	&dev_attr_keyboard_mode.attr,
	&dev_attr_flip_mode.attr,
#ifdef CONFIG_INPUT_BOOSTER
	&dev_attr_boost_level.attr,
#endif
	NULL,
};

static struct attribute_group sec_touchkey_attr_group = {
	.attrs = sec_touchkey_attributes,
};

static int abov_tk_fw_check(struct abov_tk_info *info)
{
	struct i2c_client *client = info->client;
	int ret;
	bool force = false;
	ret = get_tk_fw_version(info, true);
	if (ret) {
		input_err(true, &client->dev,
			  "%s: i2c fail...[%d], addr[%d]\n",
			  __func__, ret, info->client->addr);
#ifdef LED_TWINKLE_BOOTING
		/* regard I2C fail & LCD attached status as no TKEY device */
		if (!lcdtype) {
			input_err(true, &client->dev,
				  "%s : LCD is not attached\n", __func__);
			return ret;
		}
#endif
	}

	ret = abov_load_fw(info, BUILT_IN);
	if (ret) {
		input_err(true, &client->dev, "%s fw load fail\n", __func__);
		return -1;
	}
	abov_release_fw(info, BUILT_IN);

	if ((info->dtdata->forced_update) && (info->fw_ver != info->fw_ver_bin)) {
		input_err(true, &client->dev,
			  "define forced update(not equal & forced). Do force FW update\n");
		force = true;
	}
#ifdef FORCE_FW_UPDATE_DIFF_MODULE
	if (info->md_ver != info->md_ver_bin) {
		input_err(true, &client->dev,
			  "MD version is different.(IC %x, BN %x). Do force FW update\n",
			  info->md_ver, info->md_ver_bin);
		force = true;
	}
#endif

	if (info->fw_ver < info->fw_ver_bin || info->fw_ver > 0xf0
	    || force == true) {
		input_err(true, &client->dev,
			  "excute tk firmware update (0x%x -> 0x%x)\n",
			  info->fw_ver, info->fw_ver_bin);
		ret = abov_flash_fw(info, true, BUILT_IN);
		if (ret) {
			input_err(true, &client->dev,
				  "failed to abov_flash_fw (%d)\n", ret);
		} else {
			input_err(true, &client->dev, "fw update success\n");
		}
	}

	return ret;
}

static int abov_power(struct abov_tk_info *info, bool on)
{
	struct abov_touchkey_dt_data *dtdata = info->dtdata;
	static bool reg_boot_on = true;
	int ret = 0;

	if (dtdata->gpio_en_flag) {
		gpio_direction_output(dtdata->gpio_en, on);
	} else {
		/* 3.3V on,off control.*/
		if (!IS_ERR_OR_NULL(dtdata->avdd_vreg)) {
			if (on) {
				ret = regulator_enable(dtdata->avdd_vreg);
				if (ret) {
					pr_err
					    ("%s [TKEY] %s: 3p3 enable fail ret=%d\n",
					     SECLOG, __func__, ret);
				}
				reg_boot_on = false;
			} else {
				ret = regulator_disable(dtdata->avdd_vreg);
				if (ret) {
					pr_err
					    ("%s [TKEY] %s: 3p3 disable fail ret=%d\n",
					     SECLOG, __func__, ret);
				}
			}
		}
		/*1.8V on,off control.*/
		if (!IS_ERR_OR_NULL(dtdata->vdd_io_vreg)) {
			if (on)
				ret = regulator_enable(dtdata->vdd_io_vreg);
			else
				ret = regulator_disable(dtdata->vdd_io_vreg);

			if (ret)
				pr_err
				    ("%s [TKEY] %s: iovdd reg %s fail ret=%d\n",
				     SECLOG, __func__,
				     on ? "enable" : "disable", ret);
		}

		pr_info("%s [TKEY] %s %s", SECLOG, __func__, on ? "ON" : "OFF");
		if (!IS_ERR_OR_NULL(dtdata->avdd_vreg))
			pr_cont(", avdd is %s",
				regulator_is_enabled(dtdata->avdd_vreg) ? "ON" :
				"OFF");
		if (!IS_ERR_OR_NULL(dtdata->vdd_io_vreg))
			pr_cont(", vdd_io is %s",
				regulator_is_enabled(dtdata->vdd_io_vreg) ? "ON"
				: "OFF");
		pr_cont("\n");
	}

	if (!dtdata->vdd_led) {
		pr_err("%s [TKEY] %s VDD get regulator\n", __func__, SECLOG);
		dtdata->vdd_led = devm_regulator_get(&info->client->dev, "abov,lvdd");
		if (IS_ERR(dtdata->vdd_led)) {
			pr_err("%s [TKEY] %s annot get vdd\n", __func__, SECLOG);
			dtdata->vdd_led = NULL;
			return -ENOMEM;
		}

		if (!regulator_get_voltage(dtdata->vdd_led))
			regulator_set_voltage(dtdata->vdd_led, 3300000, 3300000);
	}

	
	if (on) {
		if (regulator_is_enabled(dtdata->vdd_led)) {
			pr_err("%s [TKEY] %s Regulator already enabled\n", __func__, SECLOG);
			return 0;
		}
		ret = regulator_enable(dtdata->vdd_led);
		if (ret)
			pr_err("%s [TKEY] %s failed to enable\n", __func__, SECLOG);
		usleep_range(10000, 11000);
	} else {
		ret = regulator_disable(dtdata->vdd_led);
		if (ret)
			pr_err("%s [TKEY] %s failed to disabl\n", __func__, SECLOG);
	}
	return ret;
}

static int abov_pinctrl_configure(struct abov_tk_info *info, bool active)
{
	struct pinctrl_state *set_state;
	int retval;

	if (active) {
		set_state =
		    pinctrl_lookup_state(info->pinctrl, "touchkey_active");
		if (IS_ERR(set_state)) {
			input_err(true, &info->client->dev,
				  "%s: cannot get ts pinctrl active state\n",
				  __func__);
			return PTR_ERR(set_state);
		}
	} else {
		set_state =
		    pinctrl_lookup_state(info->pinctrl, "touchkey_suspend");
		if (IS_ERR(set_state)) {
			input_err(true, &info->client->dev,
				  "%s: cannot get gpiokey pinctrl sleep state\n",
				  __func__);
			return PTR_ERR(set_state);
		}
	}
	retval = pinctrl_select_state(info->pinctrl, set_state);
	if (retval) {
		input_err(true, &info->client->dev,
			  "%s: cannot set ts pinctrl active state\n", __func__);
		return retval;
	}

	return 0;
}

static int abov_gpio_reg_init(struct device *dev,
			      struct abov_touchkey_dt_data *dtdata)
{
	int ret = 0;

	if (gpio_is_valid(dtdata->gpio_int)) {
		ret = gpio_request(dtdata->gpio_int, "tkey_gpio_int");
		if (ret < 0) {
			input_err(true, dev, "unable to request gpio_int\n");
			return ret;
		}
	}

	if (gpio_is_valid(dtdata->sub_det)) {
		ret = gpio_request(dtdata->sub_det, "gpio_tkey_sub_det");
		if (ret < 0) {
			input_err(true, dev,
				  "unable to request gpio_tkey_sub_det. ignoring\n");
			ret = 0;
		}
	}

	dtdata->vdd_io_vreg = regulator_get(dev, "vddo");
	if (IS_ERR(dtdata->vdd_io_vreg)) {
		dtdata->vdd_io_vreg = NULL;
		input_err(true, dev,
			  "dtdata->vdd_io_vreg get error, ignoring\n");
	} else {
		regulator_set_voltage(dtdata->vdd_io_vreg, 1800000, 1800000);
	}

	dtdata->avdd_vreg = regulator_get(dev, "avdd");
	if (IS_ERR(dtdata->avdd_vreg)) {
		input_err(true, dev, "dtdata->avdd_vreg get error\n");
		ret = -ENODEV;
	} else {
		regulator_set_voltage(dtdata->avdd_vreg, 3300000, 3300000);
	}

	dtdata->power = abov_power;

	return ret;
}

#ifdef CONFIG_OF
static int abov_parse_dt(struct device *dev,
			 struct abov_touchkey_dt_data *dtdata)
{
	struct device_node *np = dev->of_node;
	int rc;

	dtdata->gpio_int = of_get_named_gpio(np, "abov,irq-gpio", 0);
	if (!gpio_is_valid(dtdata->gpio_int)) {
		input_err(true, dev, "unable to get gpio_int\n");
		return dtdata->gpio_int;
	}

	dtdata->gpio_scl = of_get_named_gpio(np, "abov,scl-gpio", 0);
	if (!gpio_is_valid(dtdata->gpio_scl)) {
		input_err(true, dev, "unable to get gpio_scl\n");
		return dtdata->gpio_scl;
	}

	dtdata->gpio_sda = of_get_named_gpio(np, "abov,sda-gpio", 0);
	if (!gpio_is_valid(dtdata->gpio_sda)) {
		input_err(true, dev, "unable to get gpio_sda\n");
		return dtdata->gpio_sda;
	}

	dtdata->sub_det = of_get_named_gpio(np, "abov,sub-det", 0);
	if (!gpio_is_valid(dtdata->sub_det)) {
		input_info(true, dev, "unable to get sub_det\n");
	} else {
		input_info(true, dev, "%s: sub_det:%d\n", __func__,
			   dtdata->sub_det);
	}


	dtdata->gpio_en = of_get_named_gpio(np, "abov,gpio-en", 0);
	if (gpio_is_valid(dtdata->gpio_en)) {
		input_info(true, dev, "%s: sucessed to get gpio_en\n",
			   __func__);
		dtdata->gpio_en_flag = 1;
	}

	dtdata->reg_boot_on = of_property_read_bool(np, "abov,reg-boot-on");

	rc = of_property_read_string(np, "abov,fw-name", &dtdata->fw_name);
	if (rc < 0) {
		input_info(true, dev, "%s: Unable to read abov,fw_name\n",
			   __func__);
		return rc;
	}

	dtdata->forced_update = of_property_read_bool(np, "abov,forced-update");
	input_info(true, dev, "%s: gpio_int:%d, gpio_scl:%d,"
		   " gpio_sda:%d, reg_boot_on:%d, gpio_en:%d fw_name:%s, %s\n",
		   __func__, dtdata->gpio_int, dtdata->gpio_scl,
		   dtdata->gpio_sda,dtdata->reg_boot_on, dtdata->gpio_en, dtdata->fw_name,
		   dtdata->forced_update ? "F" : "");

	return 0;
}
#else
static int abov_parse_dt(struct device *dev,
			 struct abov_touchkey_dt_data *dtdata)
{
	return -ENODEV;
}
#endif

static int abov_tk_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct abov_tk_info *info;
	struct input_dev *input_dev;
	int ret = 0;

	input_err(true, &client->dev, "%s++\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		input_err(true, &client->dev,
			  "%s: i2c_check_functionality fail\n", __func__);
		return -EIO;
	}

	info = kzalloc(sizeof(struct abov_tk_info), GFP_KERNEL);
	if (!info) {
		input_err(true, &client->dev, "%s: Failed to allocate memory\n",
			  __func__);
		ret = -ENOMEM;
		goto err_alloc;
	}

	input_dev = input_allocate_device();
	if (!input_dev) {
		input_err(true, &client->dev,
			  "%s: Failed to allocate memory for input device\n",
			  __func__);
		ret = -ENOMEM;
		goto err_input_alloc;
	}

	info->client = client;
	info->input_dev = input_dev;
	info->probe_done = false;

	if (client->dev.of_node) {
		struct abov_touchkey_dt_data *dtdata;
		dtdata = devm_kzalloc(&client->dev,
				      sizeof(struct abov_touchkey_dt_data),
				      GFP_KERNEL);
		if (!dtdata) {
			input_err(true, &client->dev,
				  "%s: Failed to allocate memory\n", __func__);
			ret = -ENOMEM;
			goto err_config;
		}

		ret = abov_parse_dt(&client->dev, dtdata);
		if (ret)
			goto err_config;

		info->dtdata = dtdata;
	} else
		info->dtdata = client->dev.platform_data;

	if (info->dtdata == NULL) {
		input_err(true, &client->dev,
			  "%s: failed to get platform data\n", __func__);
		goto err_config;
	}

	/* Get pinctrl if target uses pinctrl */
	info->pinctrl = devm_pinctrl_get(&client->dev);
	if (IS_ERR(info->pinctrl)) {
		if (PTR_ERR(info->pinctrl) == -EPROBE_DEFER)
			goto err_config;

		input_err(true, &client->dev,
			  "%s: Target does not use pinctrl\n", __func__);
		info->pinctrl = NULL;
	}

	if (info->pinctrl) {
		ret = abov_pinctrl_configure(info, true);
		if (ret)
			input_err(true, &client->dev,
				  "%s: cannot set ts pinctrl active state\n",
				  __func__);
	}

	ret = abov_gpio_reg_init(&client->dev, info->dtdata);
	if (ret) {
		input_err(true, &client->dev, "%s: failed to init reg\n",
			  __func__);
		goto pwr_config;
	}
	if (info->dtdata->power)
		info->dtdata->power(info, true);

	if (!info->dtdata->reg_boot_on)
		msleep(ABOV_RESET_DELAY);

	if (gpio_is_valid(info->dtdata->sub_det)) {
		ret = gpio_get_value(info->dtdata->sub_det);
		if (ret) {
			input_err(true, &client->dev,
				  "%s: Device wasn't connected to board\n",
				  __func__);
#ifdef CONFIG_SEC_FACTORY
			ret = -ENODEV;
			goto err_i2c_check;
#endif
		}
	}

	info->enabled = true;
	info->irq = -1;
	client->irq = gpio_to_irq(info->dtdata->gpio_int);
	mutex_init(&info->lock);
	mutex_init(&info->device);

	info->touchkey_count = sizeof(touchkey_keycode) / sizeof(int);
	i2c_set_clientdata(client, info);

	ret = abov_tk_fw_check(info);
	if (ret) {
		input_err(true, &client->dev,
			  "%s: failed to firmware check (%d)\n", __func__, ret);
		goto err_reg_input_dev;
	}

	snprintf(info->phys, sizeof(info->phys),
		 "%s/input0", dev_name(&client->dev));
	input_dev->name = "sec_touchkey";
	input_dev->phys = info->phys;
	input_dev->id.bustype = BUS_HOST;
	input_dev->dev.parent = &client->dev;
	input_dev->open = abov_tk_input_open;
	input_dev->close = abov_tk_input_close;

	set_bit(EV_KEY, input_dev->evbit);
	set_bit(KEY_RECENT, input_dev->keybit);
	set_bit(KEY_BACK, input_dev->keybit);
	set_bit(EV_LED, input_dev->evbit);
	set_bit(LED_MISC, input_dev->ledbit);
	input_set_drvdata(input_dev, info);

	INIT_DELAYED_WORK(&info->resume_work, abov_tk_resume_work);

	ret = input_register_device(input_dev);
	if (ret) {
		input_err(true, &client->dev,
			  "%s: failed to register input dev (%d)\n", __func__,
			  ret);
		goto err_reg_input_dev;
	}
#ifdef CONFIG_INPUT_BOOSTER
	info->tkey_booster = input_booster_allocate(INPUT_BOOSTER_ID_TKEY);
	if (!info->tkey_booster) {
		input_err(true, &client->dev,
			  "%s: failed to allocate input booster\n", __func__);
		goto err_alloc_booster_failed;
	}
#endif

	if (!info->dtdata->irq_flag) {
		input_err(true, &client->dev, "no irq_flag\n");
		ret = request_threaded_irq(client->irq, NULL, abov_tk_interrupt,
					   IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					   ABOV_TK_NAME, info);
	} else {
		ret = request_threaded_irq(client->irq, NULL, abov_tk_interrupt,
					   info->dtdata->irq_flag, ABOV_TK_NAME,
					   info);
	}
	if (ret < 0) {
		input_err(true, &client->dev,
			  "%s: Failed to register interrupt\n", __func__);
		goto err_req_irq;
	}
	info->irq = client->irq;

#ifdef CONFIG_SEC_SYSFS
	info->sec_touchkey = sec_device_create(info, "sec_touchkey");
#else
	info->sec_touchkey = device_create(sec_class, NULL,
					   (dev_t) (uintptr_t) &
					   info->sec_touchkey, info,
					   "sec_touchkey");
#endif

	if (IS_ERR(info->sec_touchkey))
		input_err(true, &client->dev,
			  "%s: Failed to create device for the touchkey sysfs\n",
			  __func__);

	ret = sysfs_create_group(&info->sec_touchkey->kobj,
				 &sec_touchkey_attr_group);
	if (ret)
		input_err(true, &client->dev,
			  "%s: Failed to create sysfs group\n", __func__);

	ret = sysfs_create_link(&info->sec_touchkey->kobj,
				&info->input_dev->dev.kobj, "input");
	if (ret < 0) {
		input_err(true, &info->client->dev,
			  "%s: Failed to create input symbolic link\n",
			  __func__);
	}
	dev_set_drvdata(info->sec_touchkey, info);

#ifdef LED_TWINKLE_BOOTING
	if (!lcdtype) {
		input_err(true, &client->dev,
			  "%s : LCD is not connected. so start LED twinkle \n",
			  __func__);
		INIT_DELAYED_WORK(&info->led_twinkle_work, led_twinkle_work);
		info->led_twinkle_check = 1;
		schedule_delayed_work(&info->led_twinkle_work,
				      msecs_to_jiffies(400));
	}
#endif

	input_err(true, &client->dev, "%s: done\n", __func__);
	info->probe_done = true;

	return 0;

err_req_irq:
#ifdef CONFIG_INPUT_BOOSTER
	input_booster_free(info->tkey_booster);
	info->tkey_booster = NULL;
err_alloc_booster_failed:
#endif
	input_unregister_device(input_dev);
err_reg_input_dev:
	mutex_destroy(&info->lock);
	mutex_destroy(&info->device);
#ifdef CONFIG_SEC_FACTORY
err_i2c_check:
#endif
	if (info->dtdata->power)
		info->dtdata->power(info, false);
pwr_config:
err_config:
	input_free_device(input_dev);
err_input_alloc:
	kfree(info);
err_alloc:
	if (ret >= 0)
		ret = -EIO;
	input_err(true, &client->dev, "%s is failed, ret=%d\n", __func__, ret);
	return ret;

}

#ifdef LED_TWINKLE_BOOTING
static void led_twinkle_work(struct work_struct *work)
{
	struct abov_tk_info *info = container_of(work, struct abov_tk_info,
						 led_twinkle_work.work);
	static bool led_on = 1;
	static int count;

	input_err(true, &info->client->dev, "%s, on=%d, c=%d\n", __func__,
		  led_on, count++);

	if (info->led_twinkle_check == 1) {
		touchkey_led_set(info, led_on);

		if (led_on)
			led_on = 0;
		else
			led_on = 1;

		schedule_delayed_work(&info->led_twinkle_work,
				      msecs_to_jiffies(400));
	} else {
		if (led_on == 0)
			touchkey_led_set(info, 0);
	}
}
#endif

static void abov_tk_resume_work(struct work_struct *work)
{
	struct abov_tk_info *info = container_of(work, struct abov_tk_info,
						 resume_work.work);

	input_info(true, &info->client->dev, "%s\n", __func__);

	abov_tk_resume(&info->client->dev);

}

static int abov_tk_remove(struct i2c_client *client)
{
	struct abov_tk_info *info = i2c_get_clientdata(client);

	if (info->enabled)
		info->dtdata->power(info, false);

	info->enabled = false;
	if (info->irq >= 0)
		free_irq(info->irq, info);
	input_unregister_device(info->input_dev);
	input_free_device(info->input_dev);
#ifdef CONFIG_INPUT_BOOSTER
	input_booster_free(info->tkey_booster);
	info->tkey_booster = NULL;
#endif
	kfree(info);

	return 0;
}

static void abov_tk_shutdown(struct i2c_client *client)
{
	struct abov_tk_info *info = i2c_get_clientdata(client);
	u8 cmd = CMD_LED_OFF;

	input_info(true, &info->client->dev, "%s\n", __func__);

	cancel_delayed_work(&info->resume_work);

	if (info->enabled) {
		disable_irq(info->irq);
		abov_tk_i2c_write(client, ABOV_BTNSTATUS, &cmd, 1);
		info->enabled = false;
		if (info->dtdata->power)
			info->dtdata->power(info, false);
	}
}

static int abov_tk_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct abov_tk_info *info = i2c_get_clientdata(client);

	if (!info->enabled) {
		input_err(true, &info->client->dev, "%s: already power off\n",
			  __func__);
		return 0;
	}

	input_info(true, &info->client->dev, "%s: users=%d\n", __func__,
		   info->input_dev->users);

	mutex_lock(&info->device);

	disable_irq(info->irq);
	info->enabled = false;
	release_all_fingers(info);

	if (info->dtdata->power)
		info->dtdata->power(info, false);

	if (info->pinctrl)
		abov_pinctrl_configure(info, false);

	mutex_unlock(&info->device);

	return 0;
}

static int abov_tk_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct abov_tk_info *info = i2c_get_clientdata(client);
	u8 led_data;

	if (!info->probe_done)
		return 0;

	if (info->enabled) {
		input_err(true, &info->client->dev, "%s: already power on\n",
			  __func__);
		return 0;
	}

	input_info(true, &info->client->dev, "%s: users=%d\n", __func__,
		   info->input_dev->users);

	mutex_lock(&info->device);

	if (info->pinctrl)
		abov_pinctrl_configure(info, true);

	if (info->dtdata->power) {
		info->dtdata->power(info, true);
		msleep(ABOV_RESET_DELAY);
	} else
		get_tk_fw_version(info, true);

	info->enabled = true;

	if (info->flip_mode)
		abov_mode_enable(info->client, ABOV_FLIP, CMD_FLIP_ON);
#ifdef GLOVE_MODE
	else if (info->glovemode)
		abov_mode_enable(client, ABOV_GLOVE, CMD_GLOVE_ON);
#endif
	if (info->keyboard_mode)
		abov_mode_enable(info->client, ABOV_KEYBOARD,
				 CMD_MOBILE_KBD_ON);

	if (abov_touchled_cmd_reserved
	    && abov_touchkey_led_status == CMD_LED_ON) {
		abov_touchled_cmd_reserved = 0;
		led_data = abov_touchkey_led_status;

		abov_tk_i2c_write(client, ABOV_BTNSTATUS, &led_data, 1);

		input_info(true, &info->client->dev, "%s: LED reserved %s\n",
			   __func__, (led_data == CMD_LED_ON) ? "on" : "off");
	}
	enable_irq(info->irq);

	mutex_unlock(&info->device);

	return 0;
}

static int abov_tk_input_open(struct input_dev *dev)
{
	struct abov_tk_info *info = input_get_drvdata(dev);

	/* gpio_direction_input(info->dtdata->gpio_scl); */
	/* gpio_direction_input(info->dtdata->gpio_sda); */

	schedule_delayed_work(&info->resume_work, msecs_to_jiffies(1));

	/* abov_tk_resume(&info->client->dev); */

	return 0;
}

static void abov_tk_input_close(struct input_dev *dev)
{
	struct abov_tk_info *info = input_get_drvdata(dev);

#ifdef LED_TWINKLE_BOOTING
	info->led_twinkle_check = 0;
#endif

	cancel_delayed_work(&info->resume_work);
	abov_tk_suspend(&info->client->dev);
	/* gpio_set_value(info->dtdata->gpio_scl, 1); */
	/* gpio_set_value(info->dtdata->gpio_sda, 1); */
}

static const struct i2c_device_id abov_tk_id[] = {
	{ABOV_TK_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, abov_tk_id);

#ifdef CONFIG_OF
static struct of_device_id abov_match_table[] = {
	{.compatible = "abov,mc96ft16xx",},
	{},
};
#else
#define abov_match_table NULL
#endif

static struct i2c_driver abov_tk_driver = {
	.probe = abov_tk_probe,
	.remove = abov_tk_remove,
	.shutdown = abov_tk_shutdown,
	.driver = {
		   .name = ABOV_TK_NAME,
		   .of_match_table = abov_match_table,
		   },
	.id_table = abov_tk_id,
};

static int __init touchkey_init(void)
{
	pr_err("%s %s: abov,mc96ft16xx\n", SECLOG, __func__);
#if defined(CONFIG_SAMSUNG_LPM_MODE)
	if (poweroff_charging) {
		pr_notice("%s %s : LPM Charging Mode!!\n", SECLOG, __func__);
		return 0;
	}
#endif

	return i2c_add_driver(&abov_tk_driver);
}

static void __exit touchkey_exit(void)
{
	i2c_del_driver(&abov_tk_driver);
}

late_initcall(touchkey_init);
module_exit(touchkey_exit);
/* Module information */
MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("Touchkey driver for Abov MF16xx chip");
MODULE_LICENSE("GPL");
