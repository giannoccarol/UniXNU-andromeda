# UniXNU-andromeda

**iOS 16.7.16 su Xiaomi Mi Mix 3 5G (`andromeda`, Snapdragon 855 / SM8150): Project Aloha UEFI → UniXNU/DarwinBoot
→ kernel XNU 8796 (piattaforma ios-sweet "SWEET") → iOS fino alla schermata di blocco.**

Questo repository nasce da [`YangQi0408/UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus), il port per
lo Xiaomi Mi 9 (stesso SoC), a sua volta derivato da
[`JohnLeeeeeee678/UniXNU-raphael`](https://github.com/JohnLeeeeeee678/UniXNU-raphael); il kernel e i driver di base
vengono da [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) e molto lavoro sull'SM8150 da
[`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame). Tutte le persone e i progetti di cui usiamo il lavoro
sono in [CREDITS.md](CREDITS.md).

- Stato, istruzioni e risultati sul telefono: [ANDROMEDA.md](ANDROMEDA.md)
- Cosa manca e prove da fare: [docs/COSA-MANCA.md](docs/COSA-MANCA.md); piano: [ROADMAP.md](ROADMAP.md)
- Regole del progetto: [CONTRIBUTING.md](CONTRIBUTING.md)

Nessun file Apple (IPSW, immagini iOS, device tree estratti) e nessun dato del telefono è in questo repository: il
sistema iOS si costruisce sul proprio PC dall'IPSW scaricato da Apple.

---

Il README originale del port per il Mi 9 (`YangQi0408/UniXNU-cepheus`) segue invariato.

# UniXNU-cepheus

**在小米 9（`cepheus`，骁龙 855 / SM8150）上通过 UEFI 引导苹果 XNU 内核，跑到 iOS 16.7.16 的 SpringBoard。**

从 [`JohnLeeeeeee678/UniXNU-raphael`](https://github.com/JohnLeeeeeee678/UniXNU-raphael)（红米 K20 Pro，
**同一颗 SM8150**）移植，并把所有硬件参数换成本机实测值。

> 状态：**🎉🎉 里程碑达成（2026-11-01）：iOS 16.7.16 成功进入 SpringBoard 主屏幕桌面！**
> 物理电源键正常工作（可睡眠/唤醒），Home 键事件接通并成功解锁进主屏幕。
> 当前使用 CPU 软渲染显示桌面图标；用户报告刷新慢，具体瓶颈尚待计数和耗时测量。
> **物理触摸已由用户确认可用**：ST-FTS 经 L1C 电源开启＋GPIO 开漏 I²C 接入 iOS HID。
> GENI SE17 的硬件 FIFO 不可用，GPI/GSI DMA 尚未实现；现用独立 GPIO 传输，不绕过 FIFO 禁用标志。

触摸基线：`boot-cepheus-touch-power.img` 已读到芯片ID `36:39`；用户确认触摸可用，
日志记录51次按下/51次抬起、坐标移动和边缘事件，无触摸总线错误。
这不是长期稳定性或全部多点手势测试，也不是GPI DMA实现，
详见 [触摸测试记录](docs/touch-bringup.md)。

性能对照版已启动，包含一次 Home、固定期限定时和低频性能统计；
初测全屏 present 约 2–5 ms，尚不能据此断言实际动画FPS或总体加速。
构建和测量限制见 [性能实验](docs/performance.md)。

CPU/GPU加速工作：已实测Gold约711MHz、Prime约827MHz，正进行OSM固件LUT内
受限调频验证。Adreno的CX/GX尚未上电，Metal和硬件合成尚未实现；进展与
验收边界见 [CPU/GPU记录](docs/cpu-gpu.md)。

---

## 一、这台机器（实测，2026-10-31）

| 项 | 值 | 来源 |
|---|---|---|
| 型号 | Mi 9，`cepheus`，hwversion 1.9.2，CN，MP | `ro.product.device` / `ro.boot.hwversion` |
| 序列号 | `67eea163` | `ro.boot.serialno` |
| 内存 | **8 GB**（Explorer 版），`MemTotal = 7655356 kB` | `/proc/meminfo` |
| 存储 | **256 GB**，UFS | `fastboot`/`sda` 几何 |
| 面板 | `dsi_samsung_fhd_ea8076_f1p2_cmd_display`，1080×2340 | `ro.boot` cmdline |
| 触控 | **ST-FTS**（`compatible = "st,fts"`，I²C `0x49`），FTS521 驱动 | 设备树 + LineageOS `fts_521` |
| 指纹 | Goodix（屏下） | 设备树 `goodix,fingerprint` |

### 关键内存布局（live FDT `/memory`）

```
0x080000000 + 0x3BB00000   low  bank  (ends 0x0BBC0000)
0x100000000 + 0x100000000  high bank
0x0C0000000 + 0x0C0000000  mid  bank  <-- dram-base 取这里（无 carve-out）
```

注意 low bank 和 mid bank 之间有 **52 MiB 空洞 `0xBBC00000–0xC0000000`**，所以 `dram-base`
**不能**用 `0x80000000`（会踩到 hyp/modem 预留区）。

### 预留区（live FDT `reserved-memory`，决定了 UEFI 能放哪）

```
0x9C000000 + 0x02400000  cont_splash_region  (36 MiB, framebuffer)
0x9E400000 + 0x01400000  qseecom_region      (ends 0x9F800000)
0x9F800000 .. 0xA0000000  <-- 空闲 DRAM，固件放这里
```

---

## 二、产物

```
out/cepheus/boot/boot-cepheus.img     67.6 MiB  Android boot image（fastboot boot 用）
out/cepheus/payload/payload.img       64 MiB    FAT16 卷 "GUACESP"
    EFI/UniXNU/unixnu.toml                     启动菜单条目 + boot-args
    EFI/UniXNU/Payloads/kernel.macho           17.2 MiB SM8150 XNU（ios-sweet 构建）
    EFI/UniXNU/Payloads/kernel.macho.adt       12.6 KiB cepheus ADT
uefi/Build/.../RELEASE_CLANGPDB/FV/SM8150_EFI.fd   3 MiB   Project Aloha UEFI（含 UniXNU/DarwinBoot）
xnu/0082-cepheus-touch.patch                   触摸驱动补丁（应用到 ios-sweet）
```

### 为什么 FD 必须是 3 MiB（踩过的坑）

cepheus 的内存映射把顶部 DRAM 切得满满当当，**没有空洞**：

```
0x9F800000 + 0x400000  FV Region
0x9FC00000 + 0x300000  UEFI FD      <-- 固件必须正好 3 MiB，结束于 0x9FF00000
0x9FF00000 + 0x8C000   SEC Heap
0x9FF8C000 + 0x1000    CPU Vectors
0x9FF8D000 + 0x3000    MMU PageTables
0x9FF92000 + 0x1E000   RAM Partition
0x9FFB0000 + 0x20000   UEFI Stack
...
0x9FFFF000 + 0x1000    Info Blk
```

把 FD 改成 4 MiB 会让它一路踩到 `0xA0000000`，**覆盖 SEC Heap / CPU Vectors / MMU 页表 /
UEFI Stack / Info Block**。现象就是：**屏幕点亮、出日志，然后卡死。**

3 MiB 装不下 DEBUG 构建（`FVMAIN_COMPACT = 0x308610..0x313490`，`GenFv: ERROR 3000`），
但 **RELEASE 只有 `0xC3FC18`，余量充足**——所以 `FW_TARGET` 必须是 `RELEASE`
（raphael 参考端口和它的 CI 默认值也是 RELEASE）。这样 55 个 RAW 资源（含 28 个面板
XML）全部保留，不需要裁掉任何东西。

启动链：

```
fastboot boot boot-cepheus.img         # 只进 RAM，不写任何分区
 └─ Project Aloha UEFI (TARGET_DEVICE=xiaomi-cepheus, FDT=cepheus.dtb)
     └─ UniXNU 菜单 → GUACESP FAT payload (64 MiB)
          └─ DarwinBoot → XNU → UFS → userdata 里的 HFS+ 根 → launchd → SpringBoard
```

---

## 三、和 raphael 版不同的地方（都改成了本机实测值）

| 项 | raphael | cepheus（本仓库） |
|---|---|---|
| `dram_size` | `0xc0000000` | **`0x200000000`**（8 GB） |
| `userdata` | `0x7C000`–`0x1C47FFA` | **`0x7C000`–`0x3A107FA`**（256 GB 机） |
| DTB | Aloha 的 `android-raphael.dtb` | **本机 `/sys/firmware/fdt` 拉下来的 live FDT** |
| UEFI FD 大小 | 3 MiB | **3 MiB**（必须与内存映射 `{"UEFI FD", 0x9FC00000, 0x00300000}` 一致；见下方「为什么不能用 4 MiB」） |
| EDK II target | DEBUG | **RELEASE**（DEBUG 的 `FVMAIN_COMPACT` 是 `0x308610`，3 MiB 装不下） |
| 触摸 | 无需改（Goodix GT9889，与 sweet 同款协议） | **全新 ST-FTS 驱动**（见下） |

---

## 四、触摸驱动（唯一必须新写的部分）

已验证锁屏的预编译内核**仍含参考 SE7/Goodix 驱动**，触摸保持关闭，不可直接
把 `sweettouch` 改为 1。专用内核构建使用 `xnu/0082-stage-cepheus-touch.sh` 暂存
`cepheus_touch.c`，由 `0082-cepheus-touch.patch` 改编译清单，仅编译 cepheus 驱动。
`touch-kernel` workflow 会核对编译文件和 Mach-O 中的驱动标记，防止补丁静默跳过。
以下参数为源代码已核对值，专用触摸内核尚待构建和真机验证：

| 项 | cepheus | 出处 |
|---|---|---|
| I²C 控制器 | `qupv3_se17_i2c: i2c@0xc80000` | live FDT |
| GENI 时钟 RCG | `0x1e148`（wrap2 local S0 = 全局 SE17） | `gcc-sm8150.c` `gcc_qupv3_wrap2_s0_clk_src.cmd_rcgr` |
| GENI 时钟投票 | `0x52014` bit 4 | `gcc_qupv3_wrap2_s0_clk.clkr.enable_reg/mask` |
| SDA / SCL | GPIO **55 / 56**，function `qup17`，2 mA，bias-disable | live FDT `qupv3_se17_i2c_active` |
| IRQ | GPIO **122**，`0x2008`（ONESHOT｜LOW），8 mA，pull-up | live FDT `fts@49` + `ts_active` |
| RESET | GPIO **12**，**低有效**（0=复位，1=释放） | live FDT + `fts_521/fts.c` `gpio_set_value(reset,0/1)` |
| VDDIO | GPIO **84**（`touch_vddio_vreg`，1.8 V，`regulator-boot-on`） | live FDT |
| AVDD | `pm8150l_l1`（L1C，1.8 V，RPMh，ABL 已开） | live FDT |
| I²C 地址 | `0x49` | live FDT `fts@49` |
| 芯片 ID | hw reg `0x20000000`，首字节 `0x36`（DCHIP_ID_0） | `fts_521/fts_lib/ftsHardware.h` |
| 事件 FIFO | 命令 `0x86`，8 字节/事件，`event[7]&0x1F` = 剩余事件数 | `fts_521/fts.c` `FIFO_CMD_READALL` |
| 事件 ID | `0x13` enter / `0x23` motion / `0x33` leave | `fts_lib/ftsSoftware.h` |
| 坐标 | `x=((e[3]&0x0F)<<8)|e[2]`，`y=(e[4]<<4)|((e[3]&0xF0)>>4)` | `fts_521/fts.c` `fts_enter_pointer_event_handler` |
| 手指 ID | `(e[1] & 0xF0) >> 4` | 同上 |
| 触摸类型 | `e[1] & 0x0F` | 同上 |

开关：`sweettouch=0` 关闭；`sweettouchdelay=N` 延迟 N 秒启动（默认 20，防止总线卡死
时连日志都拿不到）；`sweettouchirq=1` 启用 IRQ 提示轮询。

---

## 五、真机 bring-up：判读阶梯

从早到晚七层信号，看到第 N 层就说明前 N 层都成功：

| # | 画面 | 说明 |
|---|---|---|
| A | UEFI 菜单 | 固件正常 |
| B | DarwinBoot 日志 + **底部**绿条 | 交接桩执行（`video pa` / `entry pa` 打印出来） |
| C | 左下 64×32 白块 | `start.s` 第一条指令执行（MMU 关，直写 fb） |
| C1–C4 | 白块上方红/绿/蓝/品红 4 条 64×8 色带 | 汇编检查点 A–D 全过 |
| D | `CEPHEUS EARLY 0: video pa=0x... w=1080 h=2340 row=4320` | 进了 `arm_init()` |
| E | 底部 20 格色块 + `CEPHEUS EARLY 0..19` | 内核逐段推进；停在第几格＝卡在哪 |
| F | 深蓝底 + `fbcon ready: pa=0x...` | `serial_init()` 注册 fb 控制台 |
| G | 蓝底内核日志 / panic | 之后全部可见 |

> ⚠️ **已知陷阱**：Mu-Silicium 在这个 SoC 家族上交出的 `video pa` 实测是 **`0x90000000`**
> （不是设备树的 `0x9C000000`），落在 6/8 GB RAM 的 V=P 映射内、属性是 **write-back**。
> 所有早期打点必须（a）优先用 `boot_args->Video`，（b）无条件按 cache line `dc cvac`
> 刷出，否则屏幕全黑但内核其实在跑。

---

## 六、安全动作（做之前先看）

```bash
# 1. 备份每机校准数据（丢了不可恢复）
adb reboot recovery   # 或临时 fastboot boot OrangeFox
adb pull /dev/block/by-name/persist
adb pull /dev/block/by-name/modemst1 ; adb pull /dev/block/by-name/modemst2
adb pull /dev/block/by-name/fsg     ; adb pull /dev/block/by-name/fsc
adb pull /dev/block/by-name/boot    ; adb pull /dev/block/by-name/dtbo
adb pull /dev/block/by-name/vbmeta
```

**`fastboot boot` 不写分区**。但**根文件系统要写进 `userdata`**
（`rd=ufsroot0 ufsrw=1 rootrw=1`），也就是说 **Android 的用户数据会被清掉**。
系统分区 / boot / modem / persist 不动，但这不保证重启后 Android 可用；
恢复 Android 还需要恢复其 userdata 文件系统。不要通过 recovery 格式化来“修复”iOS 根镜像。

**没有 SEP / Secure Enclave** → 永远无法登录 Apple ID、无 App Store、无蜂窝、无 Face ID。
GPU 是 CPU 软渲染（20–30 fps）。Wi-Fi/BT/音频/相机初期都没有。这是研究平台，不是日常机。

---

## 七、构建（本机已跑通，无需 GitHub Actions）

```bash
# 前置：docker（EDK II 需要 iasl/nasm/mtools，Arch 上没装）
docker build --network host -t cepheus-build build/

# 外部树（各 pin 一个 commit）
git clone https://github.com/UniXNU/UniXNU unixnu && git -C unixnu checkout b6ae99c
git clone --recurse-submodules https://github.com/Project-Aloha/mu_aloha_platforms uefi
git -C uefi checkout 0b798128          # 2402.86：唯一确认能上真机的树

# 补丁
git -C unixnu apply ../patches/unixnu-cepheus.patch
git -C unixnu/drivers/DarwinBoot apply ../../../patches/darwinboot-cepheus.patch
git -C uefi apply --reject --whitespace=nowarn ../patches/aloha-cepheus.patch
python3 scripts/fix_aloha_tree.py uefi

# 三件产物
python3 scripts/build-firmware.py      # -> uefi/Build/.../RELEASE_CLANGPDB/FV/SM8150_EFI.fd (3 MiB)
python3 scripts/build-payload.py       # -> out/cepheus/payload/payload.img
python3 scripts/build-bootimg.py       # -> out/cepheus/boot/boot-cepheus.img
```

---

## 八、致谢

- [UniXNU](https://github.com/UniXNU/UniXNU) — UEFI/XNU 引导框架
- [Project Aloha](https://github.com/Project-Aloha) — SM8150 UEFI（**自带 `xiaomi-cepheus` 设备目录**）
- [`JohnLeeeeeee678/UniXNU-raphael`](https://github.com/JohnLeeeeeee678/UniXNU-raphael) — 本仓库的移植底本
- [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) — XNU 8796 + 驱动补丁 + iPhone X 根镜像
- `wochgn/UniXNU-guacamole` — SM8150（OnePlus 7 Pro）首次跑通 iOS 16.7.x 用户态
- 本机 `/sys/firmware/fdt` 与 `~/android`（LineageOS cepheus 内核源码）提供了全部硬件事实
