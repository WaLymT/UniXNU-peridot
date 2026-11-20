# 证据状态：历史曾验证 vs 当前 main 可复现

检查对象：`main` 的跟踪代码与配置，基线提交 `51cc1fc`。

## 判定方法

对 `porting.toml` 中每条带回溯 revision 的 evidence：

1. revision 必须是当前仓库可见的 40 位 commit；
2. revision 必须是 `main` 的祖先；
3. `git diff --quiet <revision> main -- <area.paths>` 必须为空。

同时满足三条记为 **current**；否则记为 **historical**。  
该检查只证明“记录对应的文件在当前 main 仍一致”，不等于“当前 main 已重新构建并刷机复现”。

## 结果

| Area | 记录状态 | current evidence | historical evidence | 解释 |
| --- | --- | ---: | ---: | --- |
| configuration | implemented | 0 | 1 | 配置来源，无设备证据 |
| firmware | implemented | 0 | 2 | p478/p491/p538/p539 等设备观察属于历史 |
| cpu | device-verified | 1 | 1 | 当前路径仍有 evidence；另一条为历史 |
| memory | device-verified | 1 | 1 | 同上 |
| storage | implemented | 0 | 23 | APFS/UFS 设备证据均早于当前 main |
| display | device-verified | 1 | 0 | 当前路径一致 |
| gpu | implemented | 1 | 1 | 私有 Adreno735 不在 public main 组件列表 |
| packaging | implemented | 0 | 2 | 仅有历史 host 证据；当前 CI 通过不等于设备构建 |
| diagnostics | implemented | 0 | 2 | 历史设备日志证据 |
| userspace | device-verified | 3 | 1 | 当前路径仍有 3 条证据 |
| input | planned | 0 | 0 | 无证据 |
| usb | planned | 0 | 2 | 仅有来源引用，无设备证据 |
| network/audio/power/cellular | planned | 0 | 0 | 无证据 |

## 状态规则

- `device-verified`：至少一条 device evidence 与当前 tracked paths 一致；
- `host-verified`：至少一条 host evidence 与当前 tracked paths 一致；
- `implemented`：代码/配置存在，但没有当前可复现证据；
- `planned`：没有实现或没有证据。

## 注意

- main 不选择 `Adreno735`/`Sm8635Platform`；GPU 私有工作保留在 `wip/ufs-smmu`。
- CI 只运行 porting-record 结构检查，不执行固件构建、fastboot boot 或设备刷写。
- p538/p539 的 XNU/Fastboot 结果是历史设备观察；要升级为“当前 main 可复现”，必须在当前 main 重新构建、刷入并记录新 revision。
