# Adreno 640 实现路线与验收边界

## 2026-11-07：六缓冲GMU CPU工作区真机回归通过

用户授权继续操作，确认手机在fastboot serial67eea163。使用已通过单缓冲
真机回归的6f93116内核，生成`boot-cepheus-workspace-6f93116.img`，SHA256
`108be7a468172c552d732c5c04e5ea79ee2da3d90c69c082d937e77805c1f149`。
打包主机缺mcopy的第一次尝试未生成镜像；随后在已有cepheus-build:latest
容器（network none，实际mtools）完成打包/核验。相对ready-9dc6e6e只改
核文件与参数，UEFI/DTB/ADT/其他payload不变，cephmem=2，无GPU启动。

先启动UDP接收器，再fastboot boot，Sending/Booting均OKAY。200秒有界
日志129276B，SHA256 `0fcb2caf8de8f3bb8918b4d264525e4a8eaacb47f8a98a6343f0a29a4935c176`，
记录在`../logs/workspace-6f93116-live-20261107/{boot.log,fastboot.log,result.json}`。
六个NC buffer PA依次0x1843d4000/0x184494000/0x1844d0000/0x1844d8000/
0x1844dc000/0x1844e4000；实际size为0x3c000/0x3c000/0x8000/0x4000/
0x4000/0x8000（16KiB host页上取整，原设备页4KiB）。窗口为
[0x181004000,0x241004000)，全部PA超过4GiB，映射未触发旧截断panic。

CPU workspace table_pages=6、HFI VA0x60100000、log VA0x60200000，
release=0/state3。**没有安装SMMU、发布DMA、运行GMU或提交CP命令**。
此内核没有单独的retained_handles日志，不伪造计数；成功release只说明该
CPU清理函数返回成功。后续active scan err0、ST-FTS选择、one-shot Home与
帧呈现继续，捕获未见panic。日志含blank clear793us，但当前UI/电源体验仍
等待用户确认。手机留在运行状态，只停止HOST接收器，未flash或自动重启。

旧内核仍有no exact pair/ACV lookup=-1/config=-1；不把它当作新修订验证。
ARC/ACV修订b04b259的CI42249859913已完成前置测试，当前Build XNU进行中。

## 2026-11-07：用户确认内存回归UI正常；ARC/ACV拒绝定位续作

用户确认memcheck-6f93116主屏幕、触摸、电源正常，允许继续逐阶段验证。
这补足下方先前记录的UI确认缺口，但仍只证明基本启动与单缓冲NC回归。

纠正：旧日志的`no exact MIN_SVS pair`来自诊断入口；HFI编码器原本已经
向上选档。不能把同时发生的config=-1完全归因于CX无48：同一日志还报告
ACV lookup=-1，没有其原始aux字段。因此config的实际拒绝环节尚待细分。
按本地KGSL `setup_volt_dependency_tbl`，电源计划/诊断改为primary ceil(48)，
MX再ceil(rounded primary)。本机已捕获CX/MX表的结果为CXindex2/raw64、
MXindex2/raw128；HFI GMU票为`0x00400202`，仍NOT SENT。不选最高档代替
ownership，不降继承票。KGSL对secondary不足时会饱和到末档；本实现保留
更严格的拒绝，不将低于primary的MX列为有效依赖。

ACV位投票入口单独处理：允许固定字面名称ACV的unit/width为0，不让任意
BCM绕过除数验证；AB=0、IB仅0或apps bit0x8。保留8字节aux、对齐、范围、
保留字节、VCD、重复项检查，普通BCM零除数仍拒绝。零元数据是测试假设，
**未取得本机ACV原始字节，尚不能确认旧拒绝因此产生**。原厂源码没有在
`msm_bus_bcm_init`拒绝零字段，ACV单独编码；本实现也不对零字段做除法，
不把此边界修复当作原厂宽度/聚合结果或真实带宽租约证明。

新增bounded ACV原始entry/aux诊断（失败时可标明未读aux），以及config
rails/DDR BCM/CNOC BCM/routes/PDC/wire分阶段负错误码。用于下一版只读
日志定位，不授予任何电源/DDR/PAS/SMMU所有权。测试覆盖日志中CX/MX表
经synthetic carrier进入生产config、GMU票、ACV bit、普通BCM零除数拒绝、
reserved/短aux/越界拒绝。`test-arc-bcm-source.py`新增显式原厂源码对照。
GMU workspace自检增加六个backing PA/bytes及retained_handles/leased_mask
清理日志，不改变CPU-only准备/释放行为，不发布DMA地址。

离线receipt `../logs/gpu-offline-arc-bcm-20261107/receipt.json`共25项PASS、
零SKIP，包含62个host程序、sanitizers、7生产单元静态分析、真实固件RAM
fixture和ARC/ACV源码对照；receipt如实标明f441a35 dirty工作树，不冒称已
提交修订的构建。本节尚无新CI或cephmem=2真机结果。

## 2026-11-06：PA截断修复单缓冲真机回归通过

用户授权代理操作已连接手机。先确认fastboot serial67eea163与镜像SHA，
启用有界UDP接收器，再`fastboot boot boot-cepheus-memcheck-6f93116.img`。
Sending/Booting均OKAY；没有flash、GPU供电写入、PAS或自动复位。
主机现有NetworkManager配置按ECM MAC02:00:5e:77:ee:01自动恢复192.168.7.1，
不依赖sudo。设备仍留在运行状态，只停止主机日志接收器。

真机日志完整观察到allocate -> prepare -> map_nc -> pattern PASS -> release0 ->
stale_release-1 -> retained_pages0，32KiB buffer PA=`0x184394000`、host页16KiB。
本次实际managed window=`[0x181004000,0x241004000)`，mask=`0x241003000`。
加载器窗口本次与旧日志不同，不能将某一固定gPhysBase当作所有启动事实；
运行时读取bounds是必要的。这次PA经旧32位shift会截成0x84394000，修复后
NC transition成功，直接支持底层cache维护截断纠正。

捕获中没有panic，后续ST-FTS36:39探测/active scan0、one-shot Home完成、
帧提交与呈现继续。尚无本轮用户UI/手势确认，不把驱动日志当作体验验收。
原始125707B日志在`../logs/memcheck-6f93116-live-20261106-222832/boot.log`，
SHA256=`c4e6b0329630d598c32b510d8c0b3ec853685d3b84f3bac97d6b92567e14541c`；
同目录result.json绑定镜像/内核/结果。仅单缓冲回归，不是GMU workspace或GPU加速。

新增真实阻塞：cmd-db CX raw levels为0/16/64/128/192/256/384/416，
没有exact MIN_SVS48；ARC probe报no exact pair，GPU config dry-run=-1。
GX因GMU未供电/上时钟正确跳过，**没有任何GPU命令/PAS测试**。
后续需按真实ARC rounded level/dependency语义修正源绑定配置，不能为pass伪造48。

## 2026-11-06：memcheck再次panic，纠正physicalMask根因

`boot-cepheus-memcheck-9dc6e6e.img` 真机失败。用户照片为CPU7，非法PA
`0x8439c000`，gPhysBase=`0x101004000`、real_phys_size=`0xc0000000`。
此镜像不再推荐。UDP捕获94780字节含cephmem=1参数，但**没有CEPHEUS阶段
或panic尾部**，不能声称它提供了完整backtrace。照片转录/身份在
`../logs/panic-memcheck-9dc6e6e/panic-transcription.txt`，原日志SHA256
`bbb42393b07fca90ec08f6b4080bb797ca75ef51071591d1d5ba935a7da4a3ac`。

源码与实际内核机器码揭示此前漏查的根因：
`pmap_sync_page_attributes_phys(ppnum_t pp)` 使用
`(vm_offset_t)(pp << PAGE_SHIFT)`；pp是uint32_t，转换发生在32位左移之后。
已部署内核该函数0xfffffe0007475328首指令为`lsl w0,w0,#14`，确实截断PA。
合法窗口内的`0x18439c000`经过该转换恰好变为照片中的`0x8439c000`，而前两次
`0x843e4000`/`0x843b4000`也有同样高地址截断解释。**此前把这些低PA视为
分配器真的返回窗口外内存，是未成立的根因推断**；不能仅凭panic证明allocator
越界。保留allocation bounds与NC有效属性门禁，但它们修不了底层cache callback。

caller0xfffffe0007afc1c8是`phystokv_range.cold.1`中BL panic的下一地址，
恰好又是nm的`ml_static_vtop.cold.1`起点；按最近符号归属曾误归因。
这是return-PC边界，不是cold合并证据。一次REA内核分析超时，未算分析成功；
实际llvm-objdump与源码才是本次定位证据。

新0088补丁改为`flush_dcache((vm_offset_t)ptoa(pp),...)`，ptoa先扩展为
vm_address_t再移位。**Pong的Darwin24补丁也已修了同一处**；此前参考调查只看
GPU目录，没有追到内核pmap通用补丁，本轮补足。未改cache策略，不删检查，
不映射低PA、不强制override。回归直接应用production patch并编译该函数，
4/16KiB两种host页均重现旧低32值并确认新64位正确；linked gate拒绝已panic内核。
新内存phase日志仅帮助定位，不能代替底层修复。仍需完整CI与再一次真机回归。

CI补证：42210687545 @6f93116 completed/success，实际内核callback首指令
验证为64bit PA UBFIZ（16KiB host page），内核SHA256
`bb9f547dd8eadf46d3e587c96cdbf0ab2f3f4c47288cf83a5421f3af9d3e7ec1`。
新镜像 `boot-cepheus-memcheck-6f93116.img` SHA256
`38e28ba3cd34142985a50dcf79c1f4c2ef1feefe9f8e7dc2f25c06f151a3e003`，
包核验通过，未在手机启动。保留原UEFI/ADT/触摸与cephmem=1，不加自动reset。
离线receipt ../logs/gpu-offline-513a44d/receipt.json为24项PASS零SKIP，
范围包括patch应用、回归codegen、linked gate parser、原固件fixture与静态分析。

## 2026-11-06：多次真机回归准备（尚未启动）

用户取消单次机会限制，要求可测试时直接告知。现生成
`boot-cepheus-memcheck-9dc6e6e.img`，SHA256
`cbf16f00cd8499f806c7b529ee0947d101dccce3736c13fd788703e9c0db0d40`。
与已验证ready镜像使用同一内核/UEFI/DTB/ADT，仅增加cephmem=1 CPU-only
单缓冲NC分配/读写/释放自检；不是GPU上电/PAS/命令测试。全部包内容检查通过。
fastboot只读枚举确认67eea163存在，代理没有启动镜像或刷写。
需要开机前UDP监听和ECM地址watcher，以取得完整PA/缓存/自检日志，不能只看UI。

## 2026-11-06：唯一真机机会前的源码收敛（机会未使用）

用户要求继续至GPU可用，手机在fastboot，仅一次启动机会。因此没有启动只读
候选去消耗机会；真实power/DDR/SMMU/PAS/stop仍需先准备完整，不能以“理论实现”
或一轮host模拟冒充最终加速。

- 复查power事务发现独立RCG UPDATE store没有在成功read之后重验lease，
  与其他RMW路径不一致。现在写前重验；测试在read后丢lease，断言不再写。
- CM3 reset==1原来仅在电源/时钟效果之后验证，不能将未知/非reset状态作为
  冷启动输入。前置读取与拒绝，末尾再验证保留；测试确认非reset时零投票零写。
- 新`cepheus_gx_llc.h`只编码A640 LLC配置：已取得**真实激活slice租约**后，
  `GBIF_SCACHE_CNTL1`(0x3c02)低25bit重复5个SCID12，保留高7bit。
  GPUHTW(SCID11)由XBL NoC override设置，A640不在GX路径改写；MMU-500
  不支持GBIF页属性覆盖，因此不写CNTL0或CX_MISC缓存控制。
  不把FDT的slice ID当作激活证据、不提供LLCC共享硬件后端，仍保持CP halt。
- `test-llc-source.py`与本地KGSL/llcc-sm8150源独立对照，RAM用例覆盖高位保留、
  每lease失败、错SCID/芯片/短aperture/解除halt/fault拒绝。
- 对本机ScmDxeLA.efi做REA静态调查，确认SMC64路径同时依赖BootServices TPL、
  shared allocator与listener回调，不能直接转用DXE指针作为XNU后端。
  记录`docs/scm-static-evidence.md`含完整artifact SHA/Evidence IDs。
  `smc`可达不证明PAS13存在/认证成功/可安全超时返回。

新增61个host程序通过与7生产单元静态分析；仍无GPU实际命令退休/Metal证明。

进一步新增受限SCM discovery引擎（62个host程序）：仅INFO call-available和
PAS13 supported查询，不发PAS init/mem/auth/shutdown，不传共享buffer。
64->32 convention只在明确不支持/无可用值时回退，transport错误/busy不重试；
INTERRUPTED保留返回寄存器含x6继续，整个引擎最多16次调用。
`cepheus_gpu_scm_probe_once`提供真实XNU线程上下文SMC桥和序列mutex，
**未接入任何启动钩子/boot参数/用户态接口，未在手机执行**。
它不使用DXE函数指针，也不能证明secure call会及时返回。调用次数有界不等于
墙钟超时可中断，不能据此启动ZAP或清CP halt。真实PAS13支持仍是未知。

完整CI `42168758490` @007a683、`42169779972` @637bc7a均success。
后一项真实XNU编译了未hook的SMC线程桥，内核SHA256
`2d3ed01858fd85eaab794ef2d2e04bfdbc9591fcfbdcd037e99c47308ea31d9e`。
后续1d24457的拒绝字段修正只经host/source/codegen验证，未包含在该CI内核。
未启动手机，唯一验证机会仍未使用；完整证据与硬阻塞详见`gpu-one-shot.md`。

## 2026-11-06：ready 镜像基本启动验收

用户自行启动 `boot-cepheus-ready-9dc6e6e.img`，随后确认主屏幕、触摸、
物理电源键与熄屏“都正常”。镜像SHA256再次校验为
`2e1e7e2c8e04cd243e62de090898fd8234c1d8b14755ba6504f64d6243ab5621`。
因此可作为新的基本启动基线，旧触摸基线保留回退；未覆盖任何镜像。

代理仅开机后接收20秒USB UDP日志，来自192.168.7.2，记录见
`../logs/ready-9dc6e6e-live-20261106-124516.log`。包括用户态exec和USB网络
计数；没有完整早期启动日志，也没有本次GPU诊断结果。接收窗口中
submitted/presented=0不能代替用户的主屏幕确认或面板帧率测量。

**未验证项不变**：该镜像不启用cephmem，故不是physicalMask/panic修复
真机回归；GPU上电、GMU启动、CP提交、Metal加速未启用；上游新版UEFI
未混入、未测；也未做长时间稳定性/多点手势/完整休眠恢复验收。
代理没有重启、启动镜像或刷写手机。

## 2026-11-04：cephmem panic 与构建结论纠正（未复测手机）

用户本轮禁止启动镜像/刷写；以下仅源码、host 和 CI 工作。

- 真机二分：无 cephmem 正常；cephmem=1/2 都曾 panic，非法 PA 为
  `0x843b4000`/`0x843e4000`，低于 `gPhysBase=0x101004000`。
  一个日志给出的 size 为 `0xc0000000`，窗口终点 `0x1c1004000`。
  此前 d336276 preflight 没有启用 cephmem，不是内存测试通过证据。
- `87c9791`/`0cd11d4` 的 physicalMask 修复**不完整**：mask 只能给
  maxPhys 上界，不能给分配器下界；检查即使移到显式 createMappingInTask
  前，也可能晚于 IOBMD 构造期间的 cache transition。
  `IOBufferMemoryDescriptor.cpp:394–419` 对 cache flags 设置 persistent，
  `IOMemoryDescriptor.cpp:2332` 调 memoryReferenceCreate；后者携带指定
  cache mode 调 mach_make_memory_entry_internal。不能断言 panic 一定只在
  外层 mapping 发生。现在分配时不传 cache flag，仅在检查后显式请求 NC。
- 每页所属范围以整个连续 segment + host-page 对齐校验，检查先于 NC map；
  无效/空/超过40位的窗口在分配前拒绝，无固定40位 mask 兜底。
  任意“向下取整到页”的 mask 仍可能因低位连续零被 IOBMD 解成过大对齐和
  扩大上界；现在固定12位低零、保守向下收紧终点并测试真实解码式。
- 保留 `IODefaultCacheBits` 的每 host page 实际 WCOMB 检查，不接受只看
  map options，不强改未拥有页属性。host 双替身现提供真实布局的 bounds
  和 cache query，执行同一检查代码；两次 panic PA 均断言**零次 map 调用**。
  这些 mock 仍不能证明 XNU 分配器会返回可用 PA，或真机 panic 已消失。
- `41845027677` 真编译错误是 `pmap_paddr_t` 对 IOKit TU 不可见；修正提交
  实际是 `197fb57`（此前答复误写 `4c3f5cb`）。其余 BearSSL 502 是传输故障，
  不是编译成功/失败的代码证据。
- “没有同 SHA 镜像”结论错误：用 gh search commits 找到多个同对象镜像。
  AmanoTeam/BearSSL 已实际 fetch、fsckObjects 校验并核对原 commit
  `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f` 与 tree
  `a8f933a27b14eaf3a348fe9b294bf4262dbb216c`，另一镜像 API 一致。
  新 prepare 脚本只换传输源并追加 tree 检查，不换加密实现、不变版本。
  之前“2018版附近”的判断也不成立，该提交元数据为 2026-04-06。

原始日志保留在 `../logs/gpu-gxprobe-3e41cb8/boot.log` 和
`../logs/panic-cephmem2/`。新补丁的硬件效果仍待用户回来后另行授权验证。

构建补证：`41875375125` @`9dc6e6e` 的 Build XNU、必要补丁/符号检查、
上传步骤均为success；实际下载的内核SHA256为
`1ddd182f99a2934870cb3870140db41eef2cc8cde6ccf138a2841143daf62958`，
`verify-touch-kernel.py` 再次通过。日志明确stage了原BearSSL `7bea48e5`。
内核和日志分别保存于 `../inputs/ci-41875375125/`、`../logs/ci-41875375125/`。
记录时 GitHub run/job整体API仍in_progress，尽管Complete job步骤也为success；
不将步骤结果冒称整体平台状态已经收尾。

最终源码离线receipt `../logs/gpu-offline-9dc6e6e/receipt.json` 为干净提交，
19项PASS零SKIP。两个独立review子代理均超时，无完整审查报告，未算审查通过。
本轮未生成/启动新的boot.img，未改任何手机分区。

随后用户要求提供启动镜像：生成 `boot-cepheus-ready-9dc6e6e.img`，SHA256
`2e1e7e2c8e04cd243e62de090898fd8234c1d8b14755ba6504f64d6243ab5621`。
这是修复版内核配旧的已验证UEFI/DTB/ADT的启动候选，**不启用cephmem**，
不运行CPU boost/GPU上电；不是physicalMask真机回归通过证据。触摸/电源和
只读诊断保留。全部其他payload文件与触摸基线相同，receipt在dist同名json。
CI41875375125整体状态现已completed/success。本次仍未在手机上启动。

**后续独立审查修正优先于本文早期阶段描述。** 首版内存池未显式创建NC映射、
workspace持有view却无法发布；这两点不是可用GPU链路。下方“独立审查修复”
记录后续显式IOMemoryMap、批量共享和真实协议退休实现。所有硬件结论仍待验收。

**最新布局修正：** GMU持久dummy页为0x60000000，HFI为0x60100000，log为
0x60200000；工作区现有6个分配。早期“五buffer/HFI首槽”描述仅为历史，不能
作为实际固件布局。依据kgsl_gmu.c的“WB DUMMY PAGE Must be the first alloc”。
持久页独立分配、privileged RW/NC/NX，VA计划共5段；table需要6个4KiB页，
预留的8页仍足够。全部发布/退休/诊断句柄和联合HFI测试已改用统一数量常量。

整合入口`cepheus_gmu_bringup.c`现按实际依赖串接sealed固件、workspace、电源、
PDC、SMMU、上传、CM3及同一HFI session。七阶段失败测试保留对应状态，不继续
下一步。仍无可直接启用的真实platform bind/lease实现；整条链只在host模拟
硬件上执行，不能据此宣称手机GMU已启动。启动前先校验完整OPP/BW/PDC输入，
不允许在硬件副作用之后才用临时空表补配置。

整合路径显式在PDC/上传前设置GENERAL_7 TCM retention（不再隐含于backend
begin约定），并为bin1在CM3启动前走真实QMP固定ACD消息；bin0不发送该通知。
host整链测试覆盖bin0和bin1两个完整序列及七阶段失败，仍不执行手机硬件。

已发布工作区的启动失败现在触发one-way abandon：将整个组隔离、pool设为
terminal、保留所有backing和身份、清掉对外工作区指针。不是仅调用failed日志
回调后继续保留SHARED就绪状态；测试对PDC/SMMU/upload/CM3/HFI失败均核对。

### 本机配置接入（离线，不是平台租约）

`cepheus_gpu_config.h`把固定Cepheus A640v2 profile接入现有解析/编码器：
GPU OPP、两个bin的HFI索引、12档DDR与2档CNOC、三个BCM/路径、ACD真实12个
数据word、PDC地址均与保存FDT逐项对照。GMU 200MHz的raw VLVL48另来自
`gpucc-sm8150.c`的VDD_MIN及`vdd-level.h`/rpmh-regulator定义。KBps使用
`msm_bus_of.c`的`*1000ULL`，最高DDR的8,363,000,000B/s不截断到32bit。

配置器必须接收cmd-db和QFPROM word；ARC名/BCM名经原解析器查找，PDC按地址
反查，缺失/非法/供电表不足拒绝且不改输出。没有默认BCM地址/unit/VCD，
也没有当前DRV投票或所有权假设。调用者仍需验证live FDT匹配此profile、取得
真实fuse和稳定cmd-db；当前没有接入启动钩子。

整链host测试已替换简化BW及虚构ACD表，检查实际HFI ring发出的PERF/BW/ACD
payload；cmd-db与寄存器仍是明确标记的测试替身，不是设备采样。
`CEPHEUS_LIVE_FDT=/tmp/live-cepheus.dtb python3 scripts/test-gpu-fdt.py`
会编译实际C profile、读取其报告并和独立FDT解析逐项比较。39组host测试通过。
完整整合基线CI41503204545（988f919）通过，kernel SHA256
`dc8079790a7de185e7c5a5788e4d58635c7c522dc5b9ca9bfd5d75e02402f0e5`；
后续配置/失败隔离修改仍需新的完整CI，以上不等于真实GMU启动成功。

### GMU设备页表权限源码核对

`kgsl_gmu.c:gmu_memory_probe`分别在persist/DCACHE/ICACHE/log使用
`IOMMU_READ|IOMMU_WRITE|IOMMU_PRIV`，HFI使用RW无PRIV；这些映射均无
`IOMMU_NOEXEC`。`io-pgtable-arm.c:arm_lpae_prot_to_pte`将它们编码为
privileged AP=0（HFI AP=1）、Normal-NC、XN=0。此前CPU式W^X限制导致ICACHE
只读，并人为给其他四区加XN，不能以离线测试证明这些收紧在GMU上可用。

现按原驱动修正设备PTE，保持nG/AF/outer-shareable和无物理别名；不更改CPU
pmap/IOKit映射或安全固件认证。S2CR PRIVCFG保持默认，不强制提升所有访问；
SCTLR无WXN/UWXN覆盖。逐页测试包含实际C workspace构建的全部PTE。
`python3 scripts/test-gmu-permissions-source.py /path/to/android/kernel`
从实际KGSL分配调用提取权限后与C页表逐项比较，通过。是否解决具体硬件fault
仍未知，当前没有GMU执行证据。

独立复核子代理b4fca0ed在1800000ms超时，未给出正式结论，不能作为审查通过；
上述更改依据父代理的源码核对与自动对照测试。失败日志保留，未换CLI重试。

### AOP QMP具体传输（尚无平台连接/租约）

FDT的GMU `mboxes` phandle解析到AOP，MSG RAM=`0xc300000+0x1000`、通知寄存器
`0x17c0000c+4`、mask1、descriptor offset0。`cepheus_qmp_mmio.h`读取实际64字节
描述符，检查magic/version/features、两端link/channel和全部ACK、TX/RX边界及
不重叠；双次读取不同则拒绝，已有TX/RX内容不清除。仅绑定已连接的外部独占
租约，不自动协商、不写描述符、不抢占其他QMP客户端。

发送适配只准许ACD固定文本的8次顺序payload写、最后length32和一次通知。
每步检查租约及完整descriptor身份；状态改变、消费超时或租约失效会停止，
不重试、不清邮箱。来源ABI测试与FDT资源解析通过，40组host回归通过，bin1
整链测试使用真实volatile适配器+RAM mailbox而非万能成功回调。连接协商、
实际共享客户端仲裁和映射生命周期仍需真实XNU平台实现；descriptor稳定不能
证明跨重启ABA或独占所有权，消费ACK也不等于AOP应用层ACD生效。

### 退休路径具体寄存器接入

`cepheus_gmu_retire_mmio.h`将slumber状态/故障和park四TCS/CM3 reset/GBIF
连接到具名volatile读取/受限写入。GBIF位于独立GPU aperture的word0x3c45/46，
不能用GMU aperture相对偏移访问。每次park操作检查真实RPMh状态15；reset还
检查GX off与TCS全部排空，halt只允许保留其他位后OR2且reset必须已保持。
真实协议完成仍由已有retire orchestrator保证，单独寄存器状态不是HFI ACK。

原生产workspace退休host测试现在通过此具体适配器访问两块RAM，而非枚举到
假状态值的快捷回调；全部成功/失败保留场景通过。41组host测试通过。平台的
实际映射、资源持有和禁止新提交仍未实现，不会据此自动退电或释放未知DMA。
配置版63adbf1的完整CI41561755541通过（kernel364448bd...）；权限修正版
11f9ec1的CI41562697282通过（kernel4580d810...）。QMP/退休接入待后续CI。

后续be396a6已由CI41564224598完整验证，kernel SHA256：
`c9dff1f27c3e2253844c7fbb23947edfc562f7f7429dfb7912515f57c9abdc1f`。
`cepheus_pdc_mmio.h`进一步连接三个实际aperture，以48项plan为唯一写白名单，
强制顺序/值匹配，拒绝活动PDC、未reset CM3或GX未off。全回调失效测试通过。
整链GMU host测试已统一PDC/CM3/HFI doorbell所用GMU RAM aperture，执行真实
volatile适配器，仅硬件响应仍由测试替身模拟；不再用独立枚举值掩盖地址错误。
42组host回归及实际固件模拟上传/启动通过。该PDC增量仍需新的完整CI。

启动与退休整链中的SMMU也已改走`cepheus_smmu_mmio.h`具体具名地址/32或64bit
load/store；页表publish回调实际遍历当前workspace核对映射并执行barrier，
测试不模拟成“只调用成功即完成”。RAM硬件状态/ID仍是显式测试fixture，不是
真机能力探测或cache/TLB可见性证明。原所有成功和失败场景通过。

HFI共享workspace现绑定第三个debug队列（真实header索引40/41），有界处理
id101的4word debug记录和id100的18word错误记录。未知/畸形消息、坏索引或单次
超过32条均锁存错误，保留数值快照，不把固件字符串当printf格式；没有队列重置。
会话在收到ACK后再次执行fault/debug检查，覆盖ACK与异步ERROR同时到达的
原盲点。测试实际共享队列索引、debug消费、坏索引、消息洪泛、未知请求、
ACK+ERROR和失去租约；公开CI不含真实固件。

两种bin的整链测试进一步闭环：在启动成功后，不重置SMMU寄存器RAM、不替换HFI
session，直接用同一published workspace完成PREPARE_SLUMBER→park→detach，
最后核对C++分配/prepare/映射计数全归零，group不可再查，HFI序号连续增加。
这是原生产模块在RAM硬件模型上的集成证明，绝不是SM8150实际TLB/DMA证据。
PDC版CI41565725731（19ff9df）已完整成功，kernel SHA256
`b45791dfd379bb0e5667080a169dfbc6b61f4e79e35a2063931b40c0c2787dd5`。

启动失败清理进一步撤销upload maps中的CPU VA以及TCM别名，并把uploader锁存为
quarantined，保留写入计数但不保留可重试入口。CPU-only workspace准备失败时，
固件snapshot的complete若失败则保留唯一句柄，不误记为已释放。新增第11个整链
fixture制造pool满+complete失败核对该句柄。MAIR源码测试也已区分slot编号与
实际属性：KGSL NC slot0、本实现NC slot1，比较解码后的0x44而非宣称PTE数值相同。

六缓冲workspace发布仅接受SID5（gmu_kernel）。KGSL的allocate_gmu_kmem对
包括HFI在内的这些缓冲均调用GMU_CONTEXT_KERNEL；HFI的unprivileged AP并不
表示映射到SID4。通用SMMU planner仍可规划SID4，但不能误用来发布此workspace；
新增独立拒绝测试。用户context必须另行实现，不能混用内核root。

`cepheus_gpu_fault_mmio.h`增加只读具名故障适配：RBBM/CP错误、GMU AO/HFI、
所选SMMU CB的FSR/FSYNR0/64bit FAR。每次读检查外部域租约，GX还要求实际
状态on；未授权域不读、缺失不能当clean、所有寄存器均无清除接口。
43组host回归通过，包含每次域lease失败、GX off拒绝、缺域、不清故障和FAR
落点检查。真实租约/映射仍需平台提供，此模块不在启动钩子自动调用。

`cepheus_gmu_upload_mmio.h`把ITCM/DTCM固定为同一GMU aperture的byte0x3000/
0x7000各16KiB（KGSL GPU word0x1b400/0x1c400）。上传前后检查CM3 reset与
故障，发布执行`dsb sy`，只接受已有NC工作区；整链测试不再另造两个TCM数组。
44组host回归和本地固件11场景通过，尚无真实总线写入或GMU执行。

修复pool terminal并发边界：IOKit分配在锁外运行，回锁后必须再次检查terminal，
否则另一条路径在分配期间abandon可导致仍交出新句柄。未发布的新分配安全
CPU清理，旧CPU buffer与整组发布也拒绝terminal。确定性mock hook在真正的
锁外分配中触发abandon，核对新句柄被拒绝、六缓冲保留且旧句柄不能发布。
group_status不再读取正于锁外complete的ALLOCATING buffer字段，避免诊断竞态；
RETIRING期间计数仅表示当前稳定槽，最终状态仍保留完整组身份。

`cepheus_gmu_bringup_retire`是整链正式退休入口：要求同一HFI session/plan，
调用生产workspace退休协议后撤销HFI/upload别名，成功状态为RETIRED，失败保留
组身份并调用failed。非法输入且尚未进入退休时不破坏仍SHARED的工作区。
本地固件12场景覆盖闭环成功、重复/异源退休拒绝和GBIF ACK超时（不得detach/
释放，完整六缓冲保留）。退休成功仅表示内存安全撤销，**不表示**已降电源票、
释放PDC/时钟或真实平台租约，后者仍需单独实现。

整合启动在bind返回后重编码并比较预检时的PERF/BW/ACD/PDC序列及bin策略，
拒绝映射回调偷偷替换已验证policy（不比较含padding的struct）。新增第13个
场景改变最低FW版本后拒绝于BOUND，未发布工作区/写retention/PDC。策略生命周期
仍要求调用者独占与immutable，不能用该检查抵御恶意内核代码或并发写policy。

HFI整链的fault回调现通过真实只读适配器读取GMU+SMMU（required domains=6），
而非永远返回0。新增第14个场景在ACK同时注入SMMU FSR，必须拒绝成功、保留FSR
及全部workspace。全GPU健康接口仍要求7域，不把未上电的GX报告为干净。
混合生产C/C++全测试可用`CEPHEUS_GPU_HOST_ASAN=1 ASAN_OPTIONS=detect_leaks=0`
运行，14场景通过ASan+UBSan；有意隔离保留backing所以关闭退出泄漏检查，
这不构成“无保留分配”证明。新增MMIO地址源码对照也通过。

### 受限QMP冷连接（离线实现，平台所有权仍缺）

`cepheus_qmp_connect.h`补充原来仅支持已连接mailbox的缺口：只接收AOP已公布
有效layout、remote link UP但所有local/两端channel与ACK为DOWN的冷态。
仅写host拥有的ucore link ACK、mcore link、mcore channel、ucore channel ACK
四字段，按link→channel等待remote ACK。不写远端ACK、不初始化/清除任何邮箱
payload、不接管活动或半继承channel；未知mailbox layout不猜尺寸。

总5秒+次数上限；peer先open再等本端ACK也能完成，不先等local ACK造成死锁。
成功直接保留原外部租约给ACD发送器，失败后可能已有写入则terminal保留，
不reset/断开重试。45组host回归、源码字段offsetof核对和bin1整链cold connect
→ACD→GMU启动→退休已覆盖，全部仅在RAM上。真实共享AOP客户端仲裁、启动时
可用冷态及映射生命周期仍未取得，不能自动在手机上调用此流程。

### 下一轮只读配置验收

同时启用cephfuse=1/cepharc=1时，soc先完成两次一致的QFPROM读取，atomic公布
结果；ARC探测复用rh_lock与本次cmd-db，调用纯配置dry-run输出选中bin、HFI
索引/候选packed vote、DDR/CNOC地址与首末档数据。没有fuse样本则跳过，绝不
默认bin0。dry-run不调用power/HFI/QMP/CM3写路径，所有值标记NOT SENT，明确
live FDT profile未验证、ARC所有权UNKNOWN。此新诊断尚需构建/真机验收，
不能凭打印了可编码配置就启用GPU。原有触摸enable-only电源功能不受改动。

SMMU install与detach现统一要求本机ACTLR0x303；detach在任何撤销写入前比较
ACTLR，属性变化即拒绝、不释放。原通用测试的0x10示例已替换为实际值并增加
错ACTLR零副作用负例，所有stage/闭环/ASan回归通过。

GMU uploader在publish屏障后、reset仍保持时回读全部已写TCM DWORD及Normal-NC
cache字节，逐段比较sealed固件。原厂snapshot代码确认TCM窗口可读，但此处
仍需真机确认总线行为；RAM回读不能替代它。新增TCM/cache损坏负例，以及
第15个整链场景在publish后破坏ITCM：必须失败于UPLOAD、不得写CM3启动配置、
不得开始HFI；已发布workspace隔离。ASan/UBSan与45组回归通过。

workspace原子发布现在从池内真实view重新核对各角色容量/PA、不重叠、SID5、
全部materialized PTE与HFI初始header，不再只凭六句柄+root地址。页表验证遍历
全部有效描述符，拒绝额外映射、内部页权限改变、block/table非法位、表页循环/
别名、隐藏非零页；最大64页，无临时页表分配。CPU selftest也调用此验证。
新增交换ICACHE/DCACHE句柄、篡改中间PTE、洞中额外映射等负例；发布拒绝原子
且不改原工作区，完整ASan/UBSan闭环通过。此防错不替代内核单一所有者约定。

整合启动必需gpuss0/1采样与单调时间回调，先启动≤30秒thermal guard，在电源
副作用之前、每阶段之后检查；HFI实际shared访问也包裹该检查。新增启动前过热/
单传感器无效零电源副作用、中途过热不上传不启动，以及启动后期限到达仍可走
正式RETIRING/slumber停止协议的测试，共19场景。温度锁存不能阻止必要的停止
命令，但只在正式退休状态允许；没有清除thermal failure或放行普通命令。
这仍不是完整热安全系统：长阶段每个真实lease需检查、成功后空闲需要独立
期限/温度监督，失败隔离也不会自动降电源票；实际停止/降电后端未完成前，
禁止真机上电试验。

`cepheus_gpu_tsens.h`增加实际只读TM aperture采样：后经FDT热区修正为TSENS0:15
与TSENS1:11，status0xdc/0xcc、TRDY0xe4 bit3、valid bit21、signed12bit×100m°C。最多各3读，
均需valid；不采用“三次无效但相等也算有效”的回退，不写恢复/阈值/校准。
前后租约/ready与单调时间核对，采样耗时>100ms拒绝。整链温度回调已改走
此适配器+RAM状态，不再直接返回伪温度struct；46组host与19场景ASan/UBSan通过。
实际传感器所属/有效性仍需下一次只读真机采样确认。

### CP ring/SQE预配置（不启动CP）

`cepheus_cp_ring_config.h`按KGSL rb_start/microcode_load生成并执行7项地址/控制
写入：RPTR地址、RB_CNTL0x0800020c、32KiB ring地址、SQE地址。要求外部持有
GX/fence/OOB/SMMU/固件身份、SQE已halt、冷态ring/SQE/RPTR且无CP fault；不会
自行halt继承引擎或覆盖旧地址。三缓冲VA/长度/40bit边界和不重叠必须满足。

不写SQE_CNTL/WPTR/TRUST_CONTROL，不发送unsecure包，不绕过ZAP认证。此模块
仍缺真实GX初始化、GPU专用地址空间与PAS认证/CP起动入口，所以不是GPU执行。
47组host回归、7写入逐项源码寄存器对照、每lease失败以及保持halt测试通过。
此前thermal+TSENS整合CI41576685181（c0d0a68）已完整成功；后续CP增量待CI。

进一步补`cepheus_cp_protection.h`实际配置：12个64bit地址模式、CP_PROTECT_CNTL
值11、32个span，共45写，始终保持SQE halt。ring配置入口逐项回读验证45项，
不接受仅callback声称“保护已配”。源码对照提取a6xx_enable_64bit的12个实际
寄存器并比较编译后的表；48组host回归通过。仅设置受保护的访问模式，不触碰
TRUST_CONTROL或安全认证状态；GX其余初始化/认证/执行仍未完成。

`cepheus_cp_snapshot_mmio.h`只读取固定66个恢复寄存器，读取前验证完整保护/
地址模式且CP保持halt；输出不得别名到MMIO或适配器对象，失败不写部分快照。
联测复用同一GPU RAM：protection安装→ring/SQE预配置→实际寄存器快照→
136word恢复列表→12word CP_ME_INIT生成，确认始终未写WPTR/未释放halt。
49组host测试通过；ring配置版CI41577933766（48d6199）已成功，新保护/快照
增量仍待完整CI。尚未执行该CP_ME_INIT，不能称GPU命令提交成功。

`cepheus_cp_workspace.c`加入真实XNU内存池CPU侧CP backing：ring32KiB、RPTR、
SQE32KiB（剥首DWORD、32300字节有效payload）、restore与scratch。固件先私有
snapshot+固定SHA校验，再逐字节核对目标；无效固件拒绝、slot不足清理、complete
失败保留句柄。与六缓冲GMU workspace同时占用同一1MiB池仍通过，成功清理全零。
四种本地SQE场景通过ASan/UBSan，公开CI只编译该真实路径，缺固件fixture明确skip。
该CP workspace只有CPU prepare/release，没有GPU VA、SMMU映射、DMA发布或退休
接口；不会误用GMU SID5地址空间。尚需真正GX映射及安全认证后才能连接CP配置。

### 重要纠错：GPU温度传感器归属

重新解析保存FDT的`GPU/tzone-names`和`thermal-sensors`发现，实际GPU热区为
`gpuss-0-usr → TSENS0 sensor15 → 0xc263000/status0xdc`，
`gpuss-1-usr → TSENS1 sensor11 → 0xc265000/status0xcc`。
`kgsl_pwrctrl_temp_show`按这两个热区读取，不能从遗留`qcom,tsens-name`或CPU
传感器编号推断。此前TSENS0:12/13选错，相关host测试只验证算法，不能当GPU温度。
现sampler、结构字段、整链fixture、soc只读日志均修正；FDT测试断言解析结果，
两个控制器都要求ready/lease/valid。所有回归通过，但实际温度仍待真机采样。

未启动的c0d0a68诊断候选含旧GPU温度来源，**不作为GPU热验证候选**；需要从
本次修复后成功CI重新打包。没有手机实际运行错误热保护，GPU启动钩子始终禁用。

FDT报告同时补充GX专属输入：gfx3d_user SID0/mask0x401（匹配0/1/0x400/0x401）、
secure SID2/mask0x400，与GMU kernel SID5严格分离；存在hyp_secure_alloc、
use-3-lvl-tables。原SMMU驱动在AArch64三层模式限制IAS≤39，不应把GMU的32bit
VA假定为完整GPU用户地址空间。GX图形参数HBB15、MAL32、UBWC3、macrotiling
channels缺省，已由独立解析测试记录。尚未用这些字段冒充已实现GX页表/GMEM。

`cepheus_gpu_vm39.h`新增独立稀疏39bit VA/40bit PA三层4K页表构建器，16区、
每区≤256KiB、最多64表页（当前CP工作集限定，不是通用用户VM）。支持>4GiB、
跨L1边界和最高合法页；预检表预算/别名/溢出且拒绝时不改目标。GMU32构建器
保持原样；没有为GX构造CB/TCR/ASID/SMR、安装SMMU或发布CP缓冲。50组host
回归通过，下一步仍需独立GX上下文所有权与完整映射生命周期。

CP CPU workspace增加独立32页root池与`cepheus_cp_workspace_table`：调用者显式
提供已保留的五个GPU VA，构建39bit表，ring RO/NX/unpriv、SQE RO/execute/unpriv、
RPTR/restore/scratch privileged RW/NX（NC）。权限来自kgsl_iommu_get_protflags与
原始分配标志，不沿用GMU固件RW/execute策略。完整校验器分别按32/39bit边界
检查，GMU接口仍固定32bit。全部仅CPU表数据；没有SMMU context配置/DMA发布。
包含CP root后的双工作区可在1MiB池共存，release清空表指针，ASan/UBSan通过。

`cepheus_gx_core.h`补A640v2核心配置子集23写：UCHE trap/write-through/GMEM、
ROQ与内存池阈值、AHB错误响应、HBB15/MAL32/UBWC3等。1MiB GMEM来自本地
原厂内核36f369f的adreno-gpulist A640条目，UCHE base为按1MiB对齐后的size；
未独立实测。`test-gx-source-layout.py`提取实际源码寄存器/literal、GMEM条目
并结合保存FDT计算值，与编译plan比对通过。联测将此子集接入保护/CP快照链。
51组host及扩展适配器ASan/UBSan通过；不是完整GX初始化（HWCG/GBIF/LLCC/
QoS/地址空间/PAS等仍缺），始终保持CP halt。不得将该configured视为GPU ready。

TSENS采样补SROT+4的controller enable及对应sensor enable位，前后均检查，
避免TM valid旧值来自已禁用传感器。两组SROT基址0xc222000/0xc223000，FDT声明
4字节而源码访问+4；此实现明确要求平台拥有完整4KiB映射，不谎称+4在4字节内。
只读、不改变使能位。soc诊断仍只是单次温度样本而非完整热许可；实际热监督
必须走完整sampler+lease。测试包括controller/sensor各自禁用的拒绝。

TSENS24xx另检查TM0x14的watchdog bark(bit31)及所选sensor critical位，前后
均拒绝且不清除中断。必须明确：sampled_ns是CPU观察时间，不是硬件转换计数；
ready/valid无错误也不能独立证明转换器没冻结。真实平台lease须保证采样时钟/
转换健康与独立超时监督，当前仍未具备这个真机证据。

CP workspace完整性接口重取池内view核对句柄/PA/容量，重新SHA256校验剥首DWORD
后的SQE payload（32300B，`ec4c62fc9d3844337d54fc6a04fe01829efff3779fe03bcce96ee87d2d523ce0`）
及32KiB零尾，TABLED时核对各角色固定权限/完整页表。损坏payload/尾字节、假view、
篡改表根均拒绝；构表前强制该检查。CPU视图仍未硬件只读封存，此接口不是抵御
恶意内核的安全边界，也不是PAS认证；DMA发布前还需真正的密封/生命周期绑定。

`test_gpu_keepalive.c`将同一GMU/GPU RAM的OOB请求/ACK/clear接入GX子集→CP保护→
halted ring配置；只有OOB_HELD租约才准许GX配置，未drain不能释放，release后
租约立即拒绝。52组host与ASan通过。不声称OOB release使GX断电；后续仍必须
slumber/park，也没有在该链路中执行CP或认证ZAP。

独立`cepheus_gx_smmu_plan.h`只生成单一GX上下文的纯计划：SID0/mask0x401、
T0SZ25的39bit VA、40bit PA、NC walk、ACTLR0x303；严格排除foreign CB/stream，
保留GMU与secure流，GMU安装器明确拒绝该计划。53组host通过。没有GX installer/
DMA publication；单上下文只是最小受控试验候选，不代表可忽略secure CB分配或
CP SMMU aperture的SCM初始化要求。多进程页表切换、fault handler仍未实现。

CP workspace restore接口现把固定MMIO快照写入其实际池内restore buffer，并以
该已构表VA生成CP_ME_INIT。前后复检SQE/页表/角色身份，拒绝packet输出别名到
任何backing、workspace、adapter或GPU窗口；失败保持输出不变，禁止重复准备。
真实C模块+真实C++池联测通过ASan/UBSan。数据仍CPU-owned，不发布/不执行；
需要后续实际GX上下文/DMA生命周期才能使用这个包。

SMMU geometry现在明确保留ID2的VA(UBS)、IPA(IAS)、PA(OAS)三种宽度；不能只
检查输出PA40就接受GX39输入。GMU需要VA≥32/IPA≥40/PA≥40，GX纯规划另需
VA≥39。新增VA32/IPA36拒绝负例；GX mock ID2改为实际表示40VA的0x1222，
而不是原来仅32VA的0x1022。真实ID仍需允许的只读采样确认。

Clang静态分析覆盖6个生产C单元及真实内存池C++（host IOKit替身）；发现BW
返回值仅与signed count比较的隐含条件，现先显式拒绝非正/大于8的编码返回，
再比较数量，不用初始化wait为0掩盖错误路径。畸形UINT_MAX count保持输出不变
测试通过。`scripts/analyze-gpu-host.py`把任何分析warning/error作为失败，
修正后7单元无告警；静态分析仍不证明内核ABI、竞态或硬件安全。

GX寄存器访问预检新增实际SPTP power-off/clock-off位(bit2/4)以及GX bit6/7，
并要求AO_AHB_FENCE_CTRL为ALLOW0。CP保护/核心子集/快照/ring配置与GX故障读取
均复用它；任何阻塞均拒绝，不强制打开fence或唤醒GX。已有外部OOB/所有权仍
不可省略；此预检不是新的资源租约。

HFI补齐索引边界的显式arm64屏障：读取peer index后再读取payload，复制完
payload后才发布read_index归还槽位；发送payload发布后、write_index之前也执行
完整barrier。原cache callback在读index之前的屏障不能替代这些顺序。新增空
输出/未对齐索引拒绝。host测试不能证明ARM总线行为，实际NC共享仍待真机验收。

`scripts/validate-gpu-offline.py`串行运行离线检查并保存每项日志/exit code、git
commit/dirty状态、显式固件/FDT摘要；缺reference或专有fixture记SKIP，输出目录
已存在则拒绝覆盖。完整参考输入的一次运行17项PASS记录在本地
`logs/gpu-offline-20261103-1420/receipt.json`（当时脚本未提交所以dirty=true）。
该receipt不包含任何真机执行证明，也不上传专有文件。PAS前置详见
`docs/zap-platform-contract.md`，仍无XNU认证后端。

`check-gpu-diagnostic-log.py`只分析已保存日志，输出OBSERVED/MISSING/CONFLICT/
FAILED及摘要；完整只读材料也固定GPU_ACCELERATION=UNVERIFIED、ARC所有权UNKNOWN。
缺项退出2、拒绝退出1，不把旧preflight缺少后续功能日志判成已验收。原d336276
真机日志正确返回INCOMPLETE（触摸/主屏幕的既有用户验收不被否定）。日志工具
无法认证来源，检测到RAM/host fixture则拒绝；不能替代用户交互/长期稳定性测试。

原厂gmu_start在HFI完成后经kgsl_pwrctrl_set_default_gpu_pwrlevel请求默认DCVS，
不是只发PERF/BW表。现整合路径补4word id30消息（ACK_BLOCK=1,freq1,bw3），
固定本机最低257MHz初始档，不接受off/越界/任意升频。失败于DCVS隔离，成功
只记录ACK，不宣称频率实测。20个本地固件场景、ABI对照、静态分析通过。
同时发现冷启动前原厂持有默认档DDR bootstrap vote；目前此真实带宽租约
仍未实现，BW_TBL不替代它。它与ARC所有权一样是禁止真机启用的明确前置。

**纠错（同一处）**：上面初版把id30的bw写成带宽档3是错的。KGSL冷启动链是
`set_default_gpu_pwrlevel(6) -> kgsl_clk_set_rate -> gmu_core_dcvs_set(6,0xFF)`，
gmu_dcvs_set只在`bus_level < num_bwlevels && bus_level > 0`时才写bw，0xFF被
跳过并原样上线。所以启动投票是freq=1（=8-6-1，FDT bin0 initial-pwrlevel=6）、
**bw=0xFF哨兵**。带宽档3属于slumber：a6xx_gmu_notify_slumber取
`pwrlevels[default].bus_freq=3`与`perf_idx=1`。两者语义不同，混用会向固件
声明一个它从未进入的带宽档。现两条路径分别固定，退休时强制等于实际启动值。
字段顺序亦按源码核对：id30为(hdr,ack,freq,bw)，id33为(hdr,bw,freq)。
本地21个固件场景、ABI逐字节对照通过。

## 当前结论

目标是Mi 9/SM8150上的iOS16.7.16图形加速。**尚不能证明“理论完全可行”，
更没有完成硬件加速。** 原因不是缺少状态机，而是仍有明确未实现的平台后端、
安全固件加载、共享内存同步和用户态ABI。本文把可实现的路径及其条件具体化，
不以空回调或ready布尔值代替这些条件。

用户已验收d336276镜像主屏幕、触摸、电源键正常；只读GPU探测确认CX实际off。
USB自动重启功能已取消，不恢复。用户离开电脑后不重启手机，不做电源试写；
最终统一验收时才逐阶段启用硬件实验。

## 证据与分层状态

| 层 | 现有产物 | 当前证据 | 下一道验收门槛 |
|---|---|---|---|
| 保底系统 | GPIO触摸/PON/软件显示 | 用户实测通过 | 每个新硬件阶段不得破坏 |
| GPU域观测 | gpu_power + soc probe | CX_HW=0x5ff4，blockers=0x7a/0x37a | 保留完整原始日志 |
| ARC资源发现 | arc parser + read-only probe | 解析负例/完整XNU构建通过 | 实测cx/mx/gfx表与继承槽 |
| 电源时钟 | gpu_power_txn | 每回调故障注入通过，只有mock | 当前DRV投票/资源归属、真实ACK、频率实测 |
| 内存映射 | gpu_vm + smmu_plan | 属性/越界/冲突离线通过 | allocator/缓存/TBU/CB/ASID归属，实际fault检查 |
| GMU启动 | gmu_firmware + gmu_loader | 本地固件7块解析、mock每阶段失败 | PDC/RSC序列、映射、CM3新鲜BABEFACE |
| HFI | ring/table/消息/会话/boot | 真实ring模拟固件握手、线格式对照通过 | A640版本/PERF/BW/功能/CORE_START真机协商 |
| GPU CP | PM4/fenced submit | 有界scratch包与超时mock通过 | GX/SQE/ZAP/non-secure、实际scratch/fence |
| 离屏绘制 | 尚无实现 | 无 | 有限格式clear/blit/三角形及hash对照 |
| 显示加速 | 现有SweetSurface/Framebuffer软件路径 | 源码与此前实测 | GPU buffer导入、fence同步、软件回退 |
| Metal/IOAccelerator | 尚无Adreno实现 | 无 | 用户态实现/ABI/着色器与资源同步完整接入 |

## 1. 电源/时钟后端不能省略的工作

- ARC表为u16 raw VLVL，数据字段投的是索引。GMU200MHz对应最低raw48。
  CX父电源依赖MX；提升先MX后CX，保留比最低需求更高的原有投票。
- `rpmh_read()`在Android通过TCS发read请求；返回到底表示该DRV投票还是
  聚合状态，不能仅凭函数名推断。需要设备/固件接口证据。
- 同一apps DRV的completed vote可以在TCS释放后留存；不允许“选空TCS→写48”
  覆盖其他用途。若无法恢复所有者模型，应停在诊断，不做降低或猜测性写入。
- 冷启动要求CX/GX都off、HLOS软件控制、GMU halted；GDSC配置REST2/FEW8/
  CLK_DIS_WAIT8后clear collapse，轮询GPUCC0x1540，不只看0x106c。
- GPLL0-div需真实300MHz及父时钟租约，RCG0x1124选择6/div1.5，UPDATE完成后
  才开分支。该RCG启用safe_config，按照clk_rcg2_enable在切换期间force-enable
  root、等待root on，再改配置/UPDATE并撤销强制位；新旧父时钟须已持有。
  复用当前代码的命名寄存器接口，不能开放任意MMIO。
- power_txn故障处理只是“保留而不降票”，不是热安全方案。实际部署还需要
  温度监测、有限会话和可证明的停止路径，否则不允许长期带电试验。

## 2. GMU/SMMU的条件

固件来自本地合法设备的a640_gmu.bin，不提交专有固件。SHA固定在cpu-gpu.md。
ITCM/DTCM经AHB上传；ICACHE/DCACHE及HFI/log通过GMU虚拟地址映射。TCM窗口由
GPU基址word offset换算，不能误把GMU物理基址再叠加GPU绝对offset。

GMU SID4/5带0x400 mask。SMR冲突检查只解决地址选择，不解决安全所有权；
必须核对ID寄存器、TBU连通、继承的CB/ACTLR、安全CB保留及ASID分配。
安装顺序：GMU停在reset→stream FAULT→表clean→配置CB→限定ASID/TLB同步→
寄存器核对→stream TRANS。超时不能全局清SMMU，也不能先释放表。
当前planner未执行这些操作。现有映射API只接受已知连续PA区，真实离散页表
还需要由受控IOMemoryDescriptor/IODMACommand分段枚举，而非kernel VA强转PA。

PDC/RSC微序列来自对应SM8150修订、实际cmd-db地址和供电依赖，不能直接把
Android裸常量搬入未验证环境。释放CM3 reset之前要清旧init结果、设置chipid、
HFI/log地址、AHB fence与power config，并重新确认固件/数据cache发布完成。
BABEFACE只表明CM3初始化，不表明HFI/硬件GPU可用。

HFI v2队列还必须管理固件版本、单一producer、超时后不重用seq、错误队列和
CM3 fault。A640的源码明确跳过legacy INIT；版本/PERF/BW/FEATURE/CORE_START
消息依赖真实频率、电压和互连表，不能用空表骗过握手。共享queue header包含双方字段，应使用coherent或匹配的uncached映射；
不能在有对方dirty数据的同一cache line上做粗暴invalidate。

## 3. CP/SQE/ZAP与最小GPU执行

Android `adreno_a6xx.c:a6xx_microcode_load`把SQE地址写入CP，然后按secure能力
通过PIL加载a640_zap；ring启动后调用adreno_set_unsecured_mode。
这一安全世界路径尚未在XNU移植/测试。**不要通过关闭安全检查、禁用保护寄存器
或未经证实的直接unsecure写法绕过它。** UEFI有SCM协议源码不等于ExitBootServices
后XNU可调用，也不等于已获得PIL/PAS内存与认证接口。

第一个命令仅使用内核自建scratch PM4，不接受应用原始命令：
CP_MEM_WRITE→WAIT_FOR_IDLE→CP_EVENT_WRITE/CACHE_FLUSH_TS完成戳。
验收必须同时看到期望scratch、独立fence、新鲜序列、无SMMU/CP错误，以及
bounded quiescence。fence可见但GPU未停稳时不可unmap/free。单次提交后不做
自动重试，故障后保留内存并报告具体阶段。

下一步才做固定RGBA/BGRA8离屏clear/blit、再到三角形；必须有CPU参考结果、
边界guard页、缓存一致性和正常/超时释放测试，再接显示。

## 4. iOS接入：先有限显示加速，再讨论Metal

本机源码`SweetSurface.h/.cpp`：surface持有IOMemoryDescriptor，普通surface为
IOBufferMemoryDescriptor，但不承诺物理连续。`SweetFramebuffer.cpp:present`
按层查surface，逐行CPU memcpy到ABL连续splash framebuffer。

较小的落地路径是在**实际GPU blit已通过**后为Framebuffer提供可选GPU blit：
持有源surface引用→固定权限映射→缓存发布→提交→fence→完成后展示/释放。
跨帧引用、CPU/用户映射修改、熄屏并发、超时与软件回退必须明确。绝不能GPU
仍在写时CPU同时执行fallback，也不能fence未完成就把surface释放/回收到其他进程。
该路径最多减少拷贝开销，不会使CoreAnimation的所有绘制或应用Metal自动加速。

完整Metal还缺：
- 适用于iOS16的MTLDevice发现和用户态驱动装载方式；仅注册名为IOAccelerator
  的服务无法证明Apple闭源驱动可用，不能向系统谎报硬件能力。
- resource/heap/texture/pipeline/command queue/fence等用户态对象与内核提交ABI；
  每进程内存隔离、命令验证、引用生命周期、错误上报和并发调度。
- Metal着色器输入格式到Adreno ISA的编译链与验证，pipeline/renderpass、
  tiling/GMEM、纹理格式等实现。Mesa/Turnip可参考，但依赖的Linux内核接口
  要替换；MoltenVK方向是Vulkan→Metal，不能创造缺失的Metal后端。

当前仅依据本机源码确认上述接缝，没有检查闭源用户态ABI、做替换驱动或声称
能无修改运行Apple AGX驱动。这是“完整可行性”仍未解决的独立研究/实现任务。

## 统一验收顺序与交付定义

1. 原系统回归（已经通过d336276），新ARC镜像仅补采表与投票证据。
2. 供电/时钟：实际readback/计数、正常停止、故障不降他人投票、温控。
3. GMU：新鲜固件启动结果、正确版本HFI、日志和错误队列。
4. SMMU/CP：专用scratch+独立fence，成功/错误/超时后内存无误释放。
5. 离屏图像逐像素对照，再进入可回退的Framebuffer blit。
6. Metal是独立后续里程碑；必须应用真实调用+渲染/同步测试才能宣称支持。

离线回归命令：`bash scripts/test-gpu-stack.sh`（目前38个程序、UBSan）。
补丁顺序：`python3 scripts/test-gpu-arc.py /path/to/pinned/ios-sweet/xnu/patches`。
mock日志标注“mock/no backend/not installed”，任何失败不得标完成。

## 本次离线交付记录

代码提交：58a944c修复ARC staging；dc8dab6电源事务；b51f525映射/提交契约。
CI41396451358（b51f525）通过14组C/UBSan、patch顺序测试及完整XNU构建。
产出内核SHA256：
`4fee603741f3a38874a749528a31c2af065e086061922ee5f2953e3885d0f7d4`。

只读ARC候选镜像：`boot-cepheus-gpu-arc-b51f525.img`，SHA256：
`0ba44c063c4e25d1316eac5a5ff39c1e3b5e4b37a4f809a8036253944f8d0da3`。
仅在已有无网络构建容器打包，保留原ADT/UEFI/DTB及触摸电源设置。
**尚未启动该候选；用户不在场期间没有设备操作。** 电源事务/SMMU/提交模块
仍为host-only测试，不在公共内核中执行；内核新接入只有ARC只读诊断。

## 后续续作：HFI端到端与具体固件写入

- `cepheus_hfi_messages.h`输出固定59-DWORD PERF表、160-DWORD BW表、17-DWORD
  ACD表及FW_VER/FEATURE/CORE_START消息。频率Hz转kHz；ARC索引根据供应的真实
  表选择，缺MX依赖/错序OPP/坏BCM地址/越界wait mask拒绝，未使用的尾部清零。
- `test-hfi-source-layout.py /path/to/kgsl_hfi.h`读取本机Android结构定义到临时
  编译fixture，实际sizeof/offsetof和完整字节比较通过，避免测试仅重复自己的
  线格式假设。未把Android专有设备输入或固件加入仓库。
- `cepheus_hfi_session.h`直接调用共享ring write/read，等待正确req_hdr ACK；
  一次只允许一个未完成请求，4095序列用尽后不回绕，100ms/10000次双上限。
  错误/不匹配/未知异步请求标记session与queue不可重试。ACK外层ID不强求等于
  命令ID（KGSL匹配依据是回显req_hdr），避免虚构ABI要求。
- `cepheus_hfi_boot.h`按A640顺序进行FW_VER→PERF→BW→ECP disable→可选ACD
  配置→按已验证bin选择LM→CORE_START；要求调用者给出有效bin策略和对应
  throttle mask。模拟固件从真实ring取命令再回ACK，覆盖每条命令失败、队列
  wrap/full、陈旧ACK、版本错误、异步错误、时钟倒退/冻结与超时。
- `cepheus_gmu_upload.h`具体执行ITCM/DTCM的LE32 stores和ICACHE/DCACHE映射
  拷贝，而非write_block成功桩。先验证全部目标、别名、计划/固件重叠和映射
  范围，再在独占reset lease内写入并完成cache/MMIO发布。对本地a640_gmu.bin
  实际7块内容在RAM模拟TCM/缓存窗口复制后逐字节比对通过。没有映射真机MMIO。

上述仍需平台提供已验证的固件摘要/不可变缓冲、真实映射、reset/power租约和
cache屏障后才能连到硬件；模拟成功不代替实际GMU启动。命令输入由内核构建，
不作为应用任意PM4或任意电源投票接口。

## XNU集成续作（54e8951构建通过，未真机执行）

`cepheus_smmu_install.h`现已实现限定context安装顺序：验证预留资源→发布全部
表→配置CBA2R/CBAR/TCR/TTBR/MAIR/ACTLR→限定ASID失效与有界TLBSYNC→先SCTLR，
再SMR，最后S2CR由FAULT转TRANS。任何实际写入后失败均隔离，绝不全局reset。
ACTLR不是随意填0，begin后端必须验证该平台策略；当前测试使用假寄存器。

`cepheus_gpu_compile.c`把HFI boot、实际上传、SMMU install、电源事务和提交
函数纳入真实XNU C编译/链接，后续CI核对所有源码以及符号。没有启动调用，
不会因这些符号存在就执行供电或DMA。

`cepheus_gpu_memory.cpp`接入真实IOKit内存API，并提供默认关闭的`cephmem=1`
CPU-only自检：分配20KiB、显式prepare、MapperNone读取host PA并验证连续性，
写入/读取pattern后complete和release。不使用kernel VA强转PA。40bit PA mask
与4KiB对齐，单次<=256KiB、总预算后续扩至1MiB。选择WriteCombine/Normal-NC，而非
InhibitCache/Device DRAM；后者可能触发XNU managed-page保护或错误访问语义。
核对了pmap的缓存属性更新路径，但该自检尚未真机执行，不能证明缓存行为。
已发布给设备的buffer没有bool式“done”释放捷径，仍需真实停止/解除映射实现。

CI41456898966首次链接失败因0087缺少尾部patch上下文，被git按EOF语义拒绝；
54e8951加入SweetBattery后文并在实际XNU配置副本apply验证。
CI41458264813完整通过18组C/UBSan、集成/patch测试与六入口链接检查，下载
内核SHA256 `757d6d5fa85650fddb94e9ddb0596ec6a9039639978934d518c481ecf63b5690`。
这证明真实XNU API/编译链接可用，不证明设备内存属性、DMA、供电或提交已验收。
RpmhDxe进一步证据与限制见[rpmh-firmware-evidence.md](rpmh-firmware-evidence.md)。

## 实际FDT配置，不沿用通用v1参数

`scripts/gpu-fdt-config.py`直接解析保存的DTB（不需要运行dtc），解析边界、节点、
phandle、实际OPP引用、speed-bin表、BW路径、GMU/SMMU和ACD配置。记录输入SHA，
默认不选择speed bin；有eFuse/内核证据后才能显式选，不会默认使用最快档。

本机live FDT SHA `bdd364a16ceabe9960e128103162e5280b150303624a123dfd71e871f9a43274`
解析结果：GPU引用`/soc/gpu_opp_table_v2`而不是`/soc/gpu-opp-table`；
非零GPU OPP为257/345/427/499.2/585/675/770MHz，raw VLVL为64/128/192/224/
256/320/320。speed-bin0/1各自throttle mask为0x80/0x40，当前实际bin未知。
GMU仍为200MHz。PDC config/seq是`0x0b290000/0x0b490000`（各64KiB），
而非通用sm8150.dtsi的v1地址。SMMU ACTLR条目`0,0x407,0x303`匹配SID4/5及
mask0x400，因此GMU context的期望ACTLR为0x303，不应沿用测试中的0x10。
源头分析依据`arm_smmu_fwspec_match_smr/qsmmuv500_init_cb`，并非真机寄存器读数。

验证命令：`CEPHEUS_LIVE_FDT=/path/live.dtb python3 scripts/test-gpu-fdt.py`。
这只导出配置；BCM的cmd-db unit/width/VCD和当前电源投票仍需要相应证据。

### CM3启动与BCM续作

1d81a7b实现具名GMU寄存器启动序列：仅chipid0x06040001、无重叠HFI/log区间，
在已持有power/reset/SMMU/PDC/image前提下清旧init、配置启动地址/chipid/fence/
IFPC、解除CM3 reset，等待新BABEFACE及HFI status，再校验HFI v2。每次操作
前校验租约与fault、双时间/次数上限，失败隔离不重启。CI41461702642完整通过，
仅证明七入口编译/链接及19组host测试，不代表已在真机解除reset。

FDT脚本进一步解析真实有向互连路径：DDR为GPU→LLCC→MC→EBI，BCM依次SH0/
MC0/ACV；CNOC为GPU→GEMNOC-SNOC→GEMNOC→CNOC，BCM为SH2/SN15/SN3。
`cepheus_bcm.h`依据KGSL实现unit/width/VCD元数据、AB/IB归一化、稳定VCD排序、
每组末项commit和对应command-index wait位（不照抄上游按usecase索引位的疑点）。
值饱和到14bit，零流量仍可携带commit但valid=0。真实cmd-db元数据只读探测新增
上述六资源，不写票；ACV是RSC相关bitmask资源，不能套普通BW归一化。还没有
声称这些输入足以生成最终HFI BW表：需完成ACV身份/聚合策略及实际cmd-db采样。

### 有界scratch ring与SQE装载格式

`cepheus_gpu_ring.h`按KGSL32KiB ring实现空间预留、尾部NOP wrap和publish→WPTR
顺序。它只接受内核生成的10 DWORD scratch+fence包，不接受任意用户PM4；
检查scratch与ring VA不重叠、容量、read/write指针及已有inflight状态。发布失败
之后不回滚写指针或释放内存。成功只表示入队，仍须fence+停稳验证，当前是一发
测试路径，未提供清除inflight来绕过回收门槛。

`cepheus_sqe.h`对应`_load_firmware/adreno_get_ucode_version`处理SQE第一DWORD
剥离与版本编码；实际a630_sqe.fw（原记录SHA未变）解析为version0x16ee190、
8075 DWORD、32300字节payload，RAM拷贝逐字节校验通过。没有启用SQE或绕过ZAP。

### XNU内部内存池

内存模块扩展为16槽/1MiB总预算的真实IOKit池：64-bit句柄含generation和
slot，旧句柄不能释放重用后的分配；借出的CPU view阻止publish/free；CPU-only
buffer可以配对complete/release；publish之后没有取消发布/直接释放接口。
分配与complete操作在mutex外执行，槽位保持保留；完成失败保留原descriptor
与预算供未来恢复，不遗失到临时栈变量。generation用尽则退休槽位而不回绕。
API仅为内核内部用途，未建立IOUserClient或允许用户控制物理地址。
单独生命周期测试覆盖lease/代次/发布/不确定完成路径，真实IOKit调用仍需完整
XNU构建与后续cephmem自检；不把CPU pattern成功等同于DMA一致性验证。

### 单GPU客户端带宽表生成

`cepheus_gpu_bw.h`现可从明确路由节点宽度/ports、每个BCM的unit/VCD及单客户端
AB/IB构建HFI BW输入；同一BCM覆盖多个节点时取最大归一化值，按VCD稳定排序，
所有usecase的地址顺序/wait保持一致。FDT已确认路径BCM指向apps RSC(cell-id8000)，
故ACV只使用该RSC定义的bit3（值8），不是普通IB数值，也不假装它是GMU自己的
硬件DRV投票。该表来源对应KGSL查询apps interconnect route的路径。
仍缺真实cmd-db unit/VCD采样以及平台端聚合/所有权接入；只对合成元数据执行，
没有发电源/带宽请求。多客户端聚合、特殊ALC/其他RSC不在此API支持范围内。

### 固件身份与故障快照

XNU GMU upload和SQE copy入口现在先用corecrypto SHA256验证固定本地artifact
大小/摘要（host用OpenSSL/CommonCrypto）。两个真实文件通过，单bit破坏拒绝。
这是文件身份校验，不是Qualcomm PAS签名认证；输入仍须在hash至使用期间保持
不可变，不能把可被用户修改的共享buffer直接传入。

故障快照接口依据后端当前可安全访问的域分别读CP/GMU/SMMU；没有GX访问租约
就不读GX，未读域不能记成clean。保留CP fault/protect、SMMU FSR/FSYNR/FAR及
时间戳，FSR前后改变或MULTI时不声称FAR原子一致。不做IRQ W1C/clear/reset，
避免破坏第一现场或误清其他客户端错误。该接口仍是具名寄存器后端，不对应用开放。

固件生命周期进一步使用真实XNU私有快照：copy之后对私有内存计算固定digest，
sealed状态不再允许获取可写view，也不允许直接publish为GPU DMA目标；只读lease
在实际GMU上传/SQE复制整个期间阻止free。内部调用者必须传入可读固定的内核源，
不是裸用户指针；seal是可信内核API约束，不声称能防恶意内核代码直接改指针。
快照不执行文件系统读取、不自动加载专有固件、不暴露用户态方法。

`test-gpu-memory-host.sh`把**同一份生产C++文件**与隔离的IOKit测试替身编译，
不是重复实现分配器；验证8线程/800次分配与view释放、预算满额、失败prepare、
短物理段、旧句柄、封存后禁写/持读禁free、真实固件私有拷贝及complete失败保留。
物理地址和IOKit操作是测试替身，不作为真机证据。arm64使用真实dsb屏障；
非arm主机屏障仅在显式HOST_TEST且非KERNEL时可编译，不能进入XNU构建。

内存池按实际`page_size`计费：SWEET为16KiB，GPU表仍为4KiB。20KiB HFI请求
实际分配32KiB；4KiB请求实际占16KiB，均计入1MiB预算。CPU view返回实际
分配容量，GPU只映射明确需要的4KiB区段。host测试刻意模拟16KiB主机页，
并验证槽位用尽、页取整和固件valid-length不随allocation-size混淆。

GMU VA计划连接allocator与页表：ICACHE@0x4000、DCACHE@0x44000各240KiB；
HFI@0x60000000映射20KiB，log@0x60100000映射4KiB。host按16KiB取整的多余
页面不进入GPU VA；目标PA必须来自明确owned backing、不能重叠。全部Normal-NC，
设备页表权限后来按KGSL原映射修正：ICACHE/DCACHE/log/persist privileged RW、
HFI unprivileged RW；五区均未设置device XN。详见下方权限源码核对，CPU权限未改。
初版512KiB预算不足以同时容纳480KiB缓存、表/HFI/log/固件快照，已提升到有界1MiB，
保留单次256KiB限制及完整预算测试；不是无限增长的分配器。

`cepheus_gmu_workspace.c`已将真实分配接口→独占CPU views→GMU VA计划→五页三级
页表→HFI队列表连接起来。工作区持有5个分配（ICACHE/DCACHE/HFI/log/tables），
任何中间失败按逆序释放CPU资源；complete失败保留句柄和descriptor，不丢弃
恢复信息。`cephmem=2`是默认关闭的CPU-only整体工作区自检，不写SMMU、不发
电源请求、不发布GPU DMA。host测试实际调用该C文件和真实内存池C++实现，
覆盖准备成功、每个槽位不足位置、物理段不完整、部分清理失败及重入拒绝。

### PDC/RSC冷启动脚本

`cepheus_gpu_pdc.h`固定A640-v2的48项配置/微序列写入，word-offset转换为每个
窗口的byte offset，要求实际PDC地址为0xb290000/0xb490000。三个ePCB资源地址
0x30010/0x30000/0x30090按KGSL及Linux v6.12同源序列保留，但不虚构其名称；
begin后端必须将其与本次cmd-db、所需索引含义、GPU域归属和停止状态核对。
冷启动拒绝已启用的PDC/TCS1/TCS3；先写完整payload，最后才enable banks/PDC。
测试遍历48写入和全部回调失败，失败后不关共享电源、不释放潜在设备资源。
该模块没有apps-DRV投票操作，也不能解决apps投票继承；当前只在假寄存器执行。

### A640v2 bin策略修正

`adreno_a6xx.c:a6xx_efuse_power_features`对speed_bin0清除LM控制位，
`kgsl_gmu.c:gmu_aop_mailbox_init`对A640v2/bin0直接返回，不启用ACD。
因此HFI boot不再统一发送LM enable：bin0为8档（含off）、throttle0x80、
无ACD/LM；bin1为7档、throttle0x40，需要ACD且启用LM。策略解码仅接受
chipid0x06040001和明确QFPROM读数(mask0xe0000000>>29)得到的0/1，其他拒绝。
FDT仅提供可选bin，不能凭表存在推断本机是0或1；当前尚未读本机speed-bin。
两种握手命令序列均有测试。独立TSENS/运行时限等热安全仍是平台要求，不能
把原驱动关闭某硬件特性的bin策略误解为无需温控。

`cepheus_gpu_thermal.h`给试验后端提供不可静默恢复的热门槛：gpuss0/1均须
有效且样本不老于100ms，任一>=60°C、时钟倒退或最多30秒试验期限到达时拒绝
继续。停止不等于断电，真实后端还须安全停稳；没有把这个纯逻辑guard宣称为
已运行的GPU温控。平台必须把真实TSENS采样接入，不得传恒定温度伪造就绪。

### ZAP容器/认证输入核对

REA open_binary对本地a640_zap.elf返回unsupported ELF architecture（ELF标识为
Qualcomm DSP6），未反编译或执行。使用readelf和受测解析器
`inspect-zap-layout.py`只核对容器：ELF32 LE、3个program headers；148字节
header片段、6712字节hash段、1968字节relocatable LOAD。三个.b00/.b01/.b02
均与ELF对应片段逐字节一致，MDT恰为header+hash（6860字节）。
按本地PIL源码：逻辑mem_setup区域4KiB，实际分配按p_align策略为1MiB；
入口在重定位区域offset0。PAS ID由FDT确定为13。这不代表签名通过，也不代表
可复用普通GMU池作为secure memory。需要独立的可信SCM/PAS、缓存/保护域与
认证后生命周期实现；不使用no_auth或直接写trust-control作替代。

### CP初始化/恢复列表

`cepheus_cp_init.h`生成A640的12-DWORD CP_ME_INIT包（mask0x16f、64-bit
reglist地址），不混入secure/protected-mode关闭命令。恢复列表从当前已配置的
GX寄存器读取：38个IFPC项+28个preemption项，带16字节cpu_gpu_lock头；
要求保护控制bits0/1/3启用及所有64-bit address modes就绪，不用零值填假列表。
每次读取前后需持有后端租约，不满足则保持输出未修改。
`test-cp-source-layout.py`与本机KGSL列表宏解析、lock大小/字段偏移和ordinal mask
独立对照通过；实际CP初始化/保护表配置及GX访问后端仍待平台验证。

### Context撤销顺序

`cepheus_smmu_detach.h`先要求真实后端关闭提交入口并停稳producer/总线，再
验证整套context身份→stream FAULT→CB disable→限定ASID失效/TLBSYNC→SMR失效。
任何读取/写入/同步失败或context被外部修改都不发成功回执，不释放内存；
最终FSR非0也保持隔离。实现没有把“切FAULT”当DMA停止证明，没有全局TLBI，
也没添加以裸bool解除published保护的allocator API。真实park后端和最后的
可信退休连接仍需完成，当前只有假寄存器的逐调用失败验证。

`cepheus_gmu_park.h`进一步实现狭义GMU park：前置必须已由后端完成HFI slumber
且GX power/clock均off；排空4个GPU-RSC TCS，再assert CM3 reset、置GBIF arb
halt并等待ACK，最后重查GX状态。任一超时不发parked成功回执，不自动清halt/
降rail/复位全GPU。它拒绝正在运行的GX，不能当通用GPU挂起/错误恢复；实际
HFI slumber和设备门槛仍由后端完成，当前仅假寄存器测试。

slumber部分现有直接HFI实现：命令ID33、payload顺序bw/freq，先由后端关闭新
任务并排空队列，再关闭power counter、发送PREPARE_SLUMBER并核对ACK，随后
轮询真实RPMh状态15和GX power/clock off。ACK成功但状态不变会超时，不会
继续park/unmap。仅支持GMU拥有IFPC/SPTP的路径，不替代host管理SPTP的流程；
不支持的所有权状态必须拒绝。源结构memcmp与真实ring模拟响应测试均覆盖。

### 联合工作区/固件测试

同一host测试进程已连接实际固件→私有sealed快照→真实分配池接口→GMU workspace
页表/HFI→RAM模拟TCM与ICACHE/DCACHE上传→完整CPU清理。逐块比较7个固件区段，
并将缓存VA经生成页表解析为分配器返回的PA以交叉核对；不是分别测试后假设
模块可以拼接。实际IOKit分配器由host替身提供、TCM为RAM，仍不代表真机GPU执行。

组合边界检查新增：VM输入region数组不得与即将清零的页表池重叠；HFI的命令
输入/返回输出不得与共享ring数据区别名，防止边复制边改写尚未消费的数据。
测试明确覆盖拒绝后索引/页表不变，而不是仅依赖调用者避免此类错误。

### 待验收只读采样补齐

默认关闭的`cephfuse=1`只映射QFPROM目标页0x784000，并两次读0x130的GPU
speed-bin字段，不包含任何fuse写入；未知/不稳定值拒绝，且不触发GPU上电。
`cephsoc`日志补gpuss0/1独立样本（修正后的TSENS0:15/TSENS1:11），仍不写TSENS配置。
ARC只读探测新增按0x30010/0x30000/0x30090反查本次cmd-db名称和level表，
解决PDC常量不能猜资源名的问题；重复地址/非ASCII名称/格式错误均拒绝。
这些采样尚未在手机执行；用户离线期间不启动新镜像。

CP恢复列表核对加强：除保护控制位外，还逐项验证32个有序保护span与KGSL
选择的A640表完全一致（重叠span保留原顺序，最后span保护到寄存器空间末端）。
`test-cp-source-layout.py`独立计算源span编码对照通过。仅编码/验证保护，不
通过关闭CP保护来让初始化或命令测试“成功”。

bin1 ACD还需要AOP通知。`cepheus_qmp_acd.h`只允许固定32字节GPU ACD文本，
在已经连接且独占的mailbox上按payload→length→readback/屏障→通知顺序发送，
等待length清零消费确认；busy/租约改变/1秒或次数超限均拒绝并保留不确定状态。
没有任意消息接口，也不抢占或重连其他客户端通道。尚缺真实QMP连接/所有权
后端；消费确认也不等于应用层ACD已成功，须结合HFI feature状态验收。

### 输入内存错误检测

`test-gpu-sanitizers.sh`对14个解析/映射/消息测试程序启用AddressSanitizer+
UBSan。新增固定seed的20000组GMU/ARC/BCM/HFI畸形输入，回调对每个cmd-db
读取地址做越界/对齐断言，失败不得返回残留记录数。当前检测通过，仅为有界
负面覆盖，不声称穷尽fuzz或替代真实设备/并发验收。

## 独立审查修复（memory/VM/workspace限定范围）

GPT-5.5只读审查8bcaf2e0针对26c528a报告：
1. CPU view被workspace长期持有，原publish要求views=0导致发布不可达。
2. 分配cache选项并不足以证明getBytesNoCopy返回的CPU VA为NC；dsb不清cache。
3. 部分清理失败时table指针可能已悬空，隔离槽位需可追踪。

修复：
- 显式`createMappingInTask(kernel_task,0,kIOMapAnywhere|kIOMapUnique|kIOMapWriteCombineCache)`，
  核对mapping长度/cache选项，**仅使用mapping VA**，从不使用原始getBytesNoCopy VA。
  由XNU映射路径处理缓存属性转换；host mock给mapping分配与原buffer不同地址，
  并断言raw_views=0。没有把getMapOptions当作真机PTE/总线一致性实测。
- 五buffer原子共享保留各自唯一CPU view，状态转SHARED且普通get/put/free拒绝；
  工作区用已有NC视图按HFI双向字段所有权和barrier协议访问。固件sealed不参与共享。
- 公布的group记录保存generation句柄集合和SMMU root/slot/CB/ASID身份；退休
  调用实际slumber→park→detach引擎，各步成功后才解除published并complete/free。
  失败全组隔离，重复/伪造group、root、句柄或状态不匹配拒绝。私有finish bridge
  仅为可信内核C/C++协作，不作为对恶意内核的隔离边界，也不对用户态开放。
- 每次清理都使table/layout失效；部分失败保留handles供诊断。complete失败仍
  隔离而不是强制free，日志记录slot/bytes/generation。ALLOCATING排他规则加注释。

生产C/C++联合测试现在覆盖prepare→批量share→仍通过CPU视图进行HFI交换→
slumber/park/detach→释放；并分别注入四个阶段失败，断言五分配/映射全部保留。
这些是模拟寄存器/IOKit测试，尚不能证明真实平台租约、缓存转换或停稳；
不得启用硬件后端或将审查修复误报为GPU加速完成。

进一步的kernel-only门槛逐个host page查询`pmap_cache_attributes`，必须实际为
VM_WIMG_WCOMB才返回CPU view；若mapping选项与pmap有效属性不一致，清理分配
并拒绝使用，不自动强改物理页属性。该分支不在host替身中模拟为成功，需真实
XNU构建及后续CPU-only真机自检确认，避免只依赖getMapOptions的请求值。

SMMU几何解码器根据实际ID0/1/2生成bank数量、GR1/CB偏移、PA/SID位宽，
要求映射长度与硬件报告一致、支持4KiB stage1、拒绝EXIDS（当前installer只
支持15-bit旧格式）和stage2-only bank。无ID样本时不采用假定的0x8000 CB基址；
此处仍为纯解码，不自行读取未供电/未持有的SMMU。

具名SMMU MMIO适配器现把上述plan/geometry转为实际32/64-bit volatile访问，
只接受确认的64KiB窗口、4KiB寄存器页、ACTLR0x303和预留CB/SMR。所有写入值
必须等于plan定义的配置或合法禁用值；没有FSR清除、global reset、全局TLBI或
任意offset接口。lease回调每次操作前检查归属/电源/TBU，当前只在RAM aperture
测试，未接任何手机启动路径；真实lease实现仍是启用前置。

OOB keepalive实现仅支持GPU请求0，按实际HFI version branch选择旧/新位编码；
要求fresh ACK且GX power/clock均on才报告HELD，清ACK不释放请求。有未完成任务
不能释放OOB；发release也不当作GX已off。失败保留请求和资源供后端恢复，不写
保留的boot-slumber/DCVS OOB id6/7。此模块仍以假寄存器测试，不构成实际GX上电。

### 复审后的失败策略与属性查询

### GX perfcounter/GPU-busy 子块（a6xx_start 的另一半）

### CP 侧 halted 预配置顺序编排

### oob_gpu keepalive 与 H2F_MSG_START（A640 GX 启动前置）

### 只读 GX halted-window 诊断开关（`cephgx=1`）

### 安全上下文存在时的 GX VA 分区（新发现的硬约束）

### A640 GBIF 块（`a6xx_start` 的第一步）

### A640 HWCG 块（`a6xx_hwcg_set(true)`）

### CP 解除 halt 的门禁：ZAP/PAS 是硬前置

`a6xx_rb_start()` 的顺序是：写 `CP_SQE_INSTR_BASE_{LO,HI}` →
`a6xx_microcode_load()` → `CP_SQE_CNTL=1`（清 SQE halt，CP 开始执行）→
`CP_ME_INIT` → `adreno_set_unsecured_mode()`。

其中 `a6xx_microcode_load()` 在 `device->mmu.secured` 为真时，**必须**通过
`subsystem_get(zap_name)` 经 PIL 取到 ZAP，否则直接返回 `-ENODEV`/`-EPROBE`。
本机 FDT 有 `gfx3d_secure`，所以这条路径是必经的。

而 `adreno_set_unsecured_mode()` 在没有 ZAP 时会走
`RBBM_SECVID_TRUST_CONTROL = 0` 的兜底分支——那是**关掉安全校验**，不是满足它。
为了"让它能启动"而走这条分支，等于绕过安全固件，明确不做。

`cepheus_cp_unsecure_gate.h` 因此把前置条件显式化并**无条件拒绝兜底分支**：
`wants_trust_control_fallback` 无论其他条件如何都返回
`CE_CP_UNHALT_FALLBACK_FORBIDDEN`；有安全 CB 但无 PAS 认证的 ZAP 时返回
`CE_CP_UNHALT_ZAP_NOT_LOADED`；其余 `instr_base`/`ring`/`ME_INIT` 缺失各自有
独立拒绝码。该模块不执行任何写操作，也不授予权限——它只保证"没有 ZAP/PAS
就绝不清 SQE halt"这一条在代码里无法被绕过。

结论：**ZAP/PAS 可信加载是本移植 GPU 执行的硬阻塞点**，不是可选的完善项。

`a6xx_start` 在 GBIF 之后调用 `a6xx_hwcg_set(adreno_dev, true)`。A640 走默认分支
（非 a612/a615），顺序为：

1. GMU AO `CGC_MODE/DELAY/HYST`(0x23b09/0x23b0a/0x23b0b) =
   `0x00020202`/`0x00010111`/`0x00005555`
2. 读 `RBBM_CLOCK_CNTL`(0xae)；**若已等于 `0x8aa8aa82` 直接返回**
3. 清 `GMU_GX_SPTPRAC_CLOCK_CONTROL`(0x1a880) bit0
4. 写 49 项 `a640_hwcg_regs` 表
5. 置回 `0x1a880` bit0
6. 写顶层 `RBBM_CLOCK_CNTL` = `0x8aa8aa82`

第 2 步的提前返回是语义的一部分：对已门控的块重复编程会再次无谓地开关
SPTPRAC 门控。`cepheus_gx_hwcg.h` 保留该分支，并记录 `skipped`。
冷启动共 55 次写（3 + SPTPRAC off + 49 + SPTPRAC on + 顶层）。

49 项表覆盖 GPU word 到 `0x533`，所以短/错位的 GPU aperture 在**任何写之前**
就拒绝，不会中途越界。测试验证 55 次写的落点、`0x1a880` 最终 bit0=1、
提前返回时 `writes==3` 且不重写表、halt 全程保持、
以及错误 chipid/aperture/CP 解除 halt/CP fault/GMU fault 全部前置拒绝。

`a6xx_start()` 第一件事是 `adreno_vbif_start(adreno_dev, a6xx_vbif_platforms, ...)`。
平台表按 `adreno_is_a640` 选 **`a640_gbif`**，不是 `a630_vbif`：
`GBIF_QSB_SIDE0..3`(0x3c03..0x3c06) 各写 `0x00071620`，
`RBBM_GBIF_CLIENT_QOS_CNTL`(0x11) 写 `0x3`；而 a630 表里的
`RBBM_VBIF_CLIENT_QOS_CNTL` **A640 不写**。混用会多写一个 A640 不该碰的寄存器。

`cepheus_gx_gbif.h` 独立编码这 5 次写入（不改动已验证的 23 项 GX core 计划），
每次写入前后都要求 CP halted 窗口成立，退出保留 halt，不 halt/deassert GBIF、
不 reset、不碰时钟/投票。测试验证 5 个精确落点、`0x12` 未被误写、
GBIF halt 寄存器(0x3c45/0x3c46)未被触碰，以及错误 chipid/aperture/
CP 解除 halt/CP fault/GMU fault 全部在写入前拒绝、每次 lease 失败路径。
仍为 RAM aperture 模拟。

保存的 FDT 里 `/soc/qcom,kgsl-iommu@0x02CA0000/gfx3d_secure` 存在
（`iommus = <0x1b8 0x02 0x400>`，即 SID2/mask0x400）。按 `kgsl_iommu.c`，
这会让 `device->mmu.secured = true`，于是 `adreno.c:_set_secvid()` **会真的执行**，
并且 `setup_64bit_pagetable()` 把 secure pagetable 限定在
`[secure_base, 0xfc000000)`，同时把所有非安全 pagetable 的可用区间压到
`[0x300000, secure_base)`（32 位/compat）与 `[0x500000000, 0x600000000)`（64 位）。
本机 FDT 没有 `qcom,secure-size`，因此 `secure_size` 取默认 `SZ_256M`，
`secure_base = 0xec000000`。

也就是说：**这台机器上用户上下文不能使用 0xec000000 以上的 32 位 VA**。
此前只按"39 位 VA"做约束，没有体现这条 carve-out。

`cepheus_gx_va_layout.h` 现在把该分区算成纯逻辑并强制：任意 GX 映射必须
完整落在某一个非安全区间内（跨界直接拒绝、不做裁剪），secure 窗口不得出现在
非安全表里，窗口上界必须被页表几何覆盖（39 位可覆盖到 24GiB 的 64 位窗口，
34 位不行），GMU 工作区的 `0x60000000` 仍落在 user32 内不受影响。
非安全尺寸必须 4KiB 对齐且不超过 256MiB，否则拒绝而不静默取默认值。

**仍然缺失**：真正分配安全 CB、写 SMR/S2CR、认证 ZAP/PAS、写 SECVID
（`RBBM_SECVID_TSB_CNTL`/`TSB_TRUSTED_BASE/SIZE`、`TRUST_CNTL`）全部没有实现，
因为没有可信 SCM/PAS 通道。该模块只保证地址空间不被自己占错，不授予任何写权限。

真机目前没有任何手段在不写电源的前提下采集 GX 侧真实读数。新增
`--gpu-gx-probe` / `cephgx=1`，严格只读：

读前必须同时满足 `CFG_AHB` 有效、GMU 已供电/上时钟、`SPTPRAC_PWR_CLK` 的
power/clock 位为 0、`AHB_FENCE` 为 ALLOW(0)；任一不满足就打印“GX not read”
并停止，不把复位值当事实。满足后只读 `CP_HALT`/`CP_FAULT`/`PROTECT_CNTL`、
ring/SQE 高低半、12 个 64 位地址模式位、UCHE trap/GMEM 范围。
**没有写、没有清、没有解除 halt、没有提交。**

日志检查器新增 `gx-halted-window` 项，四种结局互斥：未请求
(`NOT_REQUESTED`，不算缺口)、安全拒绝、读到且 CP fault 为 0、以及
“请求了却什么都没有”/“读到但 CP fault 非 0”/“拒绝与读到同时出现”分别记为
MISSING/FAILED/CONFLICT。拒绝本身被视为安全结果，读取到的内容也**不是**写权限。
读取门限另有独立单元测试（55 个 host 程序）。真机未验证。

源码 `_adreno_start()` 的顺序是：`kgsl_mmu_start` → `oob_set(oob_gpu)` →
`hfi_start_msg()` → 才写 GX 寄存器；A640/A680 的 `a6xx_gmu_hfi_start_msg()`
实际发送 `H2F_MSG_START`(id10，4字节纯 header)。此前本移植只有 HFI boot 和
DCVS，**没有 keepalive、也没发 id10**，属于 GX 启动链的真实缺口。

现启动链补为：HFI boot → DCVS(id30) → `oob_gpu` keepalive 持有 →
id10 → 热检查。缺 OOB 后端时在**任何电源副作用之前**拒绝，绝不把 id10 发进
一个没有被 pin 住的 GX（否则 GMU 可能在调用方操作期间把 GX 掉电）。
失败路径只有在上限停机**确实完成**后才释放 keepalive；停机被拒绝则保持持有，
不让设备意外掉电。退休路径同样要求：持有 keepalive 却拿不到 release 后端时
直接拒绝，避免退完还让 GX 常亮；释放失败会作为错误上报，不静默吞掉。

新增本地场景 mode22（keepalive 永不 ACK）：验证停在 `CE_BR_KEEPALIVE`、
`start_sent==0`、id10 从未发出、keepalive 保持 `CE_OOB_FAILED` 且不释放。
退休用例新增断言：keepalive 必须在 slumber→park→detach 全部完成之后才释放。
仍为模拟寄存器，真机未验证。

此前 GX core(23写)、GX perfcounter(5写)、保护表(45写)、halted ring/SQE(7写)、
恢复快照+ME_INIT 各自独立可用，但**没有顺序约束**。源码里 `a6xx_start()` 先
HWCG/UCHE/ROQ，再 `a6xx_enable_64bit`/`a6xx_protect_init`，之后 ring buffer
才写 SQE/RPTR 地址，最后取 powerup reglist 快照。顺序颠倒会读到未配置的
保护表或把地址写进尚未 64 位化的上下文。

`cepheus_cp_preinit.h` 把这五步串成一次 one-shot 编排：
GX core → GX perfcounter → 保护表 → halted ring/SQE → 恢复快照。
每步都要求 CP halted 窗口成立；任一步失败即停在该 stage，不进入 READY。
编排前额外要求工作区已 `CE_CP_WS_TABLED` 且 `restore_ready` 仍为 false
（快照必须在这里取，预置值意味着重复/外来捕获），并核对五个引擎共享同一
GMU aperture、同一 GPU aperture、同一 owner/lease，ring/SQE/RPTR 三个 VA
必须逐字等于页表里的 region，不接受调用方重新填数。

共 80 次写。**不含**：ZAP/PAS 认证、安全 CP aperture、GX SMMU 上下文安装、
OOB keepalive、CP 解除 halt、WPTR doorbell、任何 GPU 命令提交。
测试验证 80 次写的精确分布（23/5/45/7 + 快照）、失败即停、
ring VA 不匹配/错误 chipid/owner 不一致/非 TABLED/已 restore 全部在写入前拒绝、
以及全程 halt 保持、WPTR/SQE 高半/CP fault 未被触碰。仍为 RAM aperture 模拟。

`cepheus_gx_core.h` 的23项写入覆盖UCHE/GMEM/ROQ/UBWC，但`a6xx_start`在同一
halted 窗口里还配置性能计数器与 GPU busy mask。这些寄存器**跨两个 aperture**：
`RBBM_PERFCTR_GPU_BUSY_MASKED`(0x50b)、`RBBM_PERFCTR_CNTL`(0x500)是 GPU 相对
字偏移，而 `GMU_AO_GPU_CX_BUSY_MASK`(0x23b0e)、`GMU_CX_GMU_POWER_COUNTER_ENABLE`
(0x1f840)、`_SELECT_0`(0x1f841)在 GMU aperture。混淆两者会把值写到错误的块。

`cepheus_gx_perfcounter.h` 按源码分域编码5项写入，并保留源码的语义差别：
`POWER_COUNTER_SELECT_0` 在 `adreno_a6xx.c` 是 `kgsl_regrmw(...,0xff,0x20)`，
所以本模块做掩码 RMW 并读回校验，绝不盲写覆盖相邻的 counter-select 字段；
其余4项为普通写入加读回。每次写入前后都要求 CP halted 窗口仍然成立，
退出时保留 halt、不 reset、不上电、不碰时钟/投票/HWCG/VBIF。

本地测试覆盖：5项精确落点与跨 aperture 正确性、SELECT_0 高位保留、
错误 chipid/错误 aperture/CP 解除 halt/CP fault/GMU fault 全部在写入前拒绝，
以及每一次 lease 调用失败路径。仍为 RAM aperture 模拟，未接真机。

复审08c03ba7确认共享发布、显式NC映射、悬空table及ALLOCATING注释已处理，
但指出一次不确定退休将保留560KiB，原1MiB预算不能创建第二个workspace。
此诊断阶段明确采用**失败即终止本次GPU会话**，而不是假设超时可重试：HFI超时
会话已poisoned，重复命令/状态不明确时重放可能不安全。增加pool-terminal闩锁、
显式错误日志和group_status查询，失败不再反复分配尝试。RETAINED明确是终止态。
这解决了含糊状态和静默资源丢失，但**不提供生产级故障恢复/可用性保证**。
不能用更多内存掩盖未停稳硬件继续DMA的可能性。

复审称success=0会清group与源码不符：该分支原本提前return且保留身份；真正
缺口在已detach但某complete失败后的无条件清group。现仅全部释放成功时清身份，
otherwise保留DETACHED_RETAINED状态、root/ASID/handles及剩余bytes。两类失败
有各自测试，不向外暴露强制解除隔离。对complete失败能否重试仍需确认其原子性，
不能因为GPU已停就假定重复complete无害。

CI41494467129发现IOKit导出头没有pmap_cache_attributes声明（MACH_KERNEL_PRIVATE
隔离）。改用XNU已有C ABI桥`IODefaultCacheBits(pa)`，其真实实现正是
`pmap_cache_attributes(pa >> PAGE_SHIFT)`；不设置MACH私有宏污染整个IOKit编译。
仍逐页检查实际WCOMB属性，未把检查删掉。该修复需要下一次完整构建确认。

### 工作区与实际HFI队列头绑定

`cepheus_gmu_hfi_link.c`将已发布的NC工作区与HFI session连接：read/write index
直接指向共享ABI queue header，不再用独立host变量。绑定前要求原始队列表
完整一致且无已消费数据，不通过重置指针丢弃旧消息。每次访问核对group身份、
状态和平台租约；退休状态只允许当前session的PREPARE_SLUMBER(id33,length3)
交换，普通命令拒绝。成功/失败退休后显式invalidate清掉CPU指针。
联测使用同一条已绑定session先查询版本，再通过该session完成slumber/park/
detach/释放或四阶段失败隔离，防止“另造session”掩盖退休状态的协议冲突。

统一GMU MMIO adapter现在将具名CM3寄存器、OOB0及HFI doorbell转换为同一
0x30000 GMU aperture的volatile访问。配置写入只允许精确启动plan的值，
RPMh capability只允许保留旧位再OR规定mask，OOB只允许当前版本bit编码；
status清除与普通配置隔离。host用RAM aperture测试实际读写落点及越界/错值/
租约失效拒绝。adapter不负责创建映射或取得lease，没有接入自动startup。
