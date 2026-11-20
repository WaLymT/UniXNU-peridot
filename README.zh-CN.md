# UniXNU peridot

小米 Redmi Turbo 3 / POCO F6（代号 `peridot`，高通 SM8635 "cliffs"）的
[UniXNU](https://unixnu.com) 社区设备仓库，目标是在真机上运行 iPadOS 17.7.11
（21H461）。

> English: [README.md](README.md)

## 当前状态

这是进行中的社区移植，不是官方产品。已在真机验证的阶段：

- UEFI：Mu-Silicium `peridotPkg` 固件可启动并显示菜单；
- 内核：XNU 启动到帧缓冲控制台，SMP、GIC、PMGR、panic 屏可用；
- 存储：原生 UFS 传输与 GPT 可读，bdevsw 根盘可用，APFS 可挂载 iPadOS
  System 容器并执行 `/sbin/launchd`（p368、p383）；
- 安装：`newfs_apfs` 可格式化 UFS 安装目标（p372-p381、p401）；
- SHNGROOT：官方 `unixnu build userdata` 镜像可刷入，OS 窗口作为根挂载并启动
  `launchd`（p418）。

尚未完成：dyld shared cache（SystemOS cryptex 尚未 graft）、可写 Data 卷、
输入/网络/USB/蜂窝/音频/电源以及 GPU。详见 [docs/STATUS.md](docs/STATUS.md)
与 [porting.toml](porting.toml)。

## 构建

在 UniXNU 工具树中与本仓库并排使用：

```sh
cp <device-checkout>/local.toml.example <device-checkout>/local.toml
./unixnu defconfig peridot_defconfig -D ../UniXNU-peridot
./unixnu fetch -D ../UniXNU-peridot
```

然后按 UniXNU 文档执行 kernel/package/firmware/payload/boot image 步骤。
内核与 payload 需要本地的 kernelcache、trust cache 与 device seed，它们不在
本仓库中；SM8635 共享支持位于配套仓库 `UniXNU-sm8635-common`（见
[feeds.toml](feeds.toml)）。

## 不包含的内容

Apple 固件/IPSW、签名的 trust cache 与 root hash、cryptex 元数据、厂商 GPU
固件、构建产物和日志均不进入本仓库。说明见
[docs/proprietary.md](docs/proprietary.md)。

## 许可

BSD-2-Clause-Patent，见 [LICENSE](LICENSE)。本项目与 Apple、Qualcomm 及
UniXNU 组织无隶属关系。
