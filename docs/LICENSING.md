# 许可证范围

本仓库由不同来源的源码组成，**不是一个可以用根目录LICENSE覆盖全部文件的单一授权项目**。对本次发布中由项目持有权利、此前未单独标注的实现，按下面的组件边界明确授权；第三方已有声明保持不变。

| 范围 | 授权 |
| --- | --- |
| 自有BlueTool、类型0适配库、补丁器、构建/安装/测试工具、文档/元数据 | BSD-3-Clause；根目录LICENSE及LICENSES/BSD-3-Clause.txt |
| GENI/HSP/IBS和本机电源流程适配：蓝牙probe/hci、Linux流程相关基线；既有共享clock/provider/ICC文件 | GPL-2.0-only；保留源文件已有版权与SPDX |
| XNU kernel-pipe传输与MAC credential权限适配 | APSL-2.0；LICENSES/APSL-2.0.txt及对应许可说明 |
| 补丁 | 按其修改的目标文件许可；横跨GPL/APSL组件的补丁是不同许可部分的集合 |

`REUSE.toml`与`kernel/**/*.license`补充逐文件授权元数据，让源码快照与原补丁保持逐字节可验证。此处的`AND`表示一个补丁文件包含需要分别遵守的多个部分，**不是可任意选择的双重授权，也不是两种许可证可链接组合的保证**。

BSD部分允许遵照三项条件再分发；GPL部分要遵守其对应源码等要求；APSL覆盖部分保留原声明并按其部署/修改条件提供源码。实际条款见随附完整文本，以及官方[BSD-3-Clause](https://opensource.org/license/bsd-3-clause)、[GNU GPLv2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)、[Apple APSL](https://opensource.apple.com/apsl/)。这些概括不替代许可证正文。

## 组合内核与第三方资产

本仓库公开的是源码集合与补丁，没有发布完整链接内核。GPL-2.0-only和APSL-2.0部分的最终组合/二进制再分发条件需要另行核对，不能仅凭本仓库采用开源许可证就宣称组合已获得统一授权。

以下材料不在授权范围内，也不在Git中：

- Apple的原始/修补bluetoothd、iOS镜像、kernelcache、SDK及提取的系统库。
- Qualcomm/Xiaomi固件、rampatch、NVM、校准或分区镜像。
- 设备唯一地址、密钥、配对数据库、凭据和个人日志。

构建者自行准备有权使用的本地输入；`.gitignore`把常见私有输入和生成物排除在发布路径外。补丁工具的源码授权不等同于对补丁输出中第三方内容的再分发授权。

贡献时沿用所修改组件的许可证；如果引入其他来源的代码，保留其版权和许可，并更新NOTICE/provenance。未经权利人许可，不应把GPL或APSL文件改标成BSD/MIT。
