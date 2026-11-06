# Panic 后自动回 fastboot：可行路线与当前状态

2026-11-06用户询问。这里只核查源码，没有开启自动复位、写PMIC或重启手机。
不恢复此前取消的USB命令端点。

## 为什么当前会停在panic屏幕

XNU `osfmk/kern/debug.c` 有panic后重启路径和`DB_REBOOT_ALWAYS=0x100000`。
当前debug=0xc没有该位，且SWEET启用了panic调试。更关键的是
`SweetPlatformExpert`没有haltRestart覆盖，`IOPlatformExpert::haltRestart`
最终需要`PE_halt_restart`，当前没有平台后端。因此只加debug=0x10000c也
不足以实现真正复位，更不会自动选择Android fastboot模式。

## 所需的两个独立环节

1. **物理复位**：XNU平台panic restart callback，直接调用PSCI
   SYSTEM_RESET(0x84000009)，或实际支持且验证过的SYSTEM_RESET2。
   UEFI绑定的ArmPsciResetSystemLib表明固件使用PSCI，但它仍不是本项目
   XNU runtime后端。PSCI CPU_ON已通过也不等于SYSTEM_RESET已测。
2. **选择bootloader**：SM8150 Android `msm-poweroff.c` 的bootloader分支
   写IMEM `0x146bf65c = 0x77665500`，另设置PM8150 PON bootloader reason。
   GEN2 `PON_SOFT_RB_SPARE` 是base+0x8f，reason bits7:1；不能只写IMEM就
   假定ABL必进fastboot。warm/cold reset保留语义、PMIC subtype、dload/EDL
   标志与bootloader选择都需独立确认。

## 不应采用的方式

- 不在panic中用可能已锁死的普通SPMI mutex进行无界请求。
- 不依赖已退出的UEFI BootServices/DXE reset函数指针。
- 不为了回fastboot清全部PMIC位、写PS_HOLD或启用EDL/watchdog试写。
- 不声称panic自动复位涵盖完全硬件锁死；CPU已无法执行callback时仍需按键。
- panic属于异常复位，不能承诺正在写入的文件系统数据完整或自动log持久化。

## 推荐后续独立实验

先在正常线程上下文验证受限的bootloader-reason + PSCI复位，证明真机回到
fastboot，再使用已预映射、无分配/无睡眠/有界且不依赖普通锁的panic callback。
默认关闭，留下保存日志的窗口，发生复位失败则仍显示panic而不循环试写。
确认机制前，不与下一次内存回归混合启用，以免丢失panic归因/日志。

因此答案是：**可实现，但当前镜像没有这个可靠功能，不是一个boot参数即可补齐。**
现在保存照片/UDP日志后仍使用物理电源+音量减回fastboot；不自动启动失败镜像。
