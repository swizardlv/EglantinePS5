# nxnative PoC — Phase 1 lane 1: skeleton title + RADV solid colour

- 工作树：`/data/code/EglantinePS5-nxnative-poc`，分支 `jarvis/nxnative-poc`，
  从 build55 头分出（父仓库 `cfbcc19792` / 子模块 `08598c9984`）。
- TITLE_ID：`PPSA98722`（用户 2026-11-07 08:10 确认）。绝不用 PPSA98515。
- 部署：走 U 盘拷贝（用户 2026-11-07 08:12），不走网络。见
  `skeleton/deploy-steps.md`。
- 日志：三个验证点（VA 探针 / heap 对账 / RADV 初始化与点亮）全部记日志，
  写到 title 自身目录（`/app0` = U 盘上的 `PPSA98722/`），随 U 盘带回。
  用户要求（2026-11-07 08:13）：一次上机跑完靠日志定位问题，不要反复折腾他。

## 目录

- `skeleton/skeleton.c` — title 主程序：VA 探针 + RADV 初始化 + 纯色 present
  + heap 时间序列。退出码：0 = 探针通过且纯色已呈现；1 = 探针已记日志但
  呈现失败；2 = RADV 初始化失败。
- `skeleton/param.json` — PPSA98722 的 param（titleName `nxnative-poc`）。
- `skeleton/build-skeleton.sh` — 构建脚本（在 eglantineps5-builder 容器内跑；
  复用 mihawk RADV 归档 + 链接配方，只读引用，不改共享 checkout）。
- `skeleton/build/PPSA98722/` — 构建产物 staging（eboot.bin 等），部署时原样
  拷到 U 盘根目录 `PPSA98722/`。
- `skeleton/deploy-steps.md` — 部署 + 取日志步骤清单。

## 状态（见子报告）

子报告：`~/workspace/goals/eglantineps5/hidden_files/phase1-1-skeleton-radv-2026-11-07.md`
