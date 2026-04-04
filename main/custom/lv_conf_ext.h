/*
* Copyright 2023 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/

/*
 * lv_conf_ext.h for custom lvconf file.
 * Created on: Feb 8, 2023
 * example :
 *	#undef LV_FONT_FMT_TXT_LARGE
 *  #define LV_FONT_FMT_TXT_LARGE 1
 */

#ifndef LV_CONF_EXT_H
#define LV_CONF_EXT_H


/* common code  begin  */


/* common code end */


#if LV_USE_GUIDER_SIMULATOR
/* code for simulator begin  */


/* code for simulator end */
#else
/* code for board begin */

// Enable large font support for SIMYOU Chinese font
#define LV_FONT_FMT_TXT_LARGE 1

// Enable additional fonts
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_48 1

// 超高刷新率配置 - 接近120Hz
#undef LV_DEF_REFR_PERIOD
#define LV_DEF_REFR_PERIOD 10  // ~100Hz refresh rate for super smooth animation

// 优化渲染性能
#undef LV_USE_DISPLAY_MONITORS
#define LV_USE_DISPLAY_MONITORS 0

// 增加绘制缓冲区数量
#undef LV_DRAW_BUF_MAX_NUM
#define LV_DRAW_BUF_MAX_NUM 32

/* code for board end */	
#endif



#endif  /* LV_CONF_EXT_H */	