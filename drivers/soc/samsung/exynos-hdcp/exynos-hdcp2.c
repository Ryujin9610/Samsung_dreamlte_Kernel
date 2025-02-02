/*
 * drivers/soc/samsung/exynos-hdcp/exynos-hdcp.c
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/smc.h>
#include <asm/cacheflush.h>
#include <linux/exynos_ion.h>
#include <linux/smc.h>
#if defined(CONFIG_ION)
#include <linux/ion.h>
#endif
#include "exynos-hdcp2-tx-auth.h"
#include "exynos-hdcp2-teeif.h"
#include "exynos-hdcp2-selftest.h"
#include "exynos-hdcp2-encrypt.h"
#include "exynos-hdcp2-log.h"
#include "dp_link/exynos-hdcp2-dplink-if.h"
#include "dp_link/exynos-hdcp2-dplink.h"
#include "dp_link/exynos-hdcp2-dplink-selftest.h"

#define EXYNOS_HDCP_DEV_NAME	"hdcp2"

struct miscdevice hdcp;
struct hdcp_session_list g_hdcp_session_list;
static DEFINE_MUTEX(hdcp_lock);
enum hdcp_result hdcp_link_ioc_authenticate(void);
static char *hdcp_session_st_str[] = {
	"ST_INIT",
	"ST_LINK_SETUP",
	"ST_END",
	NULL
};

static char *hdcp_link_st_str[] = {
	"ST_INIT",
	"ST_H0_NO_RX_ATTACHED",
	"ST_H1_TX_LOW_VALUE_CONTENT",
	"ST_A0_DETERMINE_RX_HDCP_CAP",
	"ST_A1_EXCHANGE_MASTER_KEY",
	"ST_A2_LOCALITY_CHECK",
	"ST_A3_EXCHANGE_SESSION_KEY",
	"ST_A4_TEST_REPEATER",
	"ST_A5_AUTHENTICATED",
	"ST_A6_WAIT_RECEIVER_ID_LIST",
	"ST_A7_VERIFY_RECEIVER_ID_LIST",
	"ST_A8_SEND_RECEIVER_ID_LIST_ACK",
	"ST_A9_CONTENT_STREAM_MGT",
	"ST_END",
	NULL
};

#if defined(CONFIG_HDCP2_SUPPORT_IIA)
static uint32_t inst_num;
#endif
int state_init_flag;

enum hdcp_result hdcp_session_open(struct hdcp_sess_info *ss_info)
{
	struct hdcp_session_data *new_ss = NULL;
	struct hdcp_session_node *new_ss_node = NULL;

	/* do open session */
	new_ss_node = (struct hdcp_session_node *)kzalloc(sizeof(struct hdcp_session_node), GFP_KERNEL);
	if (!new_ss_node) {
		return TEMP_ERROR;
	}

	new_ss = hdcp_session_data_create();
	if (!new_ss) {
		kfree(new_ss_node);
		return TEMP_ERROR;
	}

	/* send session info to SWD */
	/* todo: add error check */

	UPDATE_SESSION_STATE(new_ss, SESS_ST_LINK_SETUP);
	ss_info->ss_id = new_ss->id;
	new_ss_node->ss_data = new_ss;

	hdcp_session_list_add((struct hdcp_session_node *)new_ss_node, (struct hdcp_session_list *)&g_hdcp_session_list);

	return HDCP_SUCCESS;
}

enum hdcp_result hdcp_session_close(struct hdcp_sess_info *ss_info)
{
	struct hdcp_session_node *ss_node;
	struct hdcp_session_data *ss_data;
	uint32_t ss_handle;

	ss_handle = ss_info->ss_id;

	ss_node = hdcp_session_list_find(ss_handle, &g_hdcp_session_list);
	if (!ss_node) {
		return TEMP_ERROR;
	}

	ss_data = ss_node->ss_data;
	if (ss_data->state != SESS_ST_LINK_SETUP)
		return TEMP_ERROR;

	ss_handle = ss_info->ss_id;
	UPDATE_SESSION_STATE(ss_data, SESS_ST_END);

	hdcp_session_list_del(ss_node, &g_hdcp_session_list);
	hdcp_session_data_destroy(&(ss_node->ss_data));

	return HDCP_SUCCESS;
}

enum hdcp_result hdcp_link_open(struct hdcp_link_info *link_info, uint32_t lk_type)
{
	struct hdcp_session_node *ss_node = NULL;
	struct hdcp_link_node *new_lk_node = NULL;
	struct hdcp_link_data *new_lk_data = NULL;
	int ret = HDCP_SUCCESS;
	uint32_t ss_handle;

	ss_handle = link_info->ss_id;

	do {
		/* find Session node which will contain new Link */
		ss_node = hdcp_session_list_find(ss_handle, &g_hdcp_session_list);
		if (!ss_node) {
			ret = HDCP_ERROR_INVALID_INPUT;
			break;
		}

		/* make a new link node and add it to the session */
		new_lk_node = (struct hdcp_link_node *)kzalloc(sizeof(struct hdcp_link_node), GFP_KERNEL);
		if (!new_lk_node) {
			ret = HDCP_ERROR_MALLOC_FAILED;
			break;
		}
		new_lk_data = hdcp_link_data_create();
		if (!new_lk_data) {
			ret = HDCP_ERROR_MALLOC_FAILED;
			break;
		}

		UPDATE_LINK_STATE(new_lk_data, LINK_ST_H0_NO_RX_ATTATCHED);

		new_lk_data->ss_ptr = ss_node;
		new_lk_data->lk_type = lk_type;
		new_lk_node->lk_data = new_lk_data;

		hdcp_link_list_add(new_lk_node, &ss_node->ss_data->ln);

		link_info->ss_id = ss_node->ss_data->id;
		link_info->lk_id = new_lk_data->id;
	} while (0);

	if (ret != HDCP_SUCCESS) {
		if (new_lk_node)
			kfree(new_lk_node);
		if (new_lk_data)
			hdcp_link_data_destroy(&new_lk_data);

		return TEMP_ERROR;
	}
	else {
		UPDATE_LINK_STATE(new_lk_data, LINK_ST_H1_TX_LOW_VALUE_CONTENT);
	}

	return HDCP_SUCCESS;
}

enum hdcp_result hdcp_link_close(struct hdcp_link_info *lk_info)
{
	struct hdcp_session_node *ss_node = NULL;
	struct hdcp_link_node *lk_node = NULL;

	/* find Session node which contain the Link */
	ss_node = hdcp_session_list_find(lk_info->ss_id, &g_hdcp_session_list);

	if (!ss_node)
		return HDCP_ERROR_INVALID_INPUT;

	lk_node = hdcp_link_list_find(lk_info->lk_id, &ss_node->ss_data->ln);
	if (!lk_node)
		return HDCP_ERROR_INVALID_INPUT;

	UPDATE_LINK_STATE(lk_node->lk_data, LINK_ST_H0_NO_RX_ATTATCHED);

	hdcp_link_list_del(lk_node, &ss_node->ss_data->ln);
	hdcp_link_data_destroy(&(lk_node->lk_data));

	return HDCP_SUCCESS;
}

#if defined(CONFIG_HDCP2_SUPPORT_IIA)
static enum hdcp_result hdcp_link_authenticate(struct hdcp_msg_info *msg_info){
	struct hdcp_session_node *ss_node;
	struct hdcp_link_node *lk_node;
	struct hdcp_link_data *lk_data;
	int ret = HDCP_SUCCESS;
	int rval = TX_AUTH_SUCCESS;
	int ake_retry = 0;
	int lc_retry = 0;

	/* find Session node which contains the Link */
	ss_node = hdcp_session_list_find(msg_info->ss_handle, &g_hdcp_session_list);
	if (!ss_node)
		return HDCP_ERROR_INVALID_INPUT;

	lk_node = hdcp_link_list_find(msg_info->lk_id, &ss_node->ss_data->ln);
	if (!lk_node)
		return HDCP_ERROR_INVALID_INPUT;

	lk_data = lk_node->lk_data;

	if (!lk_data)
		return HDCP_ERROR_INVALID_INPUT;

	/**
	 * if Upstream Content Control Function call this API,
	 * it changes state to ST_A0_DETERMINE_RX_HDCP_CAP automatically.
	 * HDCP library do not check CP desire.
	 */

	if (state_init_flag == 0){
		UPDATE_LINK_STATE(lk_data, LINK_ST_A0_DETERMINE_RX_HDCP_CAP);
	}

	if (lk_data->state == LINK_ST_A0_DETERMINE_RX_HDCP_CAP){
		if (determine_rx_hdcp_cap(lk_data) < 0) {
			ret = HDCP_ERROR_RX_NOT_HDCP_CAPABLE;
			UPDATE_LINK_STATE(lk_data, LINK_ST_H1_TX_LOW_VALUE_CONTENT);
		} else
			UPDATE_LINK_STATE(lk_data, LINK_ST_A1_EXCHANGE_MASTER_KEY);
	}

	switch (lk_data->state) {
		case LINK_ST_H1_TX_LOW_VALUE_CONTENT:
			break;
		case LINK_ST_A1_EXCHANGE_MASTER_KEY:
			rval = exchange_master_key(lk_data, msg_info);
			if (rval == TX_AUTH_SUCCESS) {
				if (msg_info->next_step == DONE) {
					ake_retry = 0;
					UPDATE_LINK_STATE(lk_data, LINK_ST_A2_LOCALITY_CHECK);
					msg_info->next_step = SEND_MSG;
					state_init_flag = 1;
				}
			} else {
				ret = HDCP_ERROR_EXCHANGE_KM;
				UPDATE_LINK_STATE(lk_data, LINK_ST_H1_TX_LOW_VALUE_CONTENT);
			}
			break;
		case LINK_ST_A2_LOCALITY_CHECK:
			rval = locality_check(lk_data, msg_info);
			if (rval == TX_AUTH_SUCCESS) {
				if (msg_info->next_step == DONE) {
					lc_retry = 0;
					UPDATE_LINK_STATE(lk_data, LINK_ST_A3_EXCHANGE_SESSION_KEY);
					msg_info->next_step = SEND_MSG;
				}
			} else {
				UPDATE_LINK_STATE(lk_data, LINK_ST_H1_TX_LOW_VALUE_CONTENT);
			}
			break;
		case LINK_ST_A3_EXCHANGE_SESSION_KEY:
			if (exchange_hdcp_session_key(lk_data, msg_info) < 0) {
				ret = HDCP_ERROR_EXCHANGE_KS;
				UPDATE_LINK_STATE(lk_data, LINK_ST_H1_TX_LOW_VALUE_CONTENT);
			} else{
				msg_info->next_step = WAIT_STATE;
				UPDATE_LINK_STATE(lk_data, LINK_ST_A4_TEST_REPEATER);
			}
			break;
		case LINK_ST_A4_TEST_REPEATER:
			if (evaluate_repeater(lk_data) == TRUE){
				/* HACK: when we supports repeater, it should be removed */
				UPDATE_LINK_STATE(lk_data, LINK_ST_A6_WAIT_RECEIVER_ID_LIST);
				msg_info->next_step = RP_RECIEVE_MSG;
			}
			else{
				/* if it is not a repeater, complete authentication */
				UPDATE_LINK_STATE(lk_data, LINK_ST_A5_AUTHENTICATED);
			}
			break;
		case LINK_ST_A5_AUTHENTICATED:
			msg_info->next_step = AUTH_FINISHED;
			/* Transmitter has completed the authentication protocol */
			UPDATE_LINK_STATE(lk_data, LINK_ST_H1_TX_LOW_VALUE_CONTENT);
			state_init_flag = 0;
			return HDCP_SUCCESS;
		case LINK_ST_A6_WAIT_RECEIVER_ID_LIST:
			ret = wait_for_receiver_id_list(lk_data, msg_info);
			if (ret < 0) {
				ret = HDCP_ERROR_WAIT_RECEIVER_ID_LIST;
				UPDATE_LINK_STATE(lk_data, LINK_ST_H1_TX_LOW_VALUE_CONTENT);
			} else {
				UPDATE_LINK_STATE(lk_data, LINK_ST_A7_VERIFY_RECEIVER_ID_LIST);
				msg_info->next_step = RP_SEND_MSG;
			}
			break;
		case LINK_ST_A7_VERIFY_RECEIVER_ID_LIST:
			UPDATE_LINK_STATE(lk_data, LINK_ST_A8_SEND_RECEIVER_ID_LIST_ACK);
			break;
		case LINK_ST_A8_SEND_RECEIVER_ID_LIST_ACK:
			ret = send_receiver_id_list_ack(lk_data, msg_info);
			if (ret < 0)
				UPDATE_LINK_STATE(lk_data, LINK_ST_H1_TX_LOW_VALUE_CONTENT);
			else
				UPDATE_LINK_STATE(lk_data, LINK_ST_A9_CONTENT_STREAM_MGT);
			break;
		case LINK_ST_A9_CONTENT_STREAM_MGT:
			/* do not support yet */
			ret = HDCP_ERROR_DO_NOT_SUPPORT_YET;
			UPDATE_LINK_STATE(lk_data, LINK_ST_H1_TX_LOW_VALUE_CONTENT);
			break;
		default:
			ret = HDCP_ERROR_INVALID_STATE;
			UPDATE_LINK_STATE(lk_data, LINK_ST_H1_TX_LOW_VALUE_CONTENT);
			break;
	}

	return ret;
}

static enum hdcp_result hdcp_link_stream_manage(struct hdcp_stream_info *stream_info)
{
        struct hdcp_session_node *ss_node;
        struct hdcp_link_node *lk_node;
        struct hdcp_link_data *lk_data;
        int ret = HDCP_SUCCESS;
        struct contents_info stream_ctrl;
        int rval = TX_AUTH_SUCCESS;
        int i;

        /* find Session node which contain the Link */
        ss_node = hdcp_session_list_find(stream_info->ss_id, &g_hdcp_session_list);
        if (!ss_node)
                return HDCP_ERROR_INVALID_INPUT;

        lk_node = hdcp_link_list_find(stream_info->lk_id, &ss_node->ss_data->ln);
        if (!lk_node)
                return HDCP_ERROR_INVALID_INPUT;

        lk_data = lk_node->lk_data;
        if (!lk_data)
                return HDCP_ERROR_INVALID_INPUT;

        if (lk_data->state < LINK_ST_A4_TEST_REPEATER)
                return HDCP_ERROR_INVALID_STATE;

        stream_ctrl.str_num = stream_info->num;
        if (stream_info->num > HDCP_TX_REPEATER_MAX_STREAM) {
                return HDCP_ERROR_INVALID_INPUT;
        }

        for (i = 0; i < stream_info->num; i++) {
                stream_ctrl.str_info[i].ctr = stream_info->stream_ctr;
                stream_ctrl.str_info[i].type = stream_info->type;
                stream_ctrl.str_info[i].pid = stream_info->stream_pid;
        }

        rval = manage_content_stream(lk_data, &stream_ctrl, stream_info);
        if (rval < 0) {
                hdcp_err("manage_content_stream fail(0x%08x)\n", ret);
                return HDCP_ERROR_STREAM_MANAGE;

        }

        /* todo : state change */

        return HDCP_SUCCESS;
}

static long hdcp_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int rval;

	switch (cmd) {
	case (uint32_t)HDCP_IOC_SESSION_OPEN:
	{
		struct hdcp_sess_info ss_info;
		if (hdcp_session_open(&ss_info))
			return TEMP_ERROR;

		if (copy_to_user((void __user *)arg, &ss_info, sizeof(struct hdcp_sess_info)))
			return TEMP_ERROR;
		break;
	}
	case (uint32_t)HDCP_IOC_SESSION_CLOSE:
	{
		/* todo: session close */
		struct hdcp_sess_info ss_info;

		if (copy_from_user(&ss_info, (void __user *)arg, sizeof(struct hdcp_sess_info)))
			return TEMP_ERROR;

		if (hdcp_session_close(&ss_info))
			return TEMP_ERROR;

		break;
	}
	case (uint32_t)HDCP_IOC_LINK_OPEN:
	{
		/* todo: link open */

		struct hdcp_link_info lk_info;

		if (copy_from_user(&lk_info, (void __user *)arg, sizeof(struct hdcp_link_info)))
			return TEMP_ERROR;

		if (hdcp_link_open(&lk_info, HDCP_LINK_TYPE_IIA))
			return TEMP_ERROR;

		if (copy_to_user((void __user *)arg, &lk_info, sizeof(struct hdcp_link_info)))
			return TEMP_ERROR;

		break;
	}
	case (uint32_t)HDCP_IOC_LINK_CLOSE:
	{
		/* todo: link close */
		struct hdcp_link_info lk_info;

		/* find Session node which contain the Link */
		if (copy_from_user(&lk_info, (void __user *)arg, sizeof(struct hdcp_link_info)))
			return TEMP_ERROR;

		if (hdcp_link_close(&lk_info))
			return TEMP_ERROR;

		break;
	}
	case (uint32_t)HDCP_IOC_LINK_AUTH:
	{
		struct hdcp_msg_info msg_info;

		if (copy_from_user(&msg_info, (void __user *)arg, sizeof(struct hdcp_msg_info)))
			return TEMP_ERROR;

		if (hdcp_link_authenticate(&msg_info))
			return TEMP_ERROR;

		if (copy_to_user((void __user *)arg, &msg_info, sizeof(struct hdcp_msg_info)))
			return TEMP_ERROR;

		break;
	}
	case (uint32_t)HDCP_IOC_LINK_ENC:
	{
		/* todo: link close */
		struct hdcp_enc_info enc_info;
		size_t packet_len = 0;
		uint8_t pes_priv[HDCP_PRIVATE_DATA_LEN];
		ion_phys_addr_t input_phys, output_phys;
		struct hdcp_session_node *ss_node;
		struct hdcp_link_node *lk_node;
		struct hdcp_link_data *lk_data;
		uint8_t  * input_virt,*output_virt;
		int ret = HDCP_SUCCESS;
		/* find Session node which contain the Link */
		if (copy_from_user(&enc_info, (void __user *)arg, sizeof(struct hdcp_enc_info)))
			return TEMP_ERROR;

		/* find Session node which contains the Link */
		ss_node = hdcp_session_list_find(enc_info.id, &g_hdcp_session_list);
		if (!ss_node){
			return HDCP_ERROR_INVALID_INPUT;
		}

		lk_node = hdcp_link_list_find(enc_info.id, &ss_node->ss_data->ln);
		if (!lk_node){
			return HDCP_ERROR_INVALID_INPUT;
		}

		lk_data = lk_node->lk_data;

		if (!lk_data){
			return HDCP_ERROR_INVALID_INPUT;
		}

		input_phys = (ion_phys_addr_t)(enc_info.input_phys ^ 0xFFFFFFFF00000000);
		output_phys = (ion_phys_addr_t)(enc_info.output_phys ^ 0xFFFFFFFF00000000);

		/* set input counters */
		memset(&(lk_data->tx_ctx.input_ctr), 0x00, HDCP_INPUT_CTR_LEN);
		/* set output counters */
		memset(&(lk_data->tx_ctx.str_ctr), 0x00, HDCP_STR_CTR_LEN);

		packet_len = (size_t)enc_info.input_len;
		input_virt =  (uint8_t *)phys_to_virt(input_phys);
		output_virt =  (uint8_t *)phys_to_virt(output_phys);
		__flush_dcache_area(input_virt, packet_len);
		__flush_dcache_area(output_virt, packet_len);

		ret = encrypt_packet(pes_priv,
				input_phys, packet_len,
				output_phys, packet_len,
				&(lk_data->tx_ctx));

		if (ret) {
			hdcp_err("encrypt_packet() is failed with 0x%x\n", ret);
			return -1;
		}

		if (copy_to_user((void __user *)arg, &enc_info, sizeof(struct hdcp_enc_info)))
			return TEMP_ERROR;
		break;
	}
	case (uint32_t)HDCP_IOC_STREAM_MANAGE:
	{
		struct hdcp_stream_info stream_info;

		if (copy_from_user(&stream_info, (void __user *)arg, sizeof(struct hdcp_stream_info)))
			return TEMP_ERROR;

		if (hdcp_link_stream_manage(&stream_info))
			return TEMP_ERROR;

		if (copy_to_user((void __user *)arg, &stream_info, sizeof(struct hdcp_stream_info)))
			return TEMP_ERROR;

		break;
	}
#if defined(CONFIG_HDCP2_EMULATION_MODE)
	case (uint32_t)HDCP_IOC_DPLINK_TX_EMUL:
	{
		uint32_t emul_cmd;
		if (copy_from_user(&emul_cmd, (void __user *)arg, sizeof(uint32_t)))
			return -EINVAL;

		return dplink_emul_handler(emul_cmd);
	}
#endif
	case (uint32_t)HDCP_IOC_DPLINK_TX_AUTH:
	{
		//rval = hdcp_dplink_authenticate();
		rval = dp_hdcp_protocol_self_test();
		if (rval) {
			hdcp_err("DP self_test fail. errno(%d)\n", rval);
			return rval;
		}
		else
			hdcp_err("DP self_test success!!\n");

#if defined(TEST_VECTOR_2)
		/* todo: support test vector 1 */
		rval = iia_hdcp_protocol_self_test();
		if (rval) {
			hdcp_err("IIA self_test failed. errno(%d)\n", rval);
			return rval;
		}
		else
			hdcp_err("IIA self_test success!!\n");
#endif

		return rval;
	}
	default:
		hdcp_err("HDCP: Invalid IOC num(%d)\n", cmd);
		return -ENOTTY;
	}

	return 0;
}

static int hdcp_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct device *dev = miscdev->this_device;
	struct hdcp_info *info;

	info = kzalloc(sizeof(struct hdcp_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	file->private_data = info;

	mutex_lock(&hdcp_lock);
	inst_num++;
	/* todo: hdcp device initialize ? */
	mutex_unlock(&hdcp_lock);

	return 0;
}

static int hdcp_release(struct inode *inode, struct file *file)
{
	struct hdcp_info *info = file->private_data;

	/* disable drm if we were the one to turn it on */
	mutex_lock(&hdcp_lock);
	inst_num--;
	/* todo: hdcp device finalize ? */
	mutex_unlock(&hdcp_lock);

	kfree(info);
	return 0;
}
#endif

static int __init hdcp_init(void)
{
	int ret;

	hdcp_info("hdcp2 driver init\n");

	ret = misc_register(&hdcp);
	if (ret) {
		hdcp_err("hdcp can't register misc on minor=%d\n",
				MISC_DYNAMIC_MINOR);
		return ret;
	}

	/* todo: do initialize sequence */
	hdcp_session_list_init(&g_hdcp_session_list);

	if (hdcp_dplink_init() < 0) {
		hdcp_err("hdcp_dplink_init fail\n");
		return -EINVAL;
	}

	ret = hdcp_tee_open();
	if (ret) {
		hdcp_err("hdcp_tee_open fail\n");
		return -EINVAL;
	}

	return 0;
}

static void __exit hdcp_exit(void)
{
	/* todo: do clear sequence */

	misc_deregister(&hdcp);
	hdcp_session_list_destroy(&g_hdcp_session_list);
	hdcp_tee_close();
}

#if defined(CONFIG_HDCP2_SUPPORT_IIA)
static const struct file_operations hdcp_fops = {
	.owner		= THIS_MODULE,
	.open		= hdcp_open,
	.release	= hdcp_release,
	.compat_ioctl = hdcp_ioctl,
	.unlocked_ioctl = hdcp_ioctl,
};
#endif

struct miscdevice hdcp = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= EXYNOS_HDCP_DEV_NAME,
#if defined(CONFIG_HDCP2_SUPPORT_IIA)
	.fops	= &hdcp_fops,
#endif
};

module_init(hdcp_init);
module_exit(hdcp_exit);
