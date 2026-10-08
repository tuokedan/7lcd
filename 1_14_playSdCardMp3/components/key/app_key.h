//=============
//头文件包含
//=============
#ifndef _APP_KEY_H_
#define _APP_KEY_H_


#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"


typedef enum
{
    BUTTON_IDLE = 0,
    BUTTON_MENU,
    BUTTON_PLAY,
    BUTTON_UP,
    BUTTON_DOWN
} button_name_t;

typedef struct
{
    button_name_t key; /**< button index on the channel */
    int min;           /**< min voltage in mv corresponding to the button */
    int max;           /**< max voltage in mv corresponding to the button */
} key_config_t;

extern void initKey();
extern void key_check_task();
extern int GetKey(void);


#endif

