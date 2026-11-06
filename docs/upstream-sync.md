# 上游同步记录（2026-11-04）

## 范围与保护

本轮用户不在场，禁止启动手机镜像或刷写。`upstream-lock.json` 是本轮
完整 SHA 清单；`firmware_candidate` 与 `baseline` 分开，不覆盖已验证默认值。
这次是**固件构建候选同步**，不是把 iOS16/SM8150 的驱动 ABI 换成 A740。

| 来源 | 原版本 | 本轮同步/核查 | 接入状态 |
|---|---|---|---|
| UniXNU/UniXNU | b6ae99c | 350ddc2（领先575） | 新固件候选；主树未动 |
| Project-Aloha/mu_aloha_platforms | 0b798128 | d8aab0e2（领先22） | 新固件候选；主树未动 |
| UniXNU/DarwinBoot（UniXNU 子模块） | 85332b3 | 3c75734（领先6） | 候选；保留32MiB对齐补丁 |
| JohnLeeeeeee678/UniXNU-raphael | 历史移植底本 | b5b46ed9 | 独立参考树；不覆盖板级参数 |
| dipa-studio/ios-sweet | 45263bee | 45263bee | 已最新，内核继续固定 |
| blacktop/darwin-xnu-build | 663906528 | 663906528 | 已最新，构建器继续固定 |

之前只检查最后两项就说“无上游更新”是不完整的；把五个顶层来源称为四个
也错误。DarwinBoot、MU_BASECORE 等还属于递归依赖，不能遗漏。

## 实际集成

1. 本地新树：`../upstream-sync/{unixnu,uefi,raphael}`。前两项是独立
   detached worktree，既有 dirty tree 中的修改不丢失、不 reset/clean。
2. `scripts/prepare-upstream-firmware.py` 校验最新完整 SHA、干净状态和
   所有 patch 的 `--check` 后才修改。没有 `--reject` 或“失败也继续”。
   UniXNU 的 Main.c 已有 `1833131` connect-all 修复，不再重放旧补丁；
   firmware.py 保留新版 preflight 参数与环境支持，只保留必要板级替换。
3. 新 `upstream-firmware.yml` 单独构建 FD，不需要旧 kernel artifact，
   不下载 root/私有密钥、不打包 boot.img，更不运行 fastboot/adb。
   校验 FD 恰好3MiB，产物名称标明 NOT-HARDWARE-TESTED。
4. 上游 `a4419c8` 的内容比较修复已移植到 `scripts/tree.py`：以前仅比较
   大小会保留过时脚本；现在比较字节，同时避免覆盖外部 symlink 目标。
   `scripts/test-tree.py` 覆盖同大小变化、device override 和 symlink。
5. BearSSL 不是版本更新：只切换传输到含原 Git object 的镜像，校验原
   commit 与 tree。详见 `gpu-acceptance.md` 的纠正记录。

## 上游差异不能直接套用的部分

- UniXNU 新 catalog 已明确 Adreno740 的 A740/SM8550 scope，OS profile 为
  iPadOS17/26/27；没有本项目 iOS16.7.16 + SWEET 的等价 profile。
  因此本次保留 format1 兼容路径和 XNU8796 内核，不能只改 schema_version
  或换 defconfig 来冒充 format2 已迁移。完整迁移需单独建立 SM8150 SoC、
  iOS16 OS profile、ABI/设备树和逐驱动 recipe。
- Aloha22项变化主要是 SM8350/Windows ACPI 和 Mu依赖更新，未见 Cepheus
  板目录直接变动；但 MU_BASECORE 与 SurfaceDuoACPI 更新会影响构建/运行，
  所以“没有板目录差异”不代表可不测就晋级。
- Raphael 最新包含 Windows COM/根镜像刷写工具，本项目使用 Linux UDP
  日志和不同板配置。参考源码已同步，不引入会操作手机的脚本或分区参数。
- 新 DarwinBoot 自述 root-policy sidecar 尚未 built/exercised；解析器
  host 测试通过不代表完整加载器验证。本项目没有提供/启用该 sidecar。

## 验证边界

已执行：所有候选 patch 无拒绝适配、DarwinBoot 生产解析器 host 测试、
tree refresh 回归和 Python语法检查。内核和固件各有独立 CI；以运行记录为准，
不能把排队、源码检查或 FD生成写成手机启动成功。升级基线仍需真机授权验收。

### 本轮构建证据

- 固件 CI `41865748773` @`723adcd`：**success**，全部构建与上传步骤完成。
  FD为3145728字节，SHA256
  `2cbd61c65e3e9b52b749f03f8264cdebbc99431ac533b8677875941ccf812ce0`。
  下载到 `../inputs/upstream-firmware-41865748773/`，SHA256SUMS验证成功；
  日志在 `../logs/upstream-firmware-41865748773/`。
- 本地同版本候选在 `cepheus-build:latest`、`--network none` 容器构建成功，
  FD同为3MiB，SHA256
  `ed10dc68dd8983467f1e71e746e04f6aa7b6b611939e58b007fa95afb1a7907b`。
  产物在 `../upstream-sync/uefi/Build/SurfaceDuo1Pkg/RELEASE_CLANGPDB/FV/`。
  两个不同环境的FD摘要不同，不作bit-reproducible声明；均未启动。
- Mu CryptoBin v1.1.3 zip实取SHA256
  `2136c410dc44135830e2e33062243563d642e0be868d6affc2930c6b0854f5cf`，
  新候选workflow在解包前检查。并非下载一个不同版本的crypto实现。
- 本地构建含既有SMBIOS initializer和LibraryClass警告，且沿用旧的
  `relax_clang.py` 标志；不能将构建成功描述为无警告或运行安全审查通过。
- 两个只读子代理在30分钟上限超时，无改动，也无完整报告。本轮不能声称
  独立审查通过。父会话已直接复核相关代码与实际构建。
- 内核CI `41864232115`：runner未分配，注释
  `The job was not acquired by Runner of type hosted even after multiple attempts`。
  这不是新补丁编译错误；后续 `41868192751` 是单次重试，不无限派队列。
- 离线receipt：`../logs/gpu-offline-20261105-sync/receipt.json` @`723adcd`，
  **19项PASS、零SKIP**，包含60个host程序、23个真实固件fixture模拟场景、
  ASan/UBSan和7生产单元静态分析。私有fixture仅本地，未进公共CI。
- 重试 `41868192751` 同样未获runner（无编译步骤）。通过gh读取
  actions/runner-images确认：当前macos-15已指向arm64主机，Intel明确标签为
  macos-15-intel，仍含Xcode16.2/macOS15.2 SDK。touch-kernel改成明确Intel
  标签，目标ARCHS继续arm64；这只是主机选择纠正，不改变目标内核ABI。
- Intel内核构建 `41875375125` @`9dc6e6e`：XNU编译/链接、patch/symbol
  gate、artifact上传全部success，下载内核SHA256
  `1ddd182f99a2934870cb3870140db41eef2cc8cde6ccf138a2841143daf62958`。
  记录时整体run API仍in_progress，不假称平台状态已经结束。
- 加zip hash检查后的固件CI `41873397100` @`439aa6f` 再次success，FD摘要
  与41865748773完全相同。本地和CI间的摘要差异仍不推定为已解释。
- CI固件FVMAIN_COMPACT使用2967072字节、剩178656字节（94%），没有扩大
  FD覆盖SEC/页表区域。日志的DarwinBoot `Error 1 (ignored)` 是复制不存在的
  可选`*.pdb`调试文件；其EFI/FFS生成继续成功，不是被忽略的链接错误。
- 触摸回退镜像重新校验SHA256仍为
  `c8eae5a0abd8dd9f8cd58f954497c0510c576b941b09bc0ef1efb2648718ea88`。
