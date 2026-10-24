# 交接文档 —— 步兵底盘已跑通，下一步云台/发射

> 写于 2026-10-24。新对话直接读这个文件即可接上进度，不需要重新分析工程。

## 0. 铁律：必须保证框架的完整性

这是移植过来的跃鹿框架，**不是**一堆可以随意删改的散装代码。后续任何适配都必须在不破坏框架的前提下进行：

1. **三层结构不能打破**：`bsp/`（外设/驱动注册）→ `modules/`（通用模块、算法）→ `application/`（机器人应用）。新功能放到对应层，不要把应用逻辑塞进 bsp，也不要在 application 里直接操作寄存器。
2. **不能为了跑通功能而绕过框架机制**：电机一律走 `motor`/`PowerControl` 模块，应用间通信一律走 `message_center` 的 pub/sub，离线检测走 `daemon`。不要图省事直接调 HAL 或全局变量。
3. **暂时没用的模块也要保留**：视觉、裁判系统、UI、超级电容、IMU、HT04、OLED、蓝牙、TFmini、standard_cmd 等目前没接硬件，但源码和编译都不能删。不启用 ≠ 删掉。
4. **保留所有硬件/协议分支**：单板/双板、SBUS/DBUS、麦轮/全向轮这些条件编译分支都要留着，适配通过 `robot_def.h` 的宏切换，**不要直接删掉另一种实现**。
5. **保留原框架的接口、文件和命名**：各目录下的 `.md` 说明也是框架的一部分，不要清理。新增内容用新文件/新函数，不改原函数的语义。
6. **CubeMX 侧只在 USER CODE 区内改**，构建改动只放顶层 `CMakeLists.txt` 的 USER 区，绝不改 `cmake/stm32cubemx/`。
7. 每次改动后必须保证整个工程**能编译、能烧录、原有已通功能不回退**；不确定是否破坏框架时，先说明影响再动手。

如果为了临时测试必须做临时改动，测完要还原，并在本文件里记录。

## 1. 项目概况

| 项目 | 内容 |
| --- | --- |
| 工程路径 | `D:\STM32\basic_framework` |
| 来源 | HNU 跃鹿 RoboMaster `basic_framework`，原始工程在 `C:\Users\Administrator\Desktop\跃鹿\basic_framework` |
| 主控 | STM32F407IGH6，DJI RoboMaster C 型开发板 |
| 生成工具 | CubeMX 6.10 + STM32Cube FW_F4 V1.28.3，CMake + Ninja |
| 编译 | VS Code STM32Cube 扩展的 `cube-cmake`（自带 GCC 14.3.1），**不是** PATH 上的 GCC 12.2 |
| 调试器 | ST-Link（OpenOCD） |
| 版本管理 | **没有 git**（用户明确不要基线），改坏了只能手动回退 |
| 当前机器人 | 步兵，单板 `ONE_BOARD`，四轮 45°（X 型）全向轮 |

### 编译 / 烧录

- 编译：VS Code 任务 `build`（`cube-cmake --build build/Debug`）
- 编译+烧录：VS Code 任务 `download stlink`（OpenOCD，见 `.vscode/tasks.json`）
- 调试：F5，配置在 `.vscode/launch.json`，OpenOCD 配置 `openocd_stlink.cfg`
- 在普通 bash 里 `cube-cmake` 可能不在 PATH（exit=127），此时用 VS Code 任务或扩展目录下的完整路径。

## 2. 已完成并验证的内容

- 框架三层（`bsp/` `modules/` `application/`）已整体移植，通过 `CMakeLists.txt` 内 USER 区做递归 glob，没有改 `cmake/stm32cubemx/`。
- FreeRTOS 任务、USB 虚拟串口、HardFault 里的 `asm("bx lr")` 都已合并进 CubeMX 工程的 USER CODE 区。
- RTT 源码保留，但 `DISABLE_LOG_SYSTEM=1`，`LOGINFO/LOGERROR` 目前是空的。
- **SBUS 遥控已通**：解析、通道映射、失控保护、急停都正常。
- **底盘已通**：四个轮子转向正确、直行四轮速度一致、全向平移正常、前推=前进。

## 3. 当前关键配置（已改过的值）

### application/robot_def.h

| 宏 | 当前值 | 说明 |
| --- | --- | --- |
| `ONE_BOARD` | 启用 | 单板 |
| `RC_USE_SBUS` | 启用（`RC_USE_DBUS` 注释） | 两者必须且只能有一个 |
| `CHASSIS_OMNI` | 启用（`CHASSIS_MECANUM` 注释） | 两者必须且只能有一个 |
| `YAW_CHASSIS_ALIGN_ECD` | **临时 0** | 未装云台时为 0 才能让 `offset_angle=0`；跃鹿原值 2711，**装云台后必须改成实测值** |
| `YAW_ECD_GREATER_THAN_4096` | 0 | 实测 ALIGN_ECD > 4096 时改 1 |
| `PITCH_HORIZON_ECD` | 3412 | 未实测 |
| `WHEEL_BASE` / `TRACK_WIDTH` | 360 / 360 | **未实测**，与实车不符会导致旋转拖滑 |
| `RADIUS_WHEEL` | 75 | |
| `REDUCTION_RATIO_WHEEL` | 19.0f | M3508 |
| `REDUCTION_RATIO_LOADER` | 36.0f | 2006 拨盘 |

### 底盘电机（application/chassis/chassis.c:85-99）

- CAN1，tx_id：LF=1、RF=2、LB=4、RB=3
- 全部 `MOTOR_DIRECTION_NORMAL`
- 使用 `PowerControlInit()`（功率控制），速度环 Kp=4.5 / Ki=0 / MaxOut=15000

### 遥控通道映射（FS-i6X + iA6B，`modules/remote/remote_control.c`）

- CH1 右摇杆左右、CH2 右摇杆上下、CH3 左摇杆上下、CH4 左摇杆左右
- CH5 SWC 三档、CH6 SWA、CH7 SWB、CH8 SWD、CH9 VRA、CH10 VRB
- 功能：SWA 下=使能 / 上=急停；SWB 摩擦轮；SWC 上/中/下 = NO_FOLLOW / 跟随 / 小陀螺；SWD 拨弹
- USART3：100000 baud、8E1、PC10/PC11，经板载反相器（CubeMX 里已配好）

## 4. 关键代码位置

| 内容 | 位置 |
| --- | --- |
| 机器人初始化/任务 | `application/robot.c` |
| 云台/发射**当前被注释** | `application/robot.c:34-35`（Init）、`application/robot.c:52-53`（Task） |
| 遥控/模式/急停逻辑 | `application/cmd/robot_cmd.c`，SBUS 部分在 331-401 行 |
| 底盘线速度映射 | `application/cmd/robot_cmd.c:361-362`（`vx`/`vy` 目前都带负号 `-10.0f`） |
| 底盘解算 | `application/chassis/chassis.c`，`OmniCalculate()` 在 161-174 行 |
| 底盘模式切换 | `application/chassis/chassis.c:235-248` |
| offset_angle 旋转映射 | `application/chassis/chassis.c:252-256` |
| 功率限制兜底 | `application/chassis/chassis.c:218`（未接裁判系统时用 80W） |
| SBUS 解析 | `modules/remote/remote_control.c`：`SBUSGetChannel` / `SBUSSwitchPosition` / `fs_to_rc` / 回调 |
| 构建与宏 | `CMakeLists.txt` 42-94 行（框架 glob、RTT、`ARM_MATH_CM4`、`DISABLE_LOG_SYSTEM=1`） |
| 任务频率 | `application/robot_task.h`：INS 1kHz、Motor 1kHz、Robot 200Hz、Daemon 100Hz |

## 5. 已踩过的坑（不要重复）

1. **不接裁判系统时轮子不转**：`chassis_power_limit = 0` 会让 `PowerControl` 把输出缩放到 0。`chassis.c:218` 已用 80W 兜底，不要删。
2. **全向轮解算不能把 vy 符号改成全加**：`- + - +` 是保证 vy 列与 wz 列线性无关的关键，全加会让矩阵奇异。
3. **45° 全向轮 = 麦轮公式 × 1/√2**，符号模式相同；`OMNI_COEF = 0.70710678f`。
4. **`offset_angle` 不为 0 会破坏平移**：未装云台时必须让 `YAW_CHASSIS_ALIGN_ECD=0`。
5. **摇杆方向**：前推=前进 是靠 `robot_cmd.c:362` 的负号修正的；如果实车落地后平移方向不对，只改这里，不要动电机方向标志。
6. **不要动 `cmake/stm32cubemx/`**；CubeMX 重新生成时只保留 USER CODE 区，改过的外设配置（USART3 100000/8E1）要确认还在。
7. **OpenOCD `LIBUSB_ERROR_ACCESS`**：`stlinkserver.exe` 或其他 `openocd.exe` 占着 ST-Link，先结束它们。
8. 电机反馈字段是 `motor->measure.ecd`，不是 `motor_measure`。
9. `stop_flag` 含义：`MOTOR_STOP=0`、`MOTOR_ENABLED=1`。

## 6. 下一步待办（按建议顺序）

### A. 实车落地验证底盘

- 四个方向平移、旋转、急停各跑一遍，确认没有拖滑/方向错乱。
- 量实际轴距、轮距，改 `robot_def.h:48-49`（现在 360/360 是估的）。
- 如果觉得速度太快/太慢，调 `robot_cmd.c:361-362` 的 `10.0f` 系数。

### B. 启用云台和发射

1. 取消注释 `application/robot.c` 的 4 行：`GimbalInit()`、`ShootInit()`、`GimbalTask()`、`ShootTask()`。
2. 云台/发射的电机 ID、CAN 口、PID 在 `application/gimbal/gimbal.c` 和 `application/shoot/shoot.c` 里，**都是跃鹿原值，需要按实车改**。
3. 实测 `YAW_CHASSIS_ALIGN_ECD`：
   - 装好云台，把云台和底盘机械对齐到同一朝向；
   - 用调试器读 yaw 电机的 ecd；
   - 填进 `robot_def.h:37`；若 > 4096，把 `YAW_ECD_GREATER_THAN_4096` 改成 1。
4. 实测 `PITCH_HORIZON_ECD`（云台水平时的 pitch ecd），填 `robot_def.h:40`。
5. 云台 PID、摩擦轮/拨弹参数按实车调。

### C. 其他已知未处理

- 视觉 `VisionInit(&huart1)` 仍在跑，但没有上位机，目前无影响。
- 超级电容 `SuperCapInit(&cap_conf)` 在 CAN2 上，没接电容也不影响运行。
- 裁判系统/UI 任务在跑但离线，`referee_data` 的功率限幅目前靠 80W 兜底。
- 日志系统关闭；需要 RTT 调试时把 `CMakeLists.txt` 里 `DISABLE_LOG_SYSTEM=1` 改成 0。

## 7. 用户偏好（重要）

- **改动要小**。先说明"改哪里、改成什么"，用户经常自己动手改；不要一次性大范围重构或加大量调试代码。
- 一步一步来，每一步验证完再继续。
- 不要擅自提交 git / 新建基线（用户明确拒绝过）。
- 不要对未确认的实车参数（对齐 ecd、轴距轮距、PID）瞎填；这些必须实测。
