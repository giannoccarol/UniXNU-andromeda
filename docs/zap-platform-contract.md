# A640 ZAP/PAS 平台接入事实与缺口

范围：本地SM8150内核`36f369fdb77319a2984a48b89f025ac3e5f97386`、保存本机FDT
SHA256 `bdd364a16ceabe9960e128103162e5280b150303624a123dfd71e871f9a43274`，
以及已验证容器结构的本地`a640_zap`文件。**没有执行SMC、认证、加载或启动ZAP。**

## 已核对的链路

- FDT `/soc/qcom,kgsl-hyp`兼容`qcom,pil-tz-generic`，firmware-name `a640_zap`，
  PAS ID13；无`qcom,pil-no-auth`、无指定memory-region/subsys-vmid/clock-names。
  `scripts/gpu-fdt-config.py`现在解析并断言这些字段。缺少属性不等于不需要平台资源。
- `adreno_a6xx.c:a6xx_microcode_load`设置SQE地址后通过`subsystem_get(zap_name)`
  加载ZAP，失败不准继续。`a6xx_rb_start`随后才释放SQE halt、发CP init，并通过
  已认证的ZAP路径切入非安全执行。没有ZAP时直接改TRUST_CONTROL的参考回退
  **不是本移植允许的方案**。
- `peripheral-loader.c:pil_boot`顺序：解析MDT/分配映射 → proxy vote → init_image
  → mem_setup →（有VMID才做对应内存归属）→ load segments → auth_and_reset。
  不得先auth成功假设，再补内存；不得把完成回调假定为安全状态。
- `subsys-pil-tz.c:pil_init_image_trusted`复制元数据到物理连续DMA buffer；该下游
  使用`DMA_ATTR_STRONGLY_ORDERED`。`drivers/firmware/qcom_scm.c`同类实现明确写明
  4K对齐、连续、不可缓存以避免XPU违规。**普通GMU NC池不能自动当作PAS allocator**：
  还需SCM可访问PA范围、XPU粒度、缓存/CPU别名与内存归属的真实证据。
- init_image后原厂释放元数据buffer，是原厂已完成调用的行为；XNU在SMC传输状态
  不确定/超时的情况下不得照抄释放。auth失败同样不能盲目释放固件区域。

## 原厂调用参数（用于实现审查，不是可直接执行的SMC指令）

`include/soc/qcom/scm.h`：SVC_PIL=2，SIP FNID宏为`0x02000000 | svc<<8 | cmd`。
这不是最终SMCCC调用约定；不能跳过is_scm_armv8/SMC32或64发现。

| 阶段 | command | args | arginfo | 必须检查 |
|---|---:|---|---:|---|
| init_image | 1 | PAS13、MDT物理地址 | `SCM_ARGS(2,VAL,RW)=0x82` | 传输返回＋secure result |
| mem_setup | 2 | PAS13、region_start、logical size | `SCM_ARGS(3)=3` | 两层返回 |
| auth_and_reset | 5 | PAS13 | `SCM_ARGS(1)=1` | 两层返回，不等于CP已工作 |
| shutdown | 6 | PAS13 | `SCM_ARGS(1)=1` | 真实停止后才考虑归还内存 |

下游scm_call2读取`desc.ret[0]`；另一SCM64实现读取`res.a1`。传输层成功但secure
result非零不能当作认证成功。SCM PAS路径还会投票crypto带宽；FDT没列ZAP专属
clock/regulator不表示可以忽略该共享路径。不能复用GPU BW表冒充crypto路由。

## 本地文件与分配边界

`inspect-zap-layout.py`证据：ELF32/DSP6，14256B，三个phdr；MDT=148B头+6712B
hash segment=6860B。实际load段1968B，重定位逻辑mem_setup跨度4KiB，原厂按
段alignment分配1MiB。**mem_setup长度是逻辑4KiB，不是分配1MiB**，释放/accounting
仍需要真实1MiB区域。容器digest/布局验证不验证签名、DSP指令或secure策略。

## XNU当前必须补齐而未补齐

1. ExitBootServices之后可用的真实SCM transport/convention发现、返回错误处理、
   共享buffer cache语义和阻塞/中断策略。UEFI协议存在不证明其运行时可调用。
2. PAS/crypto带宽和时钟资源的共享所有者；不能越过现有ARC投票继承问题。
3. PAS专用连续且对齐内存/MDT、真实PA可访问性、XPU保护粒度和可靠归还路径。
4. 固件生命周期绑定（已认证ZAP实例、GPU上下文、复位epoch一致）、shutdown证据。
5. GX/CP执行与非安全切换的正式协议，不能发no_auth或直接清TRUST_CONTROL。

因此当前继续停在离线材料/源代码可证明范围，不增加伪成功PAS后端、不在手机上
试SMC、不把纯planner或public CI通过视为认证可行性已确认。

## 2026-11-06补充：真实传输候选不是可信加载完成

`cepheus_scm_discovery.h`现在有受限的call-availability/PAS13-supported编码与
one-shot引擎；`cepheus_gpu_compile.c`有线程上下文、mutex、完整x0..x7输入输出
的原生SMC callback。`test-scm-codegen.py`提取**该生产函数本体**编译为AArch64
汇编/对象（不执行），源码测试核对info6/pil2/支持查询7及64->32约定。

只读查询后端**没有接入startup**，更没有提供PAS init/mem_setup/auth_reset/
shutdown或MDT共享内存。若将来人工批准使用它，成功也只能表示查询可用；
不能把supported=1作为ZAP实例已认证，不能给`zap_loaded`填true。
任何SMC单次调用都可能不返回，本地最多16次的引擎不提供secure-world watchdog。

本机UEFI的SCM二进制静态证据见`scm-static-evidence.md`。PSCI CPU_ON实测成功
说明PSCI conduit存在，不代表SIP PAS通道/共享crypto带宽/XPU区域也已验证。
