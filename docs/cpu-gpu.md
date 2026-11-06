# CPU 和 Adreno 640 加速工作

**完整GPU/Metal加速尚未实现。** 当前可用基线为GPIO触摸＋软件合成主屏幕＋电源键。
GPU初始化、测试提交、绘制、iOS图形集成分别验收，不能用识别或编译通过代替成品。

## OSM 实测（run 41291850807）

只读探测boot SHA256 `5c4f2312f419a4552b2a4dcdfe81b69152e7ac79f747c54de24f6f8fc40745d4`。
`soc-boot.log`：

| 域 | 地址 | PERF_STATE_DESIRED | LUT目标 | 周期计数约值 |
|---|---|---|---|---|
| L3 | 0x18321000 | 13 | 1536 MHz |1536 MHz|
| Silver | 0x18323000 |15|1632 MHz|1633 MHz|
| Gold |0x18325800|0|710.4 MHz|711 MHz|
| Prime |0x18327800|0|825.6 MHz|827 MHz|

每簇CORE_DCVS_CTRL=0，因此只用core0计数代表共享时钟域；其他槽计数0不是CPU离线。
采样时间由独立架构计时器换算，短采样含读取/调度开销；LUT目标不等于实时精确频率。
当前软件没有动态调频和异构性能调度集成，Gold/Prime持续低频有直接硬件证据。

下一版 `cephsoc=1 cephboost=1` 仅做120秒Gold/Prime实验：扫描固件LUT，在不超过
1.4016GHz且open-loop电压mV与当前相同的候选中选择最大非single-core档位。
Gold为index6、Prime为index5，mV字段均756；virtual corner会变化，由OSM硬件处理。
不写电压/PLL，不关闭LMH限制，不改L3/Silver，不宣称相同mV就等于完全相同功耗。
结束后恢复旧目标；若其他组件改了目标则不覆盖其请求。尚缺持续温度管理，
因此这不是生产级governor或永久强制高频方案。运行中出现高温/不稳定需停止测试。

## GPU 状态与边界

live FDT：chipid0x06040001，GPU0x02c00000，GMU0x02c6a000，GPUCC0x02c90000，
GPU SMMU0x02ca0000。寄存器定义来自本机Android `drivers/gpu/msm/a6xx_reg.h`，
其中GMU是相对GPU的**word offset**，不能当GMU基址下的byte offset。

真机GPUCC：CX_GDSCR=GX_GDSCR=0x222001；GMU_CLK=0x80004220，GPUCC_AHB=
0x80000001。未满足CX上电/GMU时钟条件，探测没有触碰GMU/GX或启动GPU。
GCC CFG_AHB=1可访问GPUCC，不等于图形核心已供电。

本机Android提供的固件（仅记录摘要，不上传专有固件）：

| 文件 | SHA256 |
|---|---|
|a640_gmu.bin|3ff0c02708bbe78641db887fa62f3a7f9337934d0c2ce0b961ef7c43172591d2|
|a630_sqe.fw|a0e1b583f620fabe32729ce367959d1960638663244d7d0cfc21b9a5215a018b|
|a640_zap.elf|74a948e84f0aac830b314ed05d26006b3571c9a3f90600c708ca76c25612660d|

已开始PM4最小测试缓冲构建（`cepheus_a640_pm4.h`）：CP_MEM_WRITE到显式拥有的
GPU scratch范围＋WAIT_FOR_IDLE；离线校验奇偶位、64位地址、边界、容量。
它不执行GPU工作，不创建GPU VA，不能把任意CPU物理地址传入就提交。

仍需按依赖顺序实现：
1. CX/GX电源、时钟/RPMh、GMU启动及HFI协商；遵守隔离/复位顺序。
2. GMU与GPU SMMU地址域、缓冲所有权/缓存一致性、SQE/ZAP加载路径。
3. 受限ring提交、IRQ/轮询完成、fence、超时恢复；先验证scratch写入。
4. 离屏绘制，再接IOSurface、同步和显示合成。
5. iOS Metal/IOAccelerator用户态接口、着色器支持和CoreAnimation接入。

Freedreno/Turnip可参考Adreno命令、编译器和资源布局，但现有Linux/Android接口不能
直接作为iOS驱动使用。MoltenVK是Vulkan→Metal，不会提供缺失的Adreno Metal后端。

## CPU限时实验结果（run 41292760384）

boot SHA256 `da9efd9cb358959e813052f4ab35afdda76d334611699050c6dd1e2501a0df3b`，
kernel SHA256 `916140ce0965f4078e46c05832f85d74198573b34ea1e83d8f0250a6da97d26d`。
已临时启动，没有写boot/userdata。保留触摸和快速熄屏，日志 `cpu-trial-boot.log`：

```
trial gold old=0/710400 new=6/1401600 mv=756 old_corner=0 new_corner=6 duration=120s
trial prime old=0/825600 new=5/1401600 mv=756 old_corner=0 new_corner=5 duration=120s
sample1 gold core0 desired=6 target_khz=1401600 ... rate_khz=1402452
sample1 prime core0 desired=5 target_khz=1401600 ... rate_khz=1402562
restored gold desired=0
restored prime desired=0
sample25 gold ... target_khz=710400 ... rate_khz=710851
sample25 prime ... target_khz=825600 ... rate_khz=826318
```

频率请求生效和回退都有直接证据。CPU频率提升不是界面帧率提升的等价指标；
本次没有足够持续交互样本，不能声称流畅度翻倍。尚无温度闭环/调度整合，
不把这个限时实验包装成长期性能成品。

GPU基础新增 `cepheus_gmu_firmware.h`：依据源码格式规划GMU分块，严格检查
长度、对齐、地址域、重叠和溢出，零长度元数据不当成载入数据。当前测试使用
合成数据，并未通过该模块上传真实固件；仍缺电源、GMU/HFI、SMMU和SQE初始化。
PM4与固件规划模块均不发MMIO、不启动GPU，不构成Metal支持。

## GPU续作：供电证据与诊断修正

USB受控重启/诊断功能已由用户取消；回退提交`1b59abd`的代码树与`35561c0`
完全相同。GPU在独立`feat/adreno640`分支继续，不携带USB控制代码。原有
CDC-ECM日志保留。GPU分支单独选用macos-15/Xcode16.2以避免退役runner。

新的`cepheus_gpu_power.h`及SoC探测补齐以下**只读**检查：

- live FDT与Android `sm8150-gdsc.dtsi`指定CX的`hw-ctrl-addr`为
  GPUCC+`0x1540`。`gdsc.c:poll_gdsc_status`实际轮询该寄存器bit31；
  旧探测只看`0x106c`的GDSCR不足以判定CX实际状态。
- GDSCR的collapse位是HLOS投票，不代表拥有该域；不能覆盖TZ/HYP等投票。
- GMU寄存器只在CX实际on、CFG/AHB/GMU/CXO分支active及RCG非更新/非off时
  才允许本诊断读取。`0xffffffff`一律拒绝，HWCG位单独置位不授权访问。
- 同时记录DDR_AXI、MEMNOC、SNOC_DVM时钟阻塞位，但全部正常仍不代表SMMU
  映射、缓存一致性或DMA所有权已经建立。仍不读取GX或SMMU，不写电源寄存器。

这里发现的实际供电依赖尚未完成：

1. live FDT的CX父电源是`VDD_CX_LEVEL`/`cx.lvl`，GX父电源是PM8150L S2。
   现有触摸用RPMh VRM enable-only接口不能直接当作GPU ARC corner投票接口。
2. CX Linux初始化配置EN_REST=2、EN_FEW=8、CLK_DIS_WAIT=8；快照`0x222001`
   不能证明已执行这些配置。启用前要检查HW_CONTROL，不能夺取硬件控制权。
3. CX状态timeout为500us；GMU启动先开CX，再将GMU clock设为200MHz，依次
   开GMU/CXO/DDR_AXI/MEMNOC/AHB。200MHz来自GPLL0-div与RCG分频，不应为这一步
   随意改写GPU PLL。关停还需尊重共享电源投票和设备树skip-disable语义。
4. GPU SMMU为`qcom,skip-init`；GMU user/kernel SID为4/5、mask为0x400，
   不能清空全局SMMU或破坏继承的安全上下文。

参考源位于本机Android树：`drivers/clk/msm/gdsc.c`、
`drivers/clk/qcom/gpucc-sm8150.c`、`drivers/gpu/msm/kgsl_gmu.c`、
`arch/arm64/boot/dts/qcom/{sm8150-gdsc,sm8150-gpu,msm-arm-smmu-sm8150}.dtsi`。

整理现有草稿为离线模块：GMU阶段式loader（每一步失败清理与不确定资源隔离）、
HFI v2 ring和三层32-bit VA/40-bit PA页表。HFI补上payload发布完成后才更新
write index的顺序；发布或doorbell不确定时将queue标为不可重试，防止重复命令。
实际HFI头部含双方字段，仍需coherent映射或证明cache-line所有权安全的后端。
这些模块目前没有真实供电/SMMU后端，不会启动固件或执行GPU命令。

### 本轮构建产物

提交`d336276`的CI `41389359723`完整XNU构建、10组C/UBSan回归和镜像标记检查
通过，确认新内核含`CX_HW_CTRL=`且不含已取消的USB控制端点标记。
kernel SHA256：`b89909eb2633861d01a79c721e3c76446e6b3e9a6e3bdc2215d6820274de22a1`。

独立临时启动候选：`boot-cepheus-gpu-preflight-d336276.img`，SHA256：
`3799c82a5668409a0402e9851ae94fa0735f872298871afd4a4e12369dda6d92`。
宿主机缺mcopy，使用已有`cepheus-build:latest`容器（禁网络、当前UID）完成
打包，未安装系统软件。保留ADT/UEFI/DTB、GPIO触摸及PON电源键；`cephsoc=1`
只读探测，未启用CPU boost。现有触摸回退镜像哈希再次验证未变。
以下记录该候选的首次真机探测；不能将构建、启动或识别写成GPU加速已实现。

### 真机只读探测（2026-11-02）

核对fastboot序列号`67eea163`、product`cepheus`及镜像SHA后临时启动，未刷写
任何分区。日志：`logs/gpu-preflight-d336276-20261102-1259.log`（cepheus-port目录）。
USB CDC-ECM日志已收到，host使用仅匹配`02:00:5e:77:ee:01`的临时NetworkManager
内存连接，IP `192.168.7.1/24`；不设置默认路由、不修改Wi-Fi。

```
GCC CFG_AHB=0x1 MEMNOC=0x80000221 SNOC_DVM=0x80000001 DDR_AXI=0
CX_GDSCR=0x222001 GX_GDSCR=0x222001 AHB=0x80000001 GMU_CLK=0x80004220
CXO=0x80000001 SLEEP=0x80000000 RCG=0x80000000 CFG=0
CX_HW_CTRL=0x5ff4 gmu_blockers=0x7a memory_blockers=0x37a
GMU powered/clocked state not confirmed; skipped
```

CX硬件状态bit31为0，证实尚未上电；GMU blockers为CX、AHB、GMU、CXO、RCG，
memory另加MEMNOC和SNOC_DVM。DDR_AXI原始值为0，仅表示该状态检查未见halt位，
不证明DDR时钟供电、有效频率或所有权。已把该实测快照加入离线回归测试。
探测按门槛跳过GMU，没有GX/SMMU访问、GPU供电写入或固件执行。

同次日志中SpringBoard已活动，触摸芯片ID`36:39`、active scan err=0，
CPU温度样本38.8°C；快速熄屏clear_us=913。上述是日志证据，主屏幕视觉、
真实触摸和电源键体验已由用户确认均正常。下一阶段需要实现有依据的CX父电源ARC
投票、GDSC初始化及200MHz GMU时钟配置，不能直接跳到固件或Metal。

日志接收器按预设420秒租约退出（systemd Result=timeout/退出信号15），不是
手机崩溃。封存日志SHA256：
`cd6acfd583dbc9c7f6a3cec17a715fbe7f8071064ac127a1a31db57c65925668`。

### ARC只读取证接入（构建通过，真机待验证）

`cephsoc=1 cepharc=1`新增一次只读cmd-db/TCS快照，复用触摸电源传输的同一把
`rh_lock`，不调用rh_write/rh_sync/rh_send。只读取固件locator给出的有界cmd-db，
校验头、地址、重复资源、LE u16电压表与尾部零填充，输出cx.lvl/mx.lvl/gfx.lvl
索引映射。200MHz GMU的VDD_MIN为raw VLVL48；传输数据应为对应的hlvl索引，
不是48mV，也不是Linux的+1消费者编码。CX/MX配对必须满足MX>=CX，诊断只提出
精确48的候选，不会发送投票。所有未知格式与越界输入均拒绝。

同时记录apps DRV2的8个普通ACTIVE/SLEEP/WAKE槽（不访问隐藏CONTROL槽）。
只有已enable且地址精确匹配ARC资源的命令才输出内容，并二次核对槽头是否改变。
**空槽、稳定快照、cmd-db优先级都不证明该DRV当前电源投票为0或归本驱动所有**：
完成后的投票可能仍保存在RPMh硬件，后续写入还需要明确的当前投票读取/继承策略。
此诊断不会将“不确定”转为上电许可。默认无cepharc参数时不执行。

初次CI41393963305在stage阶段失败：stage脚本先于源码补丁执行，不能提前要求
sweet_rpmh.c存在。58a944c修复并新增先stage测试；CI41394365108完整通过。
下载核对的内核SHA256为
`88ff361fc533bcc17ef4b701e8a02fcd0c51f46328cd8a348eb249f0c9070f8f`，符号表确认
cepheus_gpu_arc_probe已链接。用户离开电脑后没有重启手机或部署该内核。

### 电源事务核心（只在mock执行）

`cepheus_gpu_power_txn.h`规定后端必须先持有共享ARC/时钟/GDSC所有权，提供
当前apps-DRV投票和实测GPLL0-div=300MHz，才能计划200MHz GMU序列。它依次：
不降旧投票、先提升MX覆盖CX、再提升CX、配置GDSC等待与collapse、轮询实际
CX状态、配置RCG到GPLL0-div/1.5、使能GMU/CXO/DDR/MEMNOC/AHB/DVM，最后确认
CM3仍在reset。所有寄存器操作限于命名资源；500us有界轮询。每次可能已到达
硬件的失败都保留资源隔离，绝不自动降票、关闭共享时钟或解放DMA内存。

当前**没有XNU硬件后端、没有自动rollback/断电实现**；公共内核不会执行该
事务。prepare/validate_lease接口不是现成所有权证明。mock对每个回调位置
注入失败，验证零副作用拒绝与有副作用隔离；仍需要真实ARC read响应语义和
HLOS继承投票证据，不能把mock完成当成供电功能完成。

### 映射/完成语义补齐（离线）

- GMU页表加入nG位以支持ASID限定失效，HFI缓冲支持非特权RW和Normal-NC属性；
  MAIR约定`[0]=0xff WBWA,[1]=0x44 NC`。CPU映射属性必须与共享内存协议配套，
  不允许把缓存失效回调当成任意混合CPU缓存别名的万能修复。
- 新SMMU计划只接受SID4/5、mask0x400、空且FAULT的SMR槽和未启用CB；拒绝
  重叠stream mask、共享外部CB、越界ASID/PA。输出32-bit VA/40-bit PA、
  4K三级表的TCR/TTBR/MAIR计划，不写全局SMMU。ACTLR/TBU/安全归属、CB/ASID
  预留与TLB同步仍须平台后端实现，不能仅凭空槽判断可占用。
- scratch PM4追加CP_EVENT_WRITE/CACHE_FLUSH_TS(4)完成戳，写入独立fence地址；
  one-shot提交状态机区分数据发布、doorbell、fence观察和quiesce。100ms deadline
  加10000次硬上限，故障/时钟倒退/陈旧fence/停稳失败一律隔离，不复用内存。
- GMU块计划限制最多256记录，拒绝仅元数据但没有ITCM内容的启动；每个阶段
  失败仍由后端先停止再释放，不确定则隔离。

上述新模块仅在离线/mock中执行，尚无真实XNU allocator/cache/SMMU/CP提交后端。
