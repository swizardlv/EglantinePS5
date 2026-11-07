# nxnative-poc skeleton — 部署步骤（PPSA98722，走 U 盘，不走网络）

TITLE_ID：`PPSA98722`（用户 2026-11-07 08:10 确认）。绝不用 PPSA98515。
部署方式：用户 2026-11-07 08:12 确认——没有固定时间，准备好就上；**不走网络部署**
（不用 `make install` / PS5_HOST）。编好的 title 拷回 USB 硬盘，用户自己把 U 盘接到 PS5 上跑。

## 0. 前置（已做完的不要重做）

- 253 上编过：`nxnative-poc/skeleton/build-skeleton.sh`（容器内跑），产物在
  `nxnative-poc/skeleton/build/PPSA98722/`（eboot.bin、sce_sys/param.json、
  sce_sys/icon0.png、sce_module/libc.prx）。构建日志见 tmux `coffee-order`
  窗口 `phase1-1-skeleton`。
- 不碰 09:23 主线部署（build56）：不动 cron、不动 build56 工作树、不向 PS5
  发任何网络请求。

## 1. 确认 U 盘还在 253 上（拷前必做）

```sh
lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINT /dev/sdb
# 预期：sdb ASMedia AS2235 2TB；sdb3 exfat Data 分区，未挂载。
# 若 /dev/sdb 不存在：停下，不要改走网络部署，等用户重新插上再汇报。
```

## 2. 可写挂载（之前是只读挂载，那样拷不进去）

```sh
sudo mkdir -p /mnt/usb_drive
sudo mount -t exfat -o rw /dev/sdb3 /mnt/usb_drive
mount | grep usb_drive   # 确认 rw
ls /mnt/usb_drive/       # 应看到 games/ 等原有内容；不要动它们
```

## 3. 拷贝

```sh
STAGE=/data/code/EglantinePS5-nxnative-poc/nxnative-poc/skeleton/build/PPSA98722
sudo rm -rf /mnt/usb_drive/PPSA98722
sudo cp -a "$STAGE" /mnt/usb_drive/PPSA98722
```

目标是 U 盘根目录的 `PPSA98722/`（对照 PS5 上 Eglantine 的位置是
`/mnt/usb1/PPSA98515`，即 U 盘根目录的 `PPSA98515`）。

## 4. 校验（拷完必做）

```sh
diff <(cd "$STAGE" && find . -type f | sort) <(cd /mnt/usb_drive/PPSA98722 && find . -type f | sort)
# 文件清单一致；再抽查关键文件 sha256：
(cd "$STAGE" && sha256sum eboot.bin sce_sys/param.json) > /tmp/ppsa98722-stage.sha256
(cd /mnt/usb_drive/PPSA98722 && sha256sum -c /tmp/ppsa98722-stage.sha256)
```

## 5. 卸载，通知用户

```sh
sync
sudo umount /mnt/usb_drive
lsblk -o NAME,MOUNTPOINT /dev/sdb   # 确认已卸载
```

然后汇报“可以拔盘了”。用户把 U 盘接到 PS5，ShadowMountPlus 里应出现
`PPSA98722`（nxnative-poc），启动后：

- 屏幕应为纯蓝色（0.05, 0.35, 0.85）约 2 秒（120 帧），随后 title 退出。
- **不要让用户复述他看到了什么来定位问题**：日志已写在 title 自身目录下，
  随 U 盘带回（见下）。

## 6. 取回日志（用户把 U 盘插回 253 后）

```sh
sudo mount -t exfat -o ro /dev/sdb3 /mnt/usb_drive
ls -l /mnt/usb_drive/PPSA98722/
cp /mnt/usb_drive/PPSA98722/nxnative-poc*.txt /mnt/usb_drive/PPSA98722/nxnative-poc*.csv /tmp/  # 按需
sudo umount /mnt/usb_drive
```

日志文件（都在 `PPSA98722/` 下，即 title 自身目录 `/app0`）：

| 文件 | 内容 | 格式 |
|---|---|---|
| `nxnative-poc.txt` | 全量运行日志（klog 同步一份） | 每行 `[nxnative-poc] ...`，PASS/FAIL 行 + 各步骤 rc |
| `nxnative-poc-va.txt` | VA 探针专档 | 首行 verdict；每行一次 reserve：`reserve label=... size_mib=... rc=... base=... in_range=...`；commit/decommit 各一行 |
| `nxnative-poc-heap.csv` | heap 时间序列 | 列：`tick,phase,pool_total_bytes,guest_reserved_bytes,guest_committed_bytes,radv_heap_bytes,largest_free_lb_bytes,largest_free_ub_bytes,free_bytes`；phase 覆盖 startup / va-reserved / va-committed / va-decommitted / radv-init / present（每 30 帧）/ exit |

判读要点：

- VA：`verdict: PASS single=1 ...` = 单次 9.75 GiB 成功；`single=0 split=1` =
  单次失败、拆两段成功（两段各一行 reserve 记录）；`single=0 split=0` = 全失败。
- heap：`free_bytes=unknown` 是诚实的——PS5 内核没有查空闲内存的接口，
  只有 pool 总量；`largest_free_lb/ub` 是有界探针给出的最大单块上下界。
- RADV：`frame 0: vkAcquireNextImageKHR / vkQueueSubmit / vkQueuePresentKHR`
  三行 rc 定第一帧成败；`presented=N/120` 是总数。失败时看 first_failure_frame
  与 rc，不要问用户。

## 7. 回滚

删 U 盘上的 `PPSA98722/` 即可；主线 PPSA98515 与游戏 U 盘内容完全不受影响
（不同 title、不同目录）。
