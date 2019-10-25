#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "byteorder.h"
#include "fmt.h"
#include "periph/gpio.h"
#include "periph/rtc.h"
#include "tfa_thw.h"
#include "tfa_thw_params.h"

#include "net/loramac.h"
#include "semtech_loramac.h"

#include "config.h"
#include "lorawan-keys.m01.h"

#define ENABLE_DEBUG        (0)
#include "debug.h"

/**
 * @name LoRaWAN payload buffer parameters
 *
 * 32     24    20            8          0
 *  |XXXXX| RES | DEVID                  |
 *  | WINDSPEED | TEMPERATURE | HUMIDITY |
 *
 * @{
 */
#define BUF_DEVID_M         (0xFFFFF)   /**< Mask for devid, 20 Bit */
#define BUF_TEMPWIND_M      (0xFFF)     /**< Mask for temp/wind data, 12 Bit */
#define BUF_HUMIDITY_M      (0xFF)      /**< Mask for humidity data, 8 Bit */

typedef union tfa_thw_lorawan_buf {
    uint8_t  u8[8];                 /**< raw buffer */
    uint64_t u64;
    struct {
        uint32_t humidity    : 8;   /**< humidity in % */
        uint32_t temperature : 12;  /**< temperature+500 in C x 10.0 */
        uint32_t windspeed   : 12;  /**< windspeed in kph x 10.0 */
        uint32_t id          : 32;  /**< device id, randomly generated */
    };
} tfa_thw_lorawan_buf_t;
/** @} */
#define DATALEN     (2U)
tfa_thw_t dev;
tfa_thw_data_t data[DATALEN];

semtech_loramac_t loramac;
static uint8_t deveui[LORAMAC_DEVEUI_LEN];
static uint8_t appeui[LORAMAC_APPEUI_LEN];
static uint8_t appkey[LORAMAC_APPKEY_LEN];

static volatile kernel_pid_t mpid = KERNEL_PID_UNDEF;

void sensor_setup(void)
{
    DEBUG("%s: init sensor ... ", __func__);
    if (tfa_thw_init(&dev, &tfa_thw_params[0])) {
        DEBUG("[FAIL]\n");
        /* PANIC or REBOOT ?!? */
    }
    else {
        DEBUG("[DONE]\n");
    }
}

bool lorawan_setup(semtech_loramac_t *loramac)
{
    DEBUG(". %s\n", __func__);
    /* Convert identifiers and application key */
    fmt_hex_bytes(deveui, LORA_DEVEUI);
    fmt_hex_bytes(appeui, LORA_APPEUI);
    fmt_hex_bytes(appkey, LORA_APPKEY);

    /* Initialize the loramac stack */
    semtech_loramac_init(loramac);
    semtech_loramac_set_deveui(loramac, deveui);
    semtech_loramac_set_appeui(loramac, appeui);
    semtech_loramac_set_appkey(loramac, appkey);
    /* Try to join by Over The Air Activation */
    DEBUG(".. LoRaWAN join: ");
    //LED1_ON;
    int ret = semtech_loramac_join(loramac, LORAMAC_JOIN_OTAA);
    if (ret != SEMTECH_LORAMAC_JOIN_SUCCEEDED) {
        DEBUG("[FAIL] error %d\n", ret);
        return false;
    }
    DEBUG("[DONE]\n");
    /* set datarate */
    semtech_loramac_set_dr(loramac, APP_LORAWAN_DATARATE);
    semtech_loramac_set_tx_mode(loramac, LORAMAC_TX_UNCNF);
    semtech_loramac_set_tx_port(loramac, APP_LORAWAN_TX_PORT);
    /* init sensor */
    sensor_setup();
    //LED1_OFF;
    return true;
}

void create_buf(uint32_t devid, uint16_t windspeed,
                uint16_t temperature, uint8_t humidity,
                tfa_thw_lorawan_buf_t *buf)
{
    DEBUG(". %s\n", __func__);
    /* reset buffer */
    memset(buf, 0, sizeof(tfa_thw_lorawan_buf_t));
    buf->id = devid;
    buf->windspeed = (windspeed & BUF_TEMPWIND_M);
    buf->temperature = (temperature & BUF_TEMPWIND_M);
    buf->humidity = (humidity & BUF_HUMIDITY_M);
}

void lorawan_send(semtech_loramac_t *loramac, uint8_t *buf, uint8_t len)
{
    DEBUG(". %s\n", __func__);
    /* try to send data */
    DEBUG(".. send: ");
    unsigned ret = semtech_loramac_send(loramac, buf, len);
    if (ret != SEMTECH_LORAMAC_TX_DONE)  {
        DEBUG("[FAIL] Cannot send data, ret code: %d\n", ret);
    }
    else {
        DEBUG("[DONE]\n");
    }
}

static void rtc_cb(void *arg)
{
    (void) arg;
    msg_t msg;
    msg.type = APP_MSG_ALARM;
    msg_send(&msg, mpid);
}

static void set_alarm(unsigned timeout)
{
    DEBUG(". %s\n", __func__);
    struct tm time;
    rtc_get_time(&time);
    /* set initial alarm */
    time.tm_sec += timeout;
    mktime(&time);
    rtc_set_alarm(&time, rtc_cb, NULL);
}

int main(void)
{
    printf("%s: booting ...\n", __func__);
    /* set all LEDs to off */
    LED0_OFF;
    LED1_OFF;
    LED2_OFF;
    LED3_OFF;

    mpid = thread_getpid();
    bool joined = false;
    /* Schedule the next wake-up alarm */

    set_alarm(1);

    while(1) {
        DEBUG("%s: wait for message.\n", __func__);
        msg_t n;
        msg_receive(&n);
        if (n.type != APP_MSG_ALARM) {
            DEBUG("! ERROR !\n");
            continue;
        }
        if (!joined) {
            printf("%s: init network:\n", __func__);
            joined = lorawan_setup(&loramac);
        }
        if (joined) {
            printf("%s: running ...\n", __func__);
            DEBUG("%s: read data:\n",  __func__);
            //LED3_ON;
            if (tfa_thw_read(&dev, data, DATALEN) == 0) {
                if (data[0].id != data[1].id) {
                    DEBUG("! id mismatch !\n");
                }
                else if (data[0].type == data[1].type) {
                    DEBUG("! invalid data (1) !\n");
                }
                else if ((data[0].type + data[1].type) != 3) {
                    DEBUG("! invalid data (2) !\n");
                }
                else {
                    tfa_thw_lorawan_buf_t tbuf;
                    if (data[0].type == 1) { /* temperature and humidity in data[0] */
                        create_buf(data[0].id, data[1].tempwind, data[0].tempwind,
                                   data[0].humidity, &tbuf);
                    }
                    else {
                        create_buf(data[0].id, data[0].tempwind, data[1].tempwind,
                                   data[1].humidity, &tbuf);
                    }
                    //LED2_ON;
                    lorawan_send(&loramac, tbuf.u8, sizeof(tbuf.u8));
                    //LED2_OFF;
                }
            }
            else{
                DEBUG("! ERROR !\n");
            }
            //LED3_OFF;
        }
        /* Schedule the next wake-up alarm */
        if(joined) {
            set_alarm(APP_SLEEP_S);
        }
        else {
            /* join failed wait 5 min and try again */
            set_alarm(300);
        }
    }

    return 0;
}
