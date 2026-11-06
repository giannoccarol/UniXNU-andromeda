# 两个GPU参考项目核查（2026-11-06，只读源码调查）

通过GitHub CLI按完整提交获取，未执行项目构建/部署脚本，未操作手机。
仅运行明确的RAM-only检查`tools/check-raphael-gpu.py`，见下方测试范围。
本地参考：`../inputs/gpu-refs-20261106/`。源码存在、项目记录的实测、我们
自己的硬件实测必须分开；这些参考项目没有在Cepheus上被本轮验证。

## 固定来源

- HeliumStudio-Dev/hackios-xnu-kernel，默认raphael-bringup，
  `57722468a3774e2ec60017705ec356ae9b31f40e`。
- Chengdong-Yu/UniXNU-pong，main，
  `e0ee6df109b4b5dc706cac660d4f8210dc911dac`。
  GPU代码已拆为submodule，不能只看顶层空目录：
  - PongAdreno: `3e8ffa2424e7915a1cc35da14b043057a87707e7`
  - PongMetal: `ccdbc294a9fa95b341fa6f926870ca07ddc663a4`
  - AdrenoMetal: `418c550887db141455270c151628d657dd545a7f`
  - PongPower: `c37179ed12ba7f981b2b088aea482b969085eb84`

## HackiOS：最接近本机的真实平台参考

对象是Raphael/SM8150/A640，iOS16.5/20F66。当前分支为**GLES-only**；
2026-10-27迁移记录明确撤下Metal，不将较早Metal测试结果套到当前HEAD。

### 核心源码

- `xnu/files/bsd/dev/hakios.adreno.c`，4188行：真实MMIO、时钟/电源、
  GMU固件上传、PDC/RSC/HFI、GPU/GMU SMMU、SMC/PAS13、CP/ring/fence、
  用户态提交和停止。4092–4104的A640分支实际选择GPU0x02c00000、
  GPUCC0x02c90000、GMU0x02c6a000、RSCC0x02c8d000、SMMU0x02ca0000。
  这不是只有host planner或read-only probe。
- 1450–1498：A640 ACTLR0x303，GPU SID0/mask0x401、GMU SID5/mask0x400，
  4KiB设备页表和真实CB读回/TLB sync。host页可为16KiB，1207–1330附近把
  host页划成4KiB页表块；因此不能笼统断言A640必须改XNU为4KiB才能工作。
- 1530–1620：原生`smc #0`，QCOM A6 resume。1670–1723与2764–2819
  执行PAS init(1)、mem_setup(2)、auth_reset(5)，没有把DXE函数指针当OS接口。
- 2748：ZAP reservation `0x98f15000/0x2000`，加载a640_zap.b02。
  这个地址**也存在于我们已保存Cepheus FDT**的
  `/reserved-memory/pil_gpu_region`（no-map/removed-dma-pool），不是只能使用
  1MiB动态分配。仍需证明我们加载器保留其所有权、不与payload冲突。
- `xnu/files/iokit/Kernel/hakios.gpu.cpp`，1192行：HakiosGPU IOService/
  IOUserClient，BO分配/映射、IOSurface导入、用户命令、fence和生命周期。
- `tools/mesa/hakios-drm/hakios.drm.c`：把Mesa的msm/libdrm操作转换成
  IOConnectCallStructMethod/IOConnectMapMemory；配Mesa freedreno/IR3。
  EAGL/GLES到CoreAnimation的前端在userland/src/eagl.c、gl_osmesa.c、
  hakios.gl.h，不能仅更换kernel就得到GUI加速。

### 不能原样照抄的风险

- `step_zap_load` 2787–2799：init_image或mem_setup任何失败会打印
  “TAKEN TO MEAN THAT IT KEEPS A ZAP SHADER FROM BEFORE”，然后
  `zap_is_loaded=true; return true;`。这不是可靠的认证/所有权证明，
  本项目不得引入。正常路径仍调用真实auth_reset，不能因此否认整个实现。
- 563–605：CX基于rail已有供电的假设直接清collapse，并对旧GPU尝试stale_stop；
  不能把同SoC运行结果当我们共享ARC/电源租约已满足。
- 2410附近只建一档DDR BW表、使用其独立GMU协议/OPP子集；不能替换我们的
  完整Cepheus源绑定OPP/BW表或改变DCVS哨兵修正。
- 3090以后停止有direct GX collapse/盲stale上下文撤销。本机单次机会不适合
  引入未证明的stop/reset接管；仍保留我们的失败隔离策略。
- HFI接收、共享GPU地址空间与UserClient任意PM4范围等需独立安全审查；
  文件存在和旧设备测试不等于适合公开用户态接口。

### 实测材料

README与docs/RAPHAEL.md记录FD640/OpenGL ES3.2、1080×2340三角形绘制/
回读、GL error0、持续CoreAnimation提交；历史章节记录独立Metal片元、深度、
索引和1/2/4x resolve。当前HEAD明确不提供Metal驱动，10-27基线尚有后续
兼容性待验收。引用日志路径主要在作者本机，未取得对应原始日志，不当作
我们亲自验证或“当前版本全部稳定”。

本地使用已保存Cepheus `a640_gmu.bin`运行其RAM-only host检查，ASan/UBSan
通过：7个固件block、共享0x60000000页、GMU固件复制/读回、PDC/RSC、GPU参数及
A640/642L分支。未执行任何MMIO、SMC、PAS认证或shader，不能称真机GPU通过。
测试使用其源码直接导出的offset，仍不是对寄存器来源的独立证明。

## Pong：平台与Metal整合更完整，但硬件不同

对象Nothing Phone2/SM8475/Adreno730v3 `0x07030002`，主要22H374/Darwin24，
另有20G81 ABI参考。不是A640，也不是仅拿A740表换固件。

### 实际接通的部分

- `drivers/PongAdreno/PongAdrenoPlatform.cpp`：真实GCC/GPUCC/RSC/cmd-db，
  cold ownership，APSS DRV2 active TCS3、parent票、物理内存、SMMU/GMU启动。
  92–169用**实际AUX最高corner**保留CX/MX，检查TCS4冲突及新事务完成。
  这是它绕开“未知旧投票不降票”的取舍，不是发现了可靠read-current-vote。
  我们不因此默认采用最高corner或取消热/电源边界。
- `PongAdrenoZap.c`：split MDT/签名/代码规划、真实PAS13，只有transport0且
  secure0才成功；失败保留资源/poison，不把错误当旧ZAP已加载。
  其reserve为`0x8b91a000/0x2000`，不能复制到Cepheus。
- `PongAdrenoCp.cpp:416–469`：GX/OOB/START -> hardware init -> CP解除halt/
  ME_INIT -> RPTR+GPU idle -> ZAP -> secure-mode包 -> 真实memory write/fence
  -> IRQ332 -> native客户端发布。其A730先secure bootstrap后ZAP顺序由Nothing
  gen7源支持，**不能替换A640在microcode加载阶段先认证ZAP的门禁**。
- `submit/`包括NativeAdrenoMemory/Vm/Ring/Submit/UserClient/IRQ/Timer，
  原生BO wire ABI1、IOSurface引用、失败隔离与CP时间戳。
- `PongMetal/`包括NSBundle/IOAcceleratorES discovery、Objective-C++设备
  前端、Rust对象bridge与metal2vulkan/Turnip，最终AIR/IR3链到真实CP。
  `_MTLDevice`大小20G81=528，22H374=600；这些不等于我们20H392的ABI。
  需核对本项目实际dyld缓存，不按OS大版本猜布局。

### 实测与稳定性

顶层migration/porting记录比submodule README新，后者部分“未确认”章节已经
过时。services154/155记录908/908、827/827命令完成与scanout；services160
确认900MHz/DDR11请求被ACK，但CPU缓存维护占长帧，不能用高频等同性能修好。
services165首次捕获4096次提交/4095完成、未捕获CP/SMMU故障；同文件随后
**第二次启动有FSR80000402/FAR0xe0d4100及DeviceLost**，明确不列稳定候选。
因此可参考架构与错误处理，不称该最新版本无缺陷成品。

## 对本项目建议

优先组合：**HackiOS A640平台事实/固件启动 + Pong资源生命周期/用户态分层**。
第一阶段先真实PAS13、GMU和受限CP执行；第二阶段接Mesa freedreno GLES，
利用iOS16的CoreAnimation EAGL路径得到图形加速；Metal另行按20H392做ABI适配。
这比直接把A730/22H374/Turnip整套复制过来更贴近当前设备。

必须保留：我们已验证的启动/触摸/电源、pmap-managed+NC检查、源绑定配置、
不可伪造ZAP成功、未证明DMA静默时不free。唯一fastboot机会仍未使用。
许可证分别核对：HackiOS xnu/files为APSL2、其用户态MIT；Pong多数BSD/MIT，
生成CP profile为GPL2-only，translator/Mesa各有自身许可。

本轮只形成源码调查，不合并参考代码，不宣称因此已解除真实硬件阻塞。
