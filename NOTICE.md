# 来源、归属与致谢

此仓库是[daimile2/ios-renoir](https://github.com/daimile2/ios-renoir)中蓝牙工作的独立源码整理；该移植建立在[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)及XNU板级适配工作之上。保持原文件的版权/SPDX声明，不因拆分仓库而删除上游归属。

来源提交：`161a1ba60f2d4528caf784b09730d6217a819ca1`。内核补丁系列到0143；类型0广播修复为`7d6eb57e80ae07bb84046306e387308e8e5da626`。来源提交是本地导出身份，不保证父工程远端具有全部后续提交。`provenance.json`逐项记录源路径/哈希及仅为独立仓库修改的路径。

| 上游 | 固定版本与使用内容 |
| --- | --- |
| [Apple XNU](https://github.com/apple-oss-distributions/xnu/tree/xnu-8796.141.3) | XNU 8796.141.3，内存映射、调度、kernel pipe、credential/MAC接口；APSL及各文件原许可 |
| [MiCode内核](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/a9244def8a5178edee9ed4a30d60034de8286cbb) | star-r-oss固定a9244def；本机GENI UART、GCC/ICC、btpower流程；GPL-2.0及各文件声明 |
| [Linux HSP蓝牙](https://github.com/torvalds/linux/tree/05441d697f7f9a927264f4058cbbc6a03dfcac9d/drivers/bluetooth) | hci_qca/btqca固件与IBS协议流程，GPL-2.0-only |
| [MiCode设备树](https://github.com/MiCode/kernel_devicetree/tree/star-r-oss) | 结合本机实际DT核对renoir资源，不能以共享分支名替代设备型号 |
| [Bluetooth Core HCI](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/host-controller-interface-functional-specification.html) | 标准HCI功能及新旧广播命令族规则；仅链接规范，没有复制规范正文 |
| [Unicorn](https://github.com/unicorn-engine/unicorn) | 可选本地ARM64原件函数执行测试，2.1.4；不随仓库附带引擎二进制 |

共享GPL源码中的既有声明包括The Linux Foundation和ARM Limited，保留在相应文件内。厂商驱动作为参考来源，不表示本机Android实际启用了Linux hci_qca；本机串口分支与HSP固件协议来源分别记录。

Apple bluetoothd的SHA-256/UUID用于本地版本匹配；本仓库没有包含该程序的完整或修改后二进制。ABI说明和补丁器不会把Apple程序、SDK、厂商固件变为本项目可授权的代码。

维护与设备验证：daimile2及贡献者。具体源码归属以保留的头部、逐文件许可与Git历史为准；不替第三方增加授权，也不声明任何厂商背书。
