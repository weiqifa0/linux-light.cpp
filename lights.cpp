/******************************************************************/
/*  Copyright (C)  ROCK-CHIPS FUZHOU . All Rights Reserved.       */
/*******************************************************************
 * File    :   lights.cpp
 * Desc    :   Implement lights adjust HAL
 * Author  :   CMY
 * Date    :   2009-07-22
 * Notes   :   ..............
 *
 * Revision 1.00  2009/07/22 CMY
 * Revision 2.00 2012/01/08 yxj
 * support button charge lights
 * Revision 3.00 2016/11/01 yhx
 *
 * ...................
 * ********************************************************************/

#define LOG_TAG "Lights Hal"

#include <cutils/log.h>
#include <cutils/atomic.h>

#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <hardware/lights.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/signal.h>



#define LOGV(fmt,args...) ALOGV(fmt,##args)
#define LOGD(fmt,args...) ALOGD(fmt,##args)
#define LOGI(fmt,args...) ALOGI(fmt,##args)
#define LOGW(fmt,args...) ALOGW(fmt,##args)
#define LOGE(fmt,args...) ALOGE(fmt,##args)

/*****************************************************************************/
#define BACKLIGHT_PATH  "/sys/class/backlight/rk28_bl/brightness"
#define BACKLIGHT_PATH1 "/sys/class/backlight/backlight/brightness" // for kernel 4.4
#define BUTTON_LED_PATH "sys/class/leds/rk29_key_led/brightness"
#define BATTERY_LED_PATH "sys/class/leds/red/brightness"
/*add by xuliu for idpad for led begin*/
#define RED_LED_PATH "sys/class/leds/red/brightness"
#define GREEN_LED_PATH "sys/class/leds/green/brightness"
#define BLUE_LED_PATH "sys/class/leds/blue/brightness"
/*add by xuliu for idpad for led begin*/
/* BEGIN: Modified by 韦启发, 2019/1/3 */
#define RED_LED_TRIGGER_PATH "sys/class/leds/red/trigger"
#define RED_LED_DELAYOFF_PATH "/sys/devices/platform/gpio-leds/leds/red/delay_off"
#define RED_LED_DELAYON_PATH "/sys/devices/platform/gpio-leds/leds/red/delay_on"

#define BLUE_LED_TRIGGER_PATH "sys/class/leds/blue/trigger"
#define BLUE_LED_DELAYOFF_PATH "/sys/devices/platform/gpio-leds/leds/blue/delay_off"
#define BLUE_LED_DELAYON_PATH "/sys/devices/platform/gpio-leds/leds/blue/delay_on"

#define GREEN_LED_TRIGGER_PATH "sys/class/leds/green/trigger"
#define GREEN_LED_DELAYOFF_PATH "/sys/devices/platform/gpio-leds/leds/green/delay_off"
#define GREEN_LED_DELAYON_PATH "/sys/devices/platform/gpio-leds/leds/green/delay_on"

#define USE_TIGGER_TIMER (1) //使用trigger来实现闪烁
#define USE_PTHREAD_TIMER (!USE_TIGGER_TIMER) //使用线程方式来实现闪烁
/* END:  Modified by 韦启发, 2019/1/3 */
int g_bl_fd = 0;   //backlight fd
int g_btn_fd = 0; //button light fd
int g_bat_fd = 0; //battery charger fd
static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

#define LIGHT_FLASH_NONE 0
#define LIGHT_FLASH_TIMED 1
#define LIGHT_FLASH_HARDWARE 2

#define msleep(ms)    usleep((ms) * 1000)
char red,blue,green;
int onMS, offMS;

#if USE_PTHREAD_TIMER
pthread_t tid;
volatile int blink;
static void* BlinkThread(void* param);
#endif
static int light_device_open(const struct hw_module_t* module, const char* name, struct hw_device_t** device);


static void init_g_lock(void)
{
    pthread_mutex_init(&g_lock, NULL);
}

static int write_int(char const *path, int value)
{
    int fd;
    static int already_warned;

    already_warned = 0;

    LOGI("write_int: path %s, value %d", path, value);
    fd = open(path, O_RDWR);

    if (fd >= 0) {
        char buffer[20];
        int bytes = sprintf(buffer, "%d\n", value);
        int amt = write(fd, buffer, bytes);
            if(amt < 0)
                ALOGE(">>> write_int failed to write %s %d\n", path,bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            LOGE(">>> write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

static int rgb_to_brightness(struct light_state_t const *state)
{
    unsigned int color = state->color & 0x00ffffff;
    unsigned char brightness = ((77*((color>>16)&0x00ff)) +
        (150*((color>>8)&0x00ff)) + (29*(color&0x00ff))) >> 8;
    return brightness;
}

int set_backlight_light(struct light_device_t* dev, struct light_state_t const* state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    pthread_mutex_lock(&g_lock);
    //LOGI(">>> Enter set_backlight_light %d",brightness);
    err = write_int(BACKLIGHT_PATH1, brightness);
    if (err !=0)
        err = write_int(BACKLIGHT_PATH, brightness);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int set_keyboard_light(struct light_device_t* dev, struct light_state_t const* state)
{
    LOGI(">>> Enter set_keyboard_light");
    return 0;
}

int set_buttons_light(struct light_device_t* dev, struct light_state_t const* state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    pthread_mutex_lock(&g_lock);
    err = write_int(BUTTON_LED_PATH, brightness?1:0);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int write_str(char const* path, char* value)
{
    int fd;
    LOGI("write_str: path %s, value %s", path, value);
    fd = open(path, O_RDWR);
    if (fd >= 0) {
        char buffer[1024];
        int bytes = snprintf(buffer, sizeof(buffer), "%s\n", value);
        ssize_t amt = write(fd, buffer, (size_t)bytes);
        if(amt <0 )
             ALOGE(">>> write_str failed to write %s %d\n", path,bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        ALOGE(">>> write_str failed to open %s\n", path);
        return -errno;
    }
}



int set_battery_light(struct light_device_t* dev, struct light_state_t const* state)
{
    /*声明*/
    int err = 0;

    /*获取颜色值*/
    unsigned int colorRGB = state->color & 0x00ffffff;
    red = (colorRGB >> 16) & 0xFF;
    green = (colorRGB >> 8) & 0xFF;
    blue = colorRGB & 0xFF;

    /*打印关键值*/
    LOGI(">>> set_battery_light:0x%x state:%d",state->color,state->flashMode);
    switch (state->flashMode)
    {
        case LIGHT_FLASH_TIMED:
            onMS = state->flashOnMS;
            offMS = state->flashOffMS;
            break;
        case LIGHT_FLASH_NONE:
        default:
            onMS = 0;
            offMS = 0;
            break;
    }

    if (onMS > 0 && offMS > 0)
    {
        pthread_mutex_lock(&g_lock);
        #if USE_PTHREAD_TIMER
            blink = 1;//不用原来的方法，是用控制节点的方法
            LOGI(">>> set_battery_light use_pthread_timer blink:%d",blink);
        #else //USE_TIGGER_TIMER
            if(red)
            {
                err = write_str(RED_LED_TRIGGER_PATH,"timer");
                //下面的代码实际没设置成功，onMs和offMS在驱动设置
                err = write_int(RED_LED_DELAYON_PATH,onMS);
                err = write_int(RED_LED_DELAYOFF_PATH,offMS);
            }
            if(blue)
            {
                err = write_str(BLUE_LED_TRIGGER_PATH,"timer");
                err = write_int(BLUE_LED_DELAYON_PATH,onMS);
                err = write_int(BLUE_LED_DELAYOFF_PATH,offMS);
            }
            if(green)
            {
                err = write_str(GREEN_LED_TRIGGER_PATH,"timer");
                err = write_int(GREEN_LED_DELAYON_PATH,onMS);
                err = write_int(GREEN_LED_DELAYOFF_PATH,offMS);
            }
            LOGI(">>> set_battery_light use_tigger_timer onMs:%d offMs%d",onMS,offMS);
        #endif
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    else
    {
        #if USE_PTHREAD_TIMER
        blink = 0;/* blink=0 表示没有闪烁功能 */
        #endif
    }
    /*执行下面的代码需要锁住线程*/
    pthread_mutex_lock(&g_lock);
    err = write_str(RED_LED_TRIGGER_PATH,"none");
    err = write_str(BLUE_LED_TRIGGER_PATH,"none");
    err = write_str(GREEN_LED_TRIGGER_PATH,"none");
    err = write_int(RED_LED_PATH, red?1:0);
    err = write_int(BLUE_LED_PATH, blue?1:0);
    err = write_int(GREEN_LED_PATH, green?1:0);
    pthread_mutex_unlock(&g_lock);
    LOGI(">>> set_battery_light red:%d blue:%d green%d",red,blue,green);

    return 0;
}

int set_notifications_light(struct light_device_t* dev, struct light_state_t const* state)
{
    LOGI(">>> Enter set_notifications_light");
    return 0;
}

int set_attention_light(struct light_device_t* dev, struct light_state_t const* state)
{
    LOGI(">>> Enter set_attention_light");
    return 0;
}

/*add by xuliu for idpad for led begin*/
int set_redled_light(struct light_device_t* dev, struct light_state_t const* state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    LOGI(">>> set_redled_light");
    pthread_mutex_lock(&g_lock);
    err = write_int(RED_LED_PATH, brightness);
    if (err !=0)
        err = write_int(RED_LED_PATH, brightness);
    LOGI(">>> set_redled_light2");
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int set_greenled_light(struct light_device_t* dev, struct light_state_t const* state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    pthread_mutex_lock(&g_lock);
    err = write_int(GREEN_LED_PATH, brightness);
    if (err !=0)
        err = write_int(GREEN_LED_PATH, brightness);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int set_blueled_light(struct light_device_t* dev, struct light_state_t const* state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    pthread_mutex_lock(&g_lock);
    err = write_int(BLUE_LED_PATH, brightness);
    if (err !=0)
        err = write_int(BLUE_LED_PATH, brightness);
    pthread_mutex_unlock(&g_lock);
    return 0;
}
/*add by xuliu for idpad for led end*/


static int lights_device_close(struct light_device_t *dev)
{
#if USE_PTHREAD_TIMER
    if(tid!=NULL)
    {
        pthread_join(tid, NULL);
    }
#endif
    LOGI(">>> Enter light_device_close");
    if (dev)
        free(dev);
    return 0;
}

/**
 * module methods
 */
static int lights_device_open(const struct hw_module_t* module, char const* name,
        struct hw_device_t** device)
{

    int status = 0;
    int err = 0;

    struct light_device_t *dev;
    dev = (light_device_t*)malloc(sizeof(*dev));
    LOGI(">>> Enter light_device_open:%s\n",name);

    /* initialize our state here */
    memset(dev, 0, sizeof(*dev));

    /* initialize the procs */
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = LIGHTS_DEVICE_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t*)module;
    dev->common.close = (int (*)(struct hw_device_t*))lights_device_close;
    *device = &dev->common;

    if (!strcmp(name, LIGHT_ID_BACKLIGHT)) {
        dev->set_light = set_backlight_light;
    }else if(!strcmp(name, LIGHT_ID_KEYBOARD)) {
        dev->set_light = set_keyboard_light;
    }else if(!strcmp(name, LIGHT_ID_BUTTONS)) {
        dev->set_light = set_buttons_light;
    }else if(!strcmp(name, LIGHT_ID_BATTERY)) {
        dev->set_light = set_battery_light;
        #if USE_PTHREAD_TIMER
        err = pthread_create(&tid, NULL, BlinkThread,(void*)&blink);
        if(err!=0)
            LOGE(">>>  pthread_create error");
        LOGI(">>>  pthread_create ");
        #endif
    }else if(!strcmp(name, LIGHT_ID_NOTIFICATIONS)) {
        dev->set_light = set_notifications_light;
    }else if(!strcmp(name, LIGHT_ID_ATTENTION)) {
        dev->set_light = set_attention_light;
    }else if(!strcmp(name, LIGHT_ID_REDLED)) {
        dev->set_light = set_redled_light;
    }else if(!strcmp(name, LIGHT_ID_GREENLED)) {
        dev->set_light = set_greenled_light;
    }else if(!strcmp(name, LIGHT_ID_BLUELED)) {
        dev->set_light = set_blueled_light;
    }else{
        LOGI(">>> undefine light id");
        free(dev);
        *device = NULL;
        return -EINVAL;
    }
    pthread_once(&g_init,init_g_lock);
    return status;
}

static struct hw_module_methods_t light_module_methods = {
    .open = lights_device_open
};

#if USE_PTHREAD_TIMER
static void* BlinkThread(void* param)
{
    int err = 0;
    static int st_blink = 0;
    static char st_red = 0,st_green =0,st_blue=0;

    /*专心跑子线程*/
    while(1)
    {
        /*状态改变才打印信息*/
        if(st_red != red ||st_blue!=blue||st_blink!=blink||st_green!=green)
        {
            st_red = red;
            st_blue = blue;
            st_green = green;
            st_blink = blink;
            LOGI(">>>  BlinkThread blink:%d red:%d green:%d blue:%d ",st_blink,red,green,blue);
        }

        if(st_blink)
        {
            /*执行下面的代码需要锁住线程*/
            //pthread_mutex_lock(&g_lock);
            if (red)
            {
                err |= write_int(RED_LED_PATH, 1);
                msleep(onMS);
                err |= write_int(RED_LED_PATH, 0);
                msleep(offMS);
            }
            if (green)
            {
                err = write_int(GREEN_LED_PATH, 1);
                msleep(onMS);
                err = write_int(GREEN_LED_PATH, 0);
                msleep(offMS);
            }
            if (blue)
            {
                err = write_int(BLUE_LED_PATH, 1);
                msleep(onMS);
                err = write_int(BLUE_LED_PATH, 0);
                msleep(offMS);
            }
            //pthread_mutex_unlock(&g_lock);
        }
    }
    return NULL;
}
#endif

/*
 * The lights Module
 */
struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "lights module",
    .author = "Rockchip, Inc.",
    .methods = &light_module_methods,
};

