# 发布快照与开发分支

本文件说明两个长期分支的职责和当前可验证状态。分支状态以 git log -1 --oneline <branch> 和 CI 结果为准。

## 分支职责

| 分支 | 职责 | 组件策略 | 可打包/刷写 |
| --- | --- | --- | --- |
| main | 公开发布快照、文档、CI | 只选择公开 catalog 组件；不选择 Adreno735/Sm8635Platform | 内存 live map 验证前不打包/刷写 |
| wip/ufs-smmu | GPU/Adreno735、DarwinBoot、Fastboot、GDSC/RSC 开发 | GPU 源码保留；私有组件暂时从可校验列表移除 | 同步内存修正并验证 live map 前不打包/刷写 |

## 发布快照 main

当前应包含：

- README 的 Current status / Evidence status；
- docs/evidence-status.md：current evidence 与 historical evidence 的区分；
- docs/memory-audit.md：2 GiB DRAM 窗口与 pram 迁移审计；
- porting.toml 中 historical device 结果降级为 implemented；
- 公开 catalog 组件集合；
- CI：Device documentation / Porting record 通过。

未包含：

- 私有 Adreno735 驱动选择；
- 私有 catalog 条目；
- GPU 开发分支的未发布实验。

## 开发分支 wip/ufs-smmu

当前应包含：

- Adreno735/GPU 源码、docs、evidence；
- docs/ATTRIBUTION.md 借鉴来源；
- 私有 catalog 注册补丁（本地/待上游合并）；
- GPU/Fastboot 历史实验记录。

当前限制：

- 仍可能保留旧内存值（owned_memory_size = 0x100000000、pram 0xa7000000）；
- 在同步 main 的 2 GiB/pram 修正并完成 live UEFI map 验证前，不能打包或刷写；
- CI 的记录检查通过不代表构建或设备验证通过。

## Catalog

公开 catalog 还没有 Adreno735/Sm8635Platform 时：

- main 不选择它们；
- wip 暂时从可校验组件列表移除；
- 等上游 catalog 合并后再恢复。

## 同步规则

1. 文档/发布改动先提交 main 并推送；
2. 只有明确需要进入开发线的改动才同步到 wip/ufs-smmu；
3. 内存修正必须先进入 wip 并通过 live map，再允许打包/刷写；
4. 不使用 force push 覆盖另一条分支；
5. 每次推送后检查对应分支的 CI。

## 当前阻塞

- live UEFI memory map 尚未抓到；
- wip 尚未同步 2 GiB/pram 修正；
- 公开 catalog 尚未合并 Adreno735/Sm8635Platform。

在以上三项完成前，两个分支都不得进入打包或刷写。
