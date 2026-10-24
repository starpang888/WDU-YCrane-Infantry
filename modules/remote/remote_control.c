#include "remote_control.h"
#include "string.h"
#include "bsp_usart.h"
#include "memory.h"
#include "stdlib.h"
#include "daemon.h"
#include "bsp_log.h"

#define REMOTE_CONTROL_FRAME_SIZE 18u // 遥控器接收的buffer大小
#define SBUS_FRAME_SIZE 25u//fs遥控器的buffer
#define SBUS_HEADER 0x0Fu        // SBUS帧头
#define SBUS_FLAG_FAILSAFE 0x08u // 第23字节bit3,接收机失控保护标志

//新遥控器摇杆位置
#define SWITCH_HIGH 1807
#define SWITCH_MID  1024
#define SWITCH_LOW  240
#define SWITCH_TOLERANCE 200

// 遥控器数据
static RC_ctrl_t rc_ctrl[2];     //[0]:当前数据TEMP,[1]:上一次的数据LAST.用于按键持续按下和切换的判断
static FS_RC_ctrl_t fs_rc_ctrl[2]; //[0]:当前数据TEMP,[1]:上一次的数据LAST.用于按键持续按下和切换的判断
static uint8_t rc_init_flag = 0; // 遥控器初始化标志位

// 遥控器拥有的串口实例,因为遥控器是单例,所以这里只有一个,就不封装了
static USARTInstance *rc_usart_instance;
static DaemonInstance *rc_daemon_instance;

/**
 * @brief 矫正遥控器摇杆的值,超过660或者小于-660的值都认为是无效值,置0
 *
 */
static void RectifyRCjoystick()
{
    for (uint8_t i = 0; i < 5; ++i)
        if (abs(*(&rc_ctrl[TEMP].rc.rocker_l_ + i)) > 660)
            *(&rc_ctrl[TEMP].rc.rocker_l_ + i) = 0;
}

/**
 * @brief 遥控器数据解析
 *
 * @param sbus_buf 接收buffer
 */
static void sbus_to_rc(const uint8_t *sbus_buf)
{
    // 摇杆,直接解算时减去偏置
    rc_ctrl[TEMP].rc.rocker_r_ = ((sbus_buf[0] | (sbus_buf[1] << 8)) & 0x07ff) - RC_CH_VALUE_OFFSET;                              //!< Channel 0
    rc_ctrl[TEMP].rc.rocker_r1 = (((sbus_buf[1] >> 3) | (sbus_buf[2] << 5)) & 0x07ff) - RC_CH_VALUE_OFFSET;                       //!< Channel 1
    rc_ctrl[TEMP].rc.rocker_l_ = (((sbus_buf[2] >> 6) | (sbus_buf[3] << 2) | (sbus_buf[4] << 10)) & 0x07ff) - RC_CH_VALUE_OFFSET; //!< Channel 2
    rc_ctrl[TEMP].rc.rocker_l1 = (((sbus_buf[4] >> 1) | (sbus_buf[5] << 7)) & 0x07ff) - RC_CH_VALUE_OFFSET;                       //!< Channel 3
    rc_ctrl[TEMP].rc.dial = ((sbus_buf[16] | (sbus_buf[17] << 8)) & 0x07FF) - RC_CH_VALUE_OFFSET;                                 // 左侧拨轮
    RectifyRCjoystick();
    // 开关,0左1右
    rc_ctrl[TEMP].rc.switch_right = ((sbus_buf[5] >> 4) & 0x0003);     //!< Switch right
    rc_ctrl[TEMP].rc.switch_left = ((sbus_buf[5] >> 4) & 0x000C) >> 2; //!< Switch left

    // 鼠标解析
    rc_ctrl[TEMP].mouse.x = (sbus_buf[6] | (sbus_buf[7] << 8)); //!< Mouse X axis
    rc_ctrl[TEMP].mouse.y = (sbus_buf[8] | (sbus_buf[9] << 8)); //!< Mouse Y axis
    rc_ctrl[TEMP].mouse.press_l = sbus_buf[12];                 //!< Mouse Left Is Press ?
    rc_ctrl[TEMP].mouse.press_r = sbus_buf[13];                 //!< Mouse Right Is Press ?

    //  位域的按键值解算,直接memcpy即可,注意小端低字节在前,即lsb在第一位,msb在最后
    *(uint16_t *)&rc_ctrl[TEMP].key[KEY_PRESS] = (uint16_t)(sbus_buf[14] | (sbus_buf[15] << 8));
    if (rc_ctrl[TEMP].key[KEY_PRESS].ctrl) // ctrl键按下
        rc_ctrl[TEMP].key[KEY_PRESS_WITH_CTRL] = rc_ctrl[TEMP].key[KEY_PRESS];
    else
        memset(&rc_ctrl[TEMP].key[KEY_PRESS_WITH_CTRL], 0, sizeof(Key_t));
    if (rc_ctrl[TEMP].key[KEY_PRESS].shift) // shift键按下
        rc_ctrl[TEMP].key[KEY_PRESS_WITH_SHIFT] = rc_ctrl[TEMP].key[KEY_PRESS];
    else
        memset(&rc_ctrl[TEMP].key[KEY_PRESS_WITH_SHIFT], 0, sizeof(Key_t));

    uint16_t key_now = rc_ctrl[TEMP].key[KEY_PRESS].keys,                   // 当前按键是否按下
        key_last = rc_ctrl[LAST].key[KEY_PRESS].keys,                       // 上一次按键是否按下
        key_with_ctrl = rc_ctrl[TEMP].key[KEY_PRESS_WITH_CTRL].keys,        // 当前ctrl组合键是否按下
        key_with_shift = rc_ctrl[TEMP].key[KEY_PRESS_WITH_SHIFT].keys,      //  当前shift组合键是否按下
        key_last_with_ctrl = rc_ctrl[LAST].key[KEY_PRESS_WITH_CTRL].keys,   // 上一次ctrl组合键是否按下
        key_last_with_shift = rc_ctrl[LAST].key[KEY_PRESS_WITH_SHIFT].keys; // 上一次shift组合键是否按下

    for (uint16_t i = 0, j = 0x1; i < 16; j <<= 1, i++)
    {
        if (i == 4 || i == 5) // 4,5位为ctrl和shift,直接跳过
            continue;
        // 如果当前按键按下,上一次按键没有按下,且ctrl和shift组合键没有按下,则按键按下计数加1(检测到上升沿)
        if ((key_now & j) && !(key_last & j) && !(key_with_ctrl & j) && !(key_with_shift & j))
            rc_ctrl[TEMP].key_count[KEY_PRESS][i]++;
        // 当前ctrl组合键按下,上一次ctrl组合键没有按下,则ctrl组合键按下计数加1(检测到上升沿)
        if ((key_with_ctrl & j) && !(key_last_with_ctrl & j))
            rc_ctrl[TEMP].key_count[KEY_PRESS_WITH_CTRL][i]++;
        // 当前shift组合键按下,上一次shift组合键没有按下,则shift组合键按下计数加1(检测到上升沿)
        if ((key_with_shift & j) && !(key_last_with_shift & j))
            rc_ctrl[TEMP].key_count[KEY_PRESS_WITH_SHIFT][i]++;
    }

    memcpy(&rc_ctrl[LAST], &rc_ctrl[TEMP], sizeof(RC_ctrl_t)); // 保存上一次的数据,用于按键持续按下和切换的判断
}

/**
 * @brief 从SBUS帧中取出第ch个通道(0~15)的11位原始值
 *        数据区从第1字节开始,每个通道11位,低位在前
 */
static uint16_t SBUSGetChannel(const uint8_t *sbus_buf, uint8_t ch)
{
    uint16_t bit = ch * 11u;
    const uint8_t *p = &sbus_buf[1 + bit / 8u];
    uint32_t raw = p[0] | (p[1] << 8) | (p[2] << 16);
    return (raw >> (bit % 8u)) & 0x07FF;
}

/**
 * @brief 将拨杆原始值转换为物理档位 FS_RC_SW_UP/MID/DOWN
 *
 * @param raw 原始通道值
 * @param up_is_high 拨杆上档对应高位(1807)时为1,上档对应低位(240)时为0
 */
static uint8_t SBUSSwitchPosition(uint16_t raw, uint8_t up_is_high)
{
    if (raw > SWITCH_HIGH - SWITCH_TOLERANCE && raw < SWITCH_HIGH + SWITCH_TOLERANCE)
        return up_is_high ? FS_RC_SW_UP : FS_RC_SW_DOWN;
    if (raw > SWITCH_LOW - SWITCH_TOLERANCE && raw < SWITCH_LOW + SWITCH_TOLERANCE)
        return up_is_high ? FS_RC_SW_DOWN : FS_RC_SW_UP;
    return FS_RC_SW_MID; // 中档或异常值默认归中
}

/**
 * @brief SBUS遥控器(FS-i6X + iA6B)数据解析,通道定义见 FS-i6X遥控器通道映射.md
 *        CH1 右摇杆左右 CH2 右摇杆上下 CH3 左摇杆上下 CH4 左摇杆左右
 *        CH5 SWC(三档) CH6 SWA CH7 SWB CH8 SWD CH9 VRA CH10 VRB
 *        摇杆统一转换为 右/上 为正,与DJI遥控器一致
 *
 * @param sbus_buf 接收buffer
 */
static void fs_to_rc(const uint8_t *sbus_buf)
{
    for (uint8_t i = 0; i < 10; ++i) // 保存原始通道值,便于调试时查看
        fs_rc_ctrl[TEMP].channel[i] = SBUSGetChannel(sbus_buf, i);

    // 摇杆,中位1024; CH1左推为高位,取反使右为正
    fs_rc_ctrl[TEMP].rc.rocker_r_ = RC_CH_VALUE_OFFSET - fs_rc_ctrl[TEMP].channel[0];
    fs_rc_ctrl[TEMP].rc.rocker_r1 = fs_rc_ctrl[TEMP].channel[1] - RC_CH_VALUE_OFFSET;
    fs_rc_ctrl[TEMP].rc.rocker_l1 = fs_rc_ctrl[TEMP].channel[2] - RC_CH_VALUE_OFFSET;
    fs_rc_ctrl[TEMP].rc.rocker_l_ = fs_rc_ctrl[TEMP].channel[3] - RC_CH_VALUE_OFFSET;

    for (uint8_t i = 0; i < 4; ++i)
        if (*(&fs_rc_ctrl[TEMP].rc.rocker_l_ + i) > 660){
             *(&fs_rc_ctrl[TEMP].rc.rocker_l_ + i) = 660;
        }else if(*(&fs_rc_ctrl[TEMP].rc.rocker_l_ + i) < -660){
            *(&fs_rc_ctrl[TEMP].rc.rocker_l_ + i) = -660;
        }
           
    // 拨杆,转换为物理档位 0上 1中 2下
    fs_rc_ctrl[TEMP].rc.switch_right_1 = SBUSSwitchPosition(fs_rc_ctrl[TEMP].channel[4], 0); // SWC,上档为低位
    fs_rc_ctrl[TEMP].rc.switch_left_1 = SBUSSwitchPosition(fs_rc_ctrl[TEMP].channel[5], 1);  // SWA,上档为高位
    fs_rc_ctrl[TEMP].rc.switch_left_2 = SBUSSwitchPosition(fs_rc_ctrl[TEMP].channel[6], 1);  // SWB,上档为高位
    fs_rc_ctrl[TEMP].rc.switch_right_2 = SBUSSwitchPosition(fs_rc_ctrl[TEMP].channel[7], 1); // SWD,上档为高位

    // 旋钮,顺时针增加,范围约0~1567
    fs_rc_ctrl[TEMP].rc.knob_l = fs_rc_ctrl[TEMP].channel[8] - SWITCH_LOW; // VRA
    fs_rc_ctrl[TEMP].rc.knob_r = fs_rc_ctrl[TEMP].channel[9] - SWITCH_LOW; // VRB

    memcpy(&fs_rc_ctrl[LAST], &fs_rc_ctrl[TEMP], sizeof(FS_RC_ctrl_t)); // 保存上一次的数据,用于按键持续按下和切换的判断

}

/**
 * @brief 对sbus_to_rc的简单封装,用于注册到bsp_usart的回调函数中
 *
 */
static void RemoteControlRxCallback()
{
    DaemonReload(rc_daemon_instance);         // 先喂狗
    sbus_to_rc(rc_usart_instance->recv_buff); // 进行协议解析
}

/**
 * @brief 对fs_to_rc的简单封装,用于注册到bsp_usart的回调函数中
 *
 */
static void FS_RemoteControlRxCallback()
{
    const uint8_t *buf = rc_usart_instance->recv_buff;
    // 帧头错误(帧不完整/错位)或接收机进入失控保护时丢弃该帧且不喂狗,超时后由守护进程判定离线
    if (buf[0] != SBUS_HEADER || (buf[23] & SBUS_FLAG_FAILSAFE))
        return;
    DaemonReload(rc_daemon_instance);       // 先喂狗
    fs_to_rc(buf);                          // 进行协议解析
}

/**
 * @brief 遥控器离线的回调函数,注册到守护进程中,串口掉线时调用
 *
 */
static void RCLostCallback(void *id)
{
    memset(rc_ctrl, 0, sizeof(rc_ctrl)); // 清空遥控器数据
    USARTServiceInit(rc_usart_instance); // 尝试重新启动接收
    LOGWARNING("[rc] remote control lost");
}

static void FS_RCLostCallback(void *id)
{
    memset(fs_rc_ctrl, 0, sizeof(fs_rc_ctrl)); // 清空遥控器数据
    USARTServiceInit(rc_usart_instance); // 尝试重新启动接收
    LOGWARNING("[rc] remote control lost");
}

RC_ctrl_t *RemoteControlInit(UART_HandleTypeDef *rc_usart_handle)
{
    USART_Init_Config_s conf;
    conf.module_callback = RemoteControlRxCallback;
    conf.usart_handle = rc_usart_handle;
    conf.recv_buff_size = REMOTE_CONTROL_FRAME_SIZE;
    rc_usart_instance = USARTRegister(&conf);

    // 进行守护进程的注册,用于定时检查遥控器是否正常工作
    Daemon_Init_Config_s daemon_conf = {
        .reload_count = 10, // 100ms未收到数据视为离线,遥控器的接收频率实际上是1000/14Hz(大约70Hz)
        .callback = RCLostCallback,
        .owner_id = NULL, // 只有1个遥控器,不需要owner_id
    };
    rc_daemon_instance = DaemonRegister(&daemon_conf);

    rc_init_flag = 1;
    return rc_ctrl;
}

FS_RC_ctrl_t *FS_RemoteControlInit(UART_HandleTypeDef *rc_usart_handle)
{
    USART_Init_Config_s conf;
    conf.module_callback = FS_RemoteControlRxCallback;
    conf.usart_handle = rc_usart_handle;
    conf.recv_buff_size = SBUS_FRAME_SIZE;
    rc_usart_instance = USARTRegister(&conf);

    // 进行守护进程的注册,用于定时检查遥控器是否正常工作
    Daemon_Init_Config_s daemon_conf = {
        .reload_count = 10, // 100ms未收到数据视为离线,遥控器的接收频率实际上是1000/14Hz(大约70Hz)
        .callback = FS_RCLostCallback,
        .owner_id = NULL, // 只有1个遥控器,不需要owner_id
    };
    rc_daemon_instance = DaemonRegister(&daemon_conf);

    rc_init_flag = 1;
    return fs_rc_ctrl;
}

uint8_t RemoteControlIsOnline()
{
    if (rc_init_flag)
        return DaemonIsOnline(rc_daemon_instance);
    return 0;
}

uint8_t FS_RemoteControlIsOnline(){
    if (rc_init_flag)
        return DaemonIsOnline(rc_daemon_instance);
    return 0;
}
