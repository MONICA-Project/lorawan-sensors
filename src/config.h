#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "net/loramac.h"

/* LoRaWAN application configurations */
#define LORAWAN_TX_PORT             (1U)
#define LORAWAN_DATARATE            LORAMAC_DR_2
/* we must respect the duty cycle limitations */
#define APP_SLEEP_S                 (37U)
#define APP_RESET_S                 (301U)

#define APP_MSG_ALARM               (0x6414)

#endif
