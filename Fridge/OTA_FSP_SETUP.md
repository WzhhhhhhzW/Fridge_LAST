# RA6M5 双 Bank OTA：FSP 5.9.0 操作清单

本清单只要求你在 e² studio 图形界面操作。不要手改
`configuration.xml`、`ra_gen/hal_data.c`、`ra_gen/hal_data.h`、
`ra_gen/vector_data.*` 或链接布局生成文件。

## A. 创建 Bootloader 工程

1. 关闭正在运行的 Debug 会话，保留现有应用工程。
2. 选择 `File > New > Renesas C/C++ Project > Renesas RA`。
3. 选择 RA6M5 MCUoot 单镜像、Flat、mbedTLS 硬件加速模板。
4. Project name 填 `project_firdge_boot`。
5. Device 必须选择 `R7FA6M5BF2CBG`。
6. FSP Version 选择 `5.9.0`；工程类型选择 `Flat` 或
   `Non-TrustZone`，不要创建 Secure/Non-Secure 工程对。
7. 创建完成后打开 `configburation.xml > Stacks`。

如果向导没有 MCUboot 模板，先创建 Flat Project，再在 Stacks 页面点击
`New Stack > Bootloader > MCUboot`。

### MCUboot 属性

选中 MCUboot stack，在 Properties 中逐项设置：

- Images：`1`
- Upgrade Mode：`Direct XIP`
- Dual Bank Mode：`Enabled`
- Signature Type：`ECDSA P-256`
- Validate Primary Image：`Enabled`
- Crypto：`mbedTLS`，保留模板自动添加的 SCE9 硬件加速栈
- Watchdog Feed：`ota_boot_watchdog_refresh`
- TrustZone：不要启用
- 不手填 Bootloader、Primary、Secondary 地址

将下列两个文件以 linked source 方式加入 Bootloader 工程：

- `Fridge/ota_bootloader_support/ota_boot_watchdog.c`
- `Fridge/ota_bootloader_support/ota_boot_trial.c`

同时把以下目录加入 Bootloader 的 C include paths：

```text
D:\Desktop\fridge\Fridge\ota_bootloader_support
D:\Desktop\fridge\Fridge\project_firdge\src
```

`ota_boot_trial.c` 暂时只编译，不要猜测 MCUboot 内部跳转函数并强行接入。
生成工程后需要根据实际 pre-jump hook 和 `.sbd` 布局完成最后一层适配。

### Flash HP

在 Stacks 中确认模板已经存在 Flash HP；没有时添加：

`New Stack > Storage > Flash (r_flash_hp)`

设置：

- Code Flash Programming：`Enabled`
- Data Flash Programming：`Enabled`
- Data Flash Background Operation：`Disabled`
- Code Flash Background Operation：`Disabled`
- Callback：空

Bootloader 需要同时读写 Code Flash 和最后 512 字节 Data Flash 日志。

### Independent Watchdog

在 Stacks 中添加：

`New Stack > Monitoring > Independent Watchdog (r_iwdt)`

Name 改为 `g_iwdt0`。然后打开 BSP/Properties 中的 OFS/IWDT 选项：

- Start Mode：`Auto Start`
- Timeout：`2048 cycles`
- IWDTCLK Division Ratio：`/256`
- Window Start：`100%`
- Window End：`0%`
- Timeout Action：`Reset`
- Low-power/Stop Control：继续计数，不要选择 Stops

如果属性名称略有差异，以数值和行为为准。

### 生成和首次构建

1. 点击 `Generate Project Content`。
2. 确认 Problems 视图没有红色 constraint。
3. `Project > Clean...`，只选择 `project_firdge_boot`。
4. 右键 Bootloader 工程，选择 `Build Project`。
5. 在 Debug 目录确认存在：
   - Bootloader `.elf`
   - Bootloader `.srec`
   - SmartBundle `.sbd`
   - `.map`
6. 保存 MCUboot 生成的签名说明、slot 布局和
   `ra_cfg/mcu-tools/include/mcuboot_config/mcuboot_config.h`。

## B. 配置现有应用工程

打开 `project_firdge/configuration.xml`。

### BSP 双 Bank 和 IWDT OFS

在 `BSP > Properties` 设置：

- Dual Bank Mode：`Enabled`
- IWDT Start Mode：`Auto Start`
- IWDT Timeout：`2048 cycles`
- IWDT Clock Divider：`/256`
- Window Start：`100%`
- Window End：`0%`
- Expire Action：`Reset`
- Stop/Low-power Control：继续计数

不要改 SSIE、DTC、GPT、SCI 或 Pins。

### 应用 Flash HP

在 Stacks 页面添加：

`New Stack > Storage > Flash (r_flash_hp)`

设置：

- Name：`g_ota_flash`
- Code Flash Programming：`Enabled`
- Data Flash Programming：`Enabled`
- Code/Data Flash BGO：`Disabled`，即 blocking
- Callback：空

### 应用 IWDT

添加：

`New Stack > Monitoring > Independent Watchdog (r_iwdt)`

- Name：`g_iwdt0`
- 其余运行参数使用 BSP/OFS 中刚才设置的值

### 生成应用 FSP 内容

1. 点击 `Generate Project Content`。
2. 检查 Problems 没有 constraint。
3. 此时先不要把 `OTA_UPDATE_FSP_READY` 改成 1。

### 绑定 Bootloader SmartBundle

1. 右键 `project_firdge > Properties`。
2. 打开 `C/C++ Build > Build Variables`。
3. 点击 `Add...`：
   - Variable name：`SmartBundle`
   - Type：`File`
   - Value：浏览选择 Bootloader Debug 目录里的 `.sbd`
4. 再添加：
   - `MCUBOOT_IMAGE_VERSION`：例如 `1.0.0+1`
   - `MCUBOOT_IMAGE_SIGNING_KEY`：仓库外的 ECDSA-P256 私钥绝对路径
5. Apply and Close。

私钥不能放进 `D:\Desktop\fridge`，不能上传网站或服务器。服务器只保存
对应 PEM 公钥。

## C. 生成后交给 Codex 检查

完成 A、B 后告诉 Codex“Bootloader 和应用 FSP 都生成完成”。检查范围：

- 应用 BSP 双 Bank 已启用
- `g_ota_flash` 为 Flash HP，Code/Data Flash 可编程且 blocking
- `g_iwdt0` 已生成，OFS 为 Auto Start、2048、/256、持续计数
- SSIE、W800、GPT、DTC 未slot 地址一致

检查通过后，Codex会把生成布局中的**实际应用 slot 起始地址、大小、擦除
块数量和擦除块大小**写入 `src/ota_update_config.h`，再将
`OTA_UPDATE_FSP_READY` 改为 1。不要根据网上示意图自己填地址。

RA6M5 1 MB Dual Bank 的文档示意地址不能替代你工程的生成布局，尤其
Bank 1 在线性地址和物理地址下的表示不同。

## D. 第一次烧录前

1. 使用新私钥重新 clean build 应用，得到 FSP 生成的 signed binary。
2. 核对应用宏版本和 MCUboot image version 完全一致。
3. 第一次用 J-Link/RFP 写入：
   - Bank 0 Bootloader
   - Bank 0 初始签名应用
   - Bank 1 Bootloader 副本回归
- Bootloader 为 Direct XIP、dual bank、ECDSA-P256、primary validation
- SCE9/mbedTLS 硬件加速栈存在
- `.sbd`、`.map` 和 MCUboot 
4. Dual Bank 的 OFS/BANKSEL 由 Bootloader 工程生成；独立写多个文件时，
   带 OFS 的 Bootloader SREC 最后写。
5. 开发阶段不要设置永久 Flash Access Window，也不要锁调试接口。

等有效升级、错误签名、断电续传和三次失败回退都通过后，再单独执行
RFP 的 Bootloader 区域保护。
