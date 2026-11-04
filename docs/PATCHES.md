# 补丁索引

按父工程原编号保留，文件与SHA-256见[series.json](../patches/series.json)。当前运行系列共12个，全部按序应用；空缺编号是其他驱动工作，不需要凑齐编号。

| 编号 | 作用 | 主要边界 |
| --- | --- | --- |
| 0129 | GENI资源、SE7频率和QUP0时钟/ICC consumer | 同时修改共享provider；不是单独的UART文件即可替代 |
| 0130 | 探测映射改用可释放的XNU接口 | 修复对永久映射执行unmap的panic |
| 0133 | 电源、GPIO、UART DMA、SID与版本查询 | 启动依赖Wi-Fi保留共享rail votes |
| 0134 | 在XNU页大小下分配连续DMA内存 | 保持实际物理连续性与SMMU映射一致 |
| 0135 | 读取控制器board ID | 对应实际HSP NVM选择依据 |
| 0136 | rampatch/NVM/Reset固件链 | 保留原分段ACK语义与真实状态 |
| 0137 | UART.SK、H4/IBS与常驻工作线程 | 原生bluetoothd通过kernel pipe连接 |
| 0138 | 从ADT取本机地址并写入控制器 | 地址由用户私有输入提供，不内置设备地址 |
| 0140 | Skywalk user-pipe签名权限映射 | 支撑原生A2DP音频管道，不无条件授予权限 |
| 0141 | UART DMA IRQ640与事件驱动 | 替代1ms轮询，明确DMA/队列归属 |
| 0142 | HSP工作速度与主机/termios一致 | 名义3200000，按原厂频率表选择100MHz/2 |
| 0143 | 完成条件先于等待、独立匹配回复 | 修复成功回包仍等10秒及回复被后续事件覆盖 |

`patches/history/0140-renoir-bluetooth-uart-power-preflight.patch`来自父工程另一套早期XNU编号，只作溯源；它已包含在运行系列所要求的基础树内，**不能与本表的0140混同或再次应用**。

用户态变更不适合表示成针对专有可执行文件的巨大二进制diff，因此提供：

- `tools/patch-renoir-bluetoothd.py`：匹配原件后生成固定离线指令/加载命令修补。
- `userland/renoir_bt_type0.c`：真实能力映射、标准广播选择和重开适配。
- `userland/renoir_bluetool.c`：XPC服务与内核真实启动结果连接。
- `config/boot-contract.json`：ADT、固件资源与速度/地址约定。

补丁和最终源码快照是同一实现的两种表示，测试会核对一致性；不要把快照再覆盖到另一版本内核并假定已完成集成。
