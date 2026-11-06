# Eden 离线预处理包（Offline Pack）

设计文档见 `docs/offline-pack-design.md`（权威版：捕获 → 打包 → PS5 加载分支、包格式、实测、已知限制）。

本文件曾是一份独立设计（原 v1 二进制包方案 `edenpack`，`caaabb714a`），
2026-11-07 已废弃：并行工作建成了文件包（manifest.json + 每 level 一个
.bc 文件、按 guest 地址 key）并补齐了 PS5 加载分支，定稿为文件包；
二进制包（`offline_pack.h`）已删除。旧方案见 git 历史。
