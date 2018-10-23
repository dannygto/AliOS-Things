/*
 * Copyright (C) 2015-2018 Alibaba Group Holding Limited
 */

#include <string.h>

#include "core.h"
#include "transport.h"
#include "auth.h"
#include "extcmd.h"
#include "common.h"
#include "ble_service.h"
#include "breeze_hal_ble.h"

#include "utils.h"
#ifdef CONFIG_AIS_SECURE_ADV
#include "sha256.h"
#endif

// TODO: rm from bz core
#include "breeze_export.h"

#define FMSK_BLUETOOTH_VER_Pos 0
#define FMSK_OTA_Pos 2
#define FMSK_SECURITY_Pos 3
#define FMSK_SECRET_TYPE_Pos 4
#define FMSK_SIGNED_ADV_Pos 5

#define BZ_PROTOCOL_ID 0x05
#define MAC_ASCII_LEN 6

core_t *g_core;

#ifdef CONFIG_AIS_SECURE_ADV
#define AIS_SEQ_KV_KEY      "ais_adv_seq"
#define AIS_SEQ_UPDATE_FREQ (1 * 60 * 60) /* in second uint */
static uint32_t g_seq = 0;
#endif

void event_notify(uint8_t event_type)
{
    ali_event_t evt;

    evt.type = event_type;
    g_core->event_handler(&evt);
}

breeze_otainfo_t g_ota_info;
void notify_ota_command(uint8_t cmd, uint8_t num_frame, uint8_t *data, uint16_t len)
{
    ali_event_t evt;

    if ((cmd & BZ_CMD_TYPE_MASK) != BZ_CMD_TYPE_OTA) {
        return;
    }

    g_ota_info.type = OTA_CMD;
    g_ota_info.cmd_evt.m_cmd.cmd = cmd;
    g_ota_info.cmd_evt.m_cmd.frame = num_frame;
    memcpy(g_ota_info.cmd_evt.m_cmd.data, data, len);
    g_ota_info.cmd_evt.m_cmd.len = len;
    /* send event to higher layer. */
    evt.type                = BZ_EVENT_OTAINFO;
    evt.data.rx_data.p_data = &g_ota_info;
    evt.data.rx_data.length = sizeof(breeze_otainfo_t);
    g_core->event_handler(&evt);
}

void notify_ota_event(uint8_t ota_evt, uint8_t sub_evt)
{
    ali_event_t evt;
    if(ota_evt == ALI_OTA_ON_TX_DONE){
         uint8_t cmd = sub_evt;
         if (!(cmd == BZ_CMD_OTA_CHECK_RESULT || cmd == BZ_CMD_ERR || cmd == BZ_CMD_OTA_PUB_SIZE)) {
             return;
	 }
    }
    g_ota_info.type      =  OTA_EVT;
    g_ota_info.cmd_evt.m_evt.evt =  ota_evt;
    g_ota_info.cmd_evt.m_evt.d   =  sub_evt;

    /* send event to higher layer. */
    evt.type                = BZ_EVENT_OTAINFO;
    evt.data.rx_data.p_data = &g_ota_info;
    evt.data.rx_data.length = sizeof(breeze_otainfo_t);
    g_core->event_handler(&evt);
}

static void create_bz_adv_data(uint32_t model_id, uint8_t *mac_bin, bool enable_ota)
{
    uint16_t i;
    uint8_t  fmsk = 0;

    SET_U16_LE(g_core->adv_data, ALI_COMPANY_ID);
    i = sizeof(uint16_t);
    g_core->adv_data[i++] = BZ_PROTOCOL_ID;
    fmsk = BZ_BLUETOOTH_VER << FMSK_BLUETOOTH_VER_Pos;
#if BZ_ENABLE_AUTH
    fmsk |= 1 << FMSK_SECURITY_Pos;
#endif
    if (enable_ota) {
        fmsk |= 1 << FMSK_OTA_Pos;
    }
#ifndef CONFIG_MODEL_SECURITY
    fmsk |= 1 << FMSK_SECRET_TYPE_Pos;
#endif
#ifdef CONFIG_AIS_SECURE_ADV
    fmsk |= 1 << FMSK_SIGNED_ADV_Pos;
#endif
    g_core->adv_data[i++] = fmsk;

    SET_U32_LE(g_core->adv_data + i, model_id);
    i += sizeof(uint32_t);

    memcpy(&g_core->adv_data[i], mac_bin, 6);
    i += 6;
    g_core->adv_data_len = i;
}

static uint32_t tx_func_indicate(uint8_t cmd, uint8_t *p_data, uint16_t length)
{
    return transport_tx(TX_INDICATION, cmd, p_data, length);
}

static uint32_t ais_init(core_t *p_ali, ali_init_t const *p_init)
{
    ble_ais_init_t init_ais;

    g_core = p_ali;

    memset(&init_ais, 0, sizeof(ble_ais_init_t));
    init_ais.mtu = p_init->max_mtu;
    return ble_ais_init(&init_ais);
}

#ifdef CONFIG_AIS_SECURE_ADV
static void update_seq(void *arg)
{
    os_kv_set(AIS_SEQ_KV_KEY, &g_seq, sizeof(g_seq), 1);
    os_post_delayed_action(AIS_SEQ_UPDATE_FREQ, update_seq, NULL);
}


static void init_seq_number(uint32_t *seq)
{
    int len = sizeof(uint32_t);

    if (!seq)
        return;

    if (os_kv_get(AIS_SEQ_KV_KEY, seq, &len) != 0) {
        *seq = 0;
        len  = sizeof(uint32_t);
        os_kv_set(AIS_SEQ_KV_KEY, seq, len, 1);
    }

    os_post_delayed_action(AIS_SEQ_UPDATE_FREQ, update_seq, NULL);
}
#endif

ret_code_t core_init(void *p_ali_ext, ali_init_t const *p_init)
{
    core_t *p_ali = (core_t *)p_ali_ext;
    uint8_t  mac_be[BLE_MAC_LEN];
    uint32_t err_code;
    uint32_t size;

    ais_adv_init_t adv_data = {
        .flag = AIS_AD_GENERAL | AIS_AD_NO_BREDR,
        .name = { .ntype = AIS_ADV_NAME_FULL, .name = "AZ" },
    };

    if (p_ali == NULL || ((uint32_t)p_ali & 0x3) != 0) {
        return BZ_EINVALIDADDR;
    }

    memset(p_ali, 0, sizeof(core_t));
    p_ali->event_handler = p_init->event_handler;

#ifdef CONFIG_AIS_SECURE_ADV
    init_seq_number(&g_seq);
#endif

    /* Initialize Alibaba Information Service (AIS). */
    err_code = ais_init(p_ali, p_init);
    VERIFY_SUCCESS(err_code);

    ble_get_mac(mac_be);

    transport_init(p_init);
#if BZ_ENABLE_AUTH
    auth_init(p_init, tx_func_indicate);
#endif

    extcmd_init(p_init, tx_func_indicate);
    create_bz_adv_data(p_init->model_id, mac_be, p_init->enable_ota);
    adv_data.vdata.len = sizeof(adv_data.vdata.data);
    err_code = get_bz_adv_data(adv_data.vdata.data, &(adv_data.vdata.len));
    if (err_code) {
        BREEZE_LOG_ERR("%s %d fail.\r\n", __func__, __LINE__);
        return AIS_ERR_INVALID_ADV_DATA;
    }

    /* append user adv data if any. */
    if (p_init->user_adv_len > 0) {
        size = sizeof(adv_data.vdata.data) - adv_data.vdata.len;
        if (size < p_init->user_adv_len) {
            BREEZE_LOG_ERR("Warning: no space for user adv data (expected %d but"
                   " only %d left). No user adv data set!!!\r\n",
                   p_init->user_adv_len, size);
        } else {
            memcpy(adv_data.vdata.data + adv_data.vdata.len,
                   p_init->user_adv_data, p_init->user_adv_len);
            adv_data.vdata.len += p_init->user_adv_len;
        }
    }
    ble_advertising_start(&adv_data);
    return BZ_SUCCESS;
}


void core_reset(void)
{
    auth_reset();
    transport_reset();
}

ret_code_t transport_packet(uint8_t type, uint8_t cmd, uint8_t *p_data, uint16_t length)
{
    if (length == 0 || length > BZ_MAX_PAYLOAD_SIZE) {
        return BZ_EDATASIZE;
    }

    if (cmd == 0) {
        cmd = BZ_CMD_STATUS;
    }
    return transport_tx(type, cmd, p_data, length);
}

void core_handle_err(uint8_t src, uint8_t code)
{
    uint8_t err;

    BREEZE_LOG_ERR("err at %04x, code %04x\r\n", src, code);
    switch (src & BZ_ERR_MASK) {
        case BZ_TRANS_ERR:
            if (code != BZ_EINTERNAL) {
                if (src == ALI_ERROR_SRC_TRANSPORT_FW_DATA_DISC) {
                    notify_ota_event(ALI_OTA_ON_DISCONTINUE_ERR, 0);
                }
                err = transport_tx(TX_NOTIFICATION, BZ_CMD_ERR, NULL, 0);
                if (err != BZ_SUCCESS) {
                    BREEZE_LOG_ERR("err at %04x, code %04x\r\n", ALI_ERROR_SRC_TRANSPORT_SEND, code);
                }
            }
            break;
        case BZ_AUTH_ERR:
            auth_reset();
            if (code == BZ_ETIMEOUT) {
                ble_disconnect(AIS_BT_REASON_REMOTE_USER_TERM_CONN);
            }
            break;
        case BZ_EXTCMD_ERR:
            break;
        default:
            BREEZE_LOG_ERR("unknow bz err\r\n");
            break;
    }
}

ret_code_t get_bz_adv_data(uint8_t *p_data, uint16_t *length)
{
#ifdef CONFIG_AIS_SECURE_ADV
    if (*length < (g_core->adv_data_len + 4 + 4)) {
#else
    if (*length < g_core->adv_data_len) {
#endif
        return BZ_ENOMEM;
    }

#ifdef CONFIG_AIS_SECURE_ADV
    uint8_t  sign[4];
    uint32_t seq;

    seq = (++g_seq);
    auth_calc_adv_sign(seq, sign);
    memcpy(p_data, g_core->adv_data, g_core->adv_data_len);
    memcpy(p_data + g_core->adv_data_len, sign, 4);
    memcpy(p_data + g_core->adv_data_len + 4, &seq, 4);
    *length = g_core->adv_data_len + 4 + 4;
#else
    memcpy(p_data, g_core->adv_data, g_core->adv_data_len);
    *length = g_core->adv_data_len;
#endif

    return BZ_SUCCESS;
}

#ifdef CONFIG_AIS_SECURE_ADV
void set_adv_sequence(uint32_t seq)
{
    g_seq = seq;
    os_kv_set(AIS_SEQ_KV_KEY, &g_seq, sizeof(g_seq), 1);
}
#endif
