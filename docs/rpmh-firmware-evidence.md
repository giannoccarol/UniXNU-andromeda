# Cepheus RpmhDxe静态取证

目标来自本机xiaomi-cepheus UEFI设备目录，PE/arm64，65536字节：
`RpmhDxe.efi` SHA256
`03ccd266ce0c9e268ac199c0f27903c9dbdac675c9374c797e4178673564a7fa`。
REA/Ghidra12.1.2分析，不执行/修改固件。完整证据在cepheus-port的
`logs/rpmhdxe-03ccd266-evidence-final.json`，快照
`logs/rpmhdxe-03ccd266.snapshot.json`。地址是此静态PE image-relative VA，
不是手机物理地址，不能直接在XNU读这些地址。

## 观察

- `0x14dc`通过UEFI BootServices安装表`0xe0f8`；表头为`0x10001`，包含
  `0x15b4..0x18e4`包装函数。ev_7c07ef033afd8ffd0bfb23bf1e0e72580d31c20f9cec5e00526ee3bfacddaefb，
  表字节ev_5bcfd2c50c3b7d9113298417de8b7fb3e9ff480e0375ba805e45fc3ebd5e7dc4。
- `0x2f80`按资源地址查找/插入24字节记录，计数位于image+0xe644，数组指针
  image+0xe648。ev_3d4d654200b59c2b29b2d6ce9c8e6988a9be1d763c2620e9eece939969ff6708。
- `0x48d4`在每个资源中查找DRV id对应的72字节链表记录，不存在则zero-init
  分配。记录+0为DRV，+0x40为next，资源+8为首指针。
  ev_0e359d2db24982f890ef9b7438bc23f978a076430ed712a613a04399e7ef7335。
- `0x4b38`写入set相关triplet：value在+0x18+12*set、valid字节在+0x1c+12*set、
  状态在+0x20+12*set，再经`0x4a3c`比较缓存数据。
  ev_10104ed4d0eb41791878e28b19561e627f66f40f37e8b02561c068174c1bbf87，
  ev_934c566b2948279469110b4ca8dd390a204fc146386b26d81769c959246e1fee。
- `0x4c34`选择缓存set值并写入+0xc和+0x10(valid)，可能将+4设为1。
  **它由`0x5ae4`的TCS配置路径在触发命令前调用**，因此这个影子值不是
  “硬件已经确认成功”的证明。
  ev_2b9de1a52a93df3363e8a5b71ef8cf0b83ad41672ebf84b69ded72a75fab5e5d，
  ev_6e8b164f46fb5682ca6f8c8ad31646e16c1e8907f5ee61dace20d15eb903f514。

## 推论与限制

观察支持存在“资源/DRV/set”软件缓存，可作为未来UEFI受控handoff的研究方向；
**不支持**直接把+0xc影子值当作当前ARC投票，更不证明READ响应语义。
需先处理未完成请求、锁与ExitBootServices期间的最后写入、loaded-image身份、
内存映射可读边界、空链表/外部投票、跨内核时序。不能按固定位移扫描手机RAM。

本次没有找到已验证的公开只读“返回本DRV已完成投票”协议函数；没有穷举所有
函数，不能据此说不存在。字符串`holding_read_request`存在，但没有交叉引用
不代表不可达。Ghidra对部分包装函数存在函数边界限制，必要处以反汇编补证。
REA bundle中unknowns=0仅表示工具记录格式，不表示硬件语义没有未知项。

因此GPU电源写入仍需可靠当前投票/所有权证据，不能因发现缓存就取消门槛。

## 2026-11-06：READ response 路径补证（仍不是 current-vote 证据）

复用同SHA的REA snapshot；本轮导出
`../logs/rpmh-read-static-20261106/evidence.json`。没有读取活RAM或执行固件。

- 字符串RscHalReadResponseData交叉引用指向`0x7578`：按DRV/TCS/CMD做
  geometry验证，然后读MMIO `base+0xd40+tcs*0x2a0+cmd*0x14`，另一DRV
  模式为`base+0x20d40+...`。这是HAL response寄存器，不是软件投票缓存。
  `ev_5698e6b71c0bf22fdf73b7d7ce700f69da6a6154839aa083152784c857a7e8bf`。
- `0x590c`按DRV/TCS映射调用HAL，返回response值；`0x4784`在完成请求队列中
  针对类型2的命令读response，再由`0x3e48`写入每client请求记录（valid=1、
  value在+4）。其他类型才走`0x4d60`完成缓存。
  `ev_bdac4a1f967dc9cfa60f49a36438c8550654149a0b028943b1ab82fb9ddbfc85`、
  `ev_668dbfadee54cb109d6991d680d97d839bfe61e9d7420750846afebda7d645fd`、
  `ev_1f62b0890bc0600d886cdc7b9e7a077edb85d59c2691b0f14710caf5086e8728`。
- `0x3174`只是初始化全局单命令请求，`0x30fc`再走`0x2cdc`命令set机制。
  当前观察的协议wrapper `0x1850`设置回调，`0x18ac`进入sleep，
  `0x18c4`退出sleep；不是三种“返回当前投票”的接口。
  `ev_db8f2a65ed10c303853aa93dcbe2934ae8f7c451073888f96479df0f2a40b6b3`、
  `ev_7d2c529f56e8c7cab38319474515bb42298635625e321069cad3284a78bf8920`。

推论：READ的返回值来自硬件response，不可与resource/DRV/set影子投票混为一谈。
尚无RPMh硬件规范或本机证据证明ARC READ response是本DRV旧request，而非
聚合/当前输出。因此不使用它填写power.prepare的current_cx/current_mx。
这轮调查缩小了未知范围，**没有解除投票继承或共享所有权阻塞**。
协议多个wrapper仍未被Ghidra定义为函数，其原始字节已读取留证，不能据此宣称
穷举所有公开接口；没有按image-relative地址扫描活RAM。
