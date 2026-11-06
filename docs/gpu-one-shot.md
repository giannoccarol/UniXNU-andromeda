# 单次 GPU 真机机会策略（2026-11-06）

**最新指示已改变：用户允许多次真机测试，可测试时直接告知。** 下方“一次机会”
仅为历史约束，不再限制当前推进。按内存 -> workspace -> SCM/PAS -> GMU/CP
逐阶段准备与告知，保留旧基线；不因此解除电源/安全认证/停止证明门禁。

用户要求推进GPU可用，设备在fastboot，仅一次验证机会。**此机会尚未使用**。
不为获取一组只读探针就启动，也不将多个自动重启当作“一次”。

## 唯一启动前必须完成

1. 真实物理分配/NC映射路径：可用连续PA与host页属性；host测试和kernel编译
   不能代替真机。final试验必须先运行自检，失败时不碰GPU。
2. 电源/DDR/时钟：apps-DRV原投票及MX/CX关系有可信继承证据，DDR bootstrap
   独立租约，不把HFI BW_TBL当作外部总线投票；平台独占/共享所有权成立。
3. GMU：PDC/RSC、QMP、SMMU stream/CB/TBU、cache/TLB、固件上传和HFI
   绑定真实后端，故障与热监督可用；不存在自返回成功的lease。
4. CP：GX初始化/secure mode、可信PAS13加载ZAP、SQE/ring/ME_INIT和受限
   非安全协议；不得TRUST_CONTROL=0、no_auth或伪造zap_loaded。
5. Stop：调用相同会话slumber/park/detach，再确认GX/CM3和所有DMA已静默；
   失败保持资源而不释放。必须准备故障日志和有限时间停止策略。
6. 可用性：先以受限scratch/fence证明GPU执行；若目标为SpringBoard加速，
   还需真实IOAccelerator/Metal ABI、shader编译和同步。命令成功不等于GUI加速。

## 本轮真实新增成果

- `007a683`：A640 LLC五个SCID12的GBIF RMW、保留XBL HTW与MMU-500语义；
  冷启动reset前置拒绝，以及RCG UPDATE写前重新验证lease。
- `637bc7a`：受限SCM query引擎与原生XNU thread-context SMC bridge（未hook），
  INFO call-available和PAS13支持查询，完整返回x0..x7，无DXE指针/共享buffer。
- `1d24457`：拒绝过期/畸形discovery成功字段；bool supported不能伪填。
- 本机ScmDxe静态反编译与RPMh READ completion补证；静态证据不作为真机事实。
- 62个host程序、AArch64生产SMC callback编译/汇编、source对照、ASan/UBSan
  23个fixture场景；receipt `../logs/gpu-offline-1d24457/receipt.json`为22项PASS。

## 尚不能据离线证据解除的阻塞

| 项目 | 当前状态 | 为什么不能强行继续 |
|---|---|---|
| ARC/DRV继承 | 未证实 | response来源已找到，但本DRV旧投票语义仍未知 |
| Native SCM conduit | 桥已实现、未真机查询 | CPU PSCI成功不证明SIP PAS可用；SMC不返回无法本地强制超时 |
| PAS/XPU memory/crypto BW | 未实现可信资源管理 | 无法将GMU普通buffer改名当secure分配 |
| 真实SMMU/电源/DDR/LLCC租约 | 未接入 | FDT/寄存器值和模拟lease不是实际共享所有者 |
| CP启动及停止平台后端 | 未可执行 | 无ZAP认证不得unhalt；没有停止证明不得回收 |
| iOS Metal/IOAccelerator | 缺失 | 不可把scratch测试宣传为SpringBoard GPU加速 |

因此**现在不具备可承诺一次启动就GPU可用的镜像**。继续源码工作可减少缺口，
但不能诚实地保证未测试的电源、PAS和用户态ABI在唯一启动中全部成功。
基线镜像仍是已验证UI正常的`boot-cepheus-ready-9dc6e6e.img`，不覆盖它。

## 本轮完整构建证据

- `42168758490` @`007a683` completed/success，内核SHA256
  `bfc7be6482503d0f806d92fdb2258c85aef3ab1b02fbe72eb222ca9bba86d6cf`。
- `42169779972` @`637bc7a` completed/success，**包含真实XNU的SMC callback
  编译/链接与符号检查**，内核SHA256
  `2d3ed01858fd85eaab794ef2d2e04bfdbc9591fcfbdcd037e99c47308ea31d9e`。
  产物和日志在`../inputs/ci-42169779972/`、`../logs/ci-42169779972/`。
- 后续`1d24457`修改为discovery初始字段与布尔返回检查，host/source/codegen
  回归通过；它不在上述@637bc7a构建内，不能用该内核冒充最终HEAD。
- 两个新内核均未生成要在设备启动的GPU-execution镜像，未做任何手机命令。
  真实PAS13支持、内存自检、电源与命令执行仍没有新增真机结果。
