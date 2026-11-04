# 构建、补丁与安装

## 1. 包的边界

本仓库是蓝牙源码/补丁集合。`kernel/base`仅提供12个补丁涉及的10个先决文件；应用后得到12个文件，并与kernel143相应文件哈希一致。它能独立验证补丁，不包含完整XNU板级移植或全部共享provider。

来源为父工程本地提交`161a1ba60f2d4528caf784b09730d6217a819ca1`。该值用于追溯导出，**不是承诺父仓库远端已有此提交**。本仓库自带所需补丁验证上下文，避免验证依赖尚未公开的父工程历史。完整内核构建仍需要持有配套Renoir工作区。

## 2. 无手机、无私有文件的验证

在仓库根目录执行：

```sh
python3 tools/check-release.py
python3 tools/check-patches.py
python3 tests/test_bluetooth_h4_parser.py
python3 tests/test_bluetooth_worker_sim.py
```

这些测试执行实际生产函数的主机模型，覆盖H4分片/IBS、工作线程事件顺序、真实失败期限和回复归属。其毫秒数来自模拟时钟，不是手机性能指标。CI仅运行公开源码测试，不下载Apple原件或固件。

## 3. 应用内核补丁

目标是已有Renoir基础移植、已应用父工程`gpu/kernel-series.json`中0001–0128的XNU树。早期`xnu/patches/0140`前置探测已经属于这个基线，`patches/history`只作历史参考。0131/0132为背光、0139为KeyStore，与本系列无关，不作为蓝牙补丁导出。

```sh
# 先验证：在临时目录逐个应用，原树不变。
python3 tools/check-patches.py --tree /path/to/renoir-kernel128

# 明确要求应用时，再修改这棵已有的完整树。
python3 tools/check-patches.py --tree /path/to/renoir-kernel128 --apply
```

补丁按`patches/series.json`排列；前后哈希固定。不要对vanilla XNU、kernel143或任意近似版本直接套用。`kernel/files`是最终源码参考，不能把两个新文件随意复制进去而跳过共享资源/权限补丁。

若需要从父工程工作区构造基线：从其已冻结的`linux-renoir/xnu-src/xnu-8796.141.3`复制一棵独立源码树，按固定父manifest依次应用0001–0128，随后执行上面的验证/应用。不要在当前正在使用的内核源码树上回退补丁。完成后按父工程的构建环境运行：

```sh
XNU_SRC=/path/to/complete-patched-xnu bash /path/to/parent-workspace/linux-renoir/xnu-build.sh -j8 -k
```

这里只说明对接入口，未声称独立导出仓库已重新完成整个内核编译。父工程kernel143此前已经完成构建和真机验证；本次发布验证的是源码一致性、补丁和主机测试。

## 4. 启动资源

准确约定见`config/boot-contract.json`，实际父工程入口是`src/tools/mkadt-renoir.py`和`workspace-tools/pack-star-gpu-boot.sh`。

- `/arm-io/bluetooth`：`device_type=bluetooth`、`compatible=qcom,qca6490-bt`、`transport-encoding=1`（H4）、`transport-speed=3200000`、真实6字节`local-mac-address`。
- 固件：本机`hpbtfw20.tlv`及`hpnv20.bin`，以启动资源`bt-hpbtfw20.tlv`和`bt-hpnv20.bin`提供。BID/NVM选择保持已审计的本机分支。
- 地址：自行从自己的设备保存到私有文件，父工程通过`RENOIR_BT_ADDRESS_FILE`传入。不提供共享示例地址，也不要把系统版本缓存中的默认占位当成本机地址。
- 启动依赖现有Wi-Fi资源所有者。父工程打包使用相应Wi-Fi/IO/radio-route设置，并设置`RENOIR_BT_FW_DIR`。独立BT供电尚未实现。

可选只读采集工具（需要用户自行进入recovery，ADB与mtools已安装）：

```sh
python3 tools/collect-firmware.py
```

该命令会实际读取连接手机的bluetooth分区，保存在忽略目录`private/firmware/`。它不刷分区、不重启；构建与CI不会调用它。Apple原件、SDK、固件和地址均由使用者自行准备且不上传Git。

## 5. 用户态构建

需要Linux上的Theos arm64 Apple工具链、ldid及iPhoneOS16.5编译SDK。运行目标是20H392，编译SDK版本不代表运行系统版本。

原件约束：

- 文件：未修改的20H392 `/usr/sbin/bluetoothd`。
- SHA-256：`8b7c0a0bea51544db82269d7f2775ab7996b8740927283428c4753e032df3a8f`。
- UUID：`AA9424E8-0648-34E2-B3CF-E6C793B9D471`。

```sh
# THEOS默认~/theos；RENOIR_IOS_SDK可以指定另一份兼容的本地SDK目录。
bash tools/build-userland.sh /path/to/original-20H392-bluetoothd
```

输出在`build/userland/`：自有BlueTool、RenoirBTType0.dylib、离线修补并重签的bluetoothd及哈希。后者保留原件entitlements。输出含Apple派生二进制，不作为本仓库Release附件发布。

可选广播原件测试（仅本地主机）：

```sh
python3 -m venv .venv
.venv/bin/pip install unicorn==2.1.4
.venv/bin/python tests/test_bluetooth_type0_advertising.py \
  --original /path/to/original-20H392-bluetoothd \
  --library build/userland/RenoirBTType0.dylib
```

运行真实ARM64原件和适配库的关键函数；OS/HCI输入由模型提供。覆盖旧2006反例、标准参数/数据/扫描响应/随机地址/启用、清理重开和错误回调。

## 6. 离线安装与恢复

需要已有配套kernel143和已挂载的20H392离线iOS根目录。下列命令会修改明确指定的挂载目录，由使用者主动执行：

```sh
sudo python3 tools/install-userland.py --root /mnt/renoir-ios --source build/userland
```

原件保存在目标根目录`/.renoir-bluetool-backup`。恢复：

```sh
sudo python3 tools/install-userland.py --root /mnt/renoir-ios --restore
```

工具检查原件备份和构建哈希，不挂载分区、不切换slot、不刷boot、不重启。安装后按自己现有的Renoir启动流程卸载文件系统并启动；此仓库不改变引导方案。
