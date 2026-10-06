// Copyright 2020 Espressif Systems (Shanghai) Co. Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _IOT_SCREEN_DRIVER_H_
#define _IOT_SCREEN_DRIVER_H_

#include "scr_interface_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Define all screen direction
 *
 */
typedef enum {
    /* @---> X
       |
       Y
    */
    SCR_DIR_LRTB,   /**< From left to right then from top to bottom, this consider as the original direction of the screen */

    /*  Y
        |
        @---> X
    */
    SCR_DIR_LRBT,   /**< From left to right then from bottom to top */

    /* X <---@
             |
             Y
    */
    SCR_DIR_RLTB,   /**< From right to left then from top to bottom */

    /*       Y
             |
       X <---@
    */
    SCR_DIR_RLBT,   /**< From right to left then from bottom to top */

    /* @---> Y
       |
       X
    */
    SCR_DIR_TBLR,   /**< From top to bottom then from left to right */

    /*  X
        |
        @---> Y
    */
    SCR_DIR_BTLR,   /**< From bottom to top then from left to right */

    /* Y <---@
             |
             X
    */
    SCR_DIR_TBRL,   /**< From top to bottom then from right to left */

    /*       X
             |
       Y <---@
    */
    SCR_DIR_BTRL,   /**< From bottom to top then from right to left */

    SCR_DIR_MAX,

    /* Another way to represent rotation with 3 bit*/
    SCR_MIRROR_X = 0x40, /**< Mirror X-axis */
    SCR_MIRROR_Y = 0x20, /**< Mirror Y-axis */
    SCR_SWAP_XY  = 0x80, /**< Swap XY axis */
} scr_dir_t;

/**
 * @brief The types of colors that can be displayed on the screen
 *
 */
typedef enum {
    SCR_COLOR_TYPE_MONO,     /**< The screen is monochrome */
    SCR_COLOR_TYPE_GRAY,     /**< The screen is gray */
    SCR_COLOR_TYPE_RGB565,   /**< The screen is colorful */
} scr_color_type_t;

/**
 * @brief configuration of screen controller
 *
 */
typedef struct {
    scr_interface_driver_t *interface_drv;   /*!< Interface driver for screen */
    int8_t pin_num_rst;                      /*!< Pin to hardreset LCD*/
    int8_t pin_num_bckl;                     /*!< Pin for control backlight */
    uint8_t rst_active_level;                /*!< Reset pin active level */
    uint8_t bckl_active_level;               /*!< Backlight active level */
    uint16_t width;                          /*!< Screen width */
    uint16_t height;                         /*!< Screen height */
    uint16_t offset_hor;                     /*!< Offset of horizontal */
    uint16_t offset_ver;                     /*!< Offset of vertical */
    scr_dir_t rotate;                        /*!< Screen rotate direction */
} scr_controller_config_t;

/**
 * @brief Information of screen
 *
 */
typedef struct {
    uint16_t width;                      /*!< Current screen width, it may change when apply to rotate */
    uint16_t height;                     /*!< Current screen height, it may change when apply to rotate */
    scr_dir_t dir;                       /*!< Current screen direction */
    scr_color_type_t color_type;         /*!< Color type of the screen, See scr_color_type_t struct */
    uint8_t bpp;                         /*!< Bits per pixel */
    const char *name;                    /*!< Name of the screen */
} scr_info_t;

#ifdef __cplusplus
}
#endif

#endif
