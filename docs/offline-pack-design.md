# Eden 离线预处理包（offline pack）——设计文档（原型 v1）

- 状态：原型，已提交并推送（子模块 `5921ae1952`、父仓库 `7ccec3b239`，
  分支 `jarvis/offline-pack`），未部署。文件留在独立工作树
  `/data/code/EglantinePS5-offline-pack`。加载器已实现（§8.2）。
- 基底：父仓库分支 `jarvis/offline-pack` @ `cfbcc19792`（build55），
  子模块 Eglantine 分支 `jarvis/offline-pack` @ `08598c9984`。
- 日期：2026-11-07。主工作树 `/data/code/EglantinePS5`（build55 主线）
  未动；09:23 部署 build54+55 的定时任务未动；未向 PS5 部署任何东西。
- 本文档所有行号均指上述基底版本，核实于 2026-11-07。

## 0. 一句话

包 = **运行时捕获到的 ASTC 纹理，离线转码成 BC1/BC3** + **预热好的
shader/pipeline 磁盘缓存**。PS5 运行时直接用包里的 BC 数据和缓存，
跳过每纹理的 ASTC 解码 + BC 重编码 dispatch 和首次 shader 编译。

**本包不解决的问题（先说清楚）：** build52 时之笛 1122 秒闪退、
heap1 5530/6035 MiB（92%）的内存增长源头仍未知；本包只去掉转码的
重复工作，不声称修 OOM、不声称提帧率（见 §7）。

## 1. 对原任务的三处纠正（已核实）

### 1.1 PPTC 不存在——(c) 项改写为真实等价物

`rg -i pptc Eglantine/src` **零命中**。PPTC 是 Ryujinx 的概念，
Eden/Yuzu 系没有。原型 (c) 项改为真实等价物：

- **预热的磁盘 shader + pipeline 缓存文件**（已存在、可直接复用，
  见 §5）：`vulkan.bin`（预编译 pipeline 缓存）+
  `vulkan_pipelines.bin`（Vulkan 驱动 pipeline 缓存）。
- **Dynarmic JIT 代码缓存不能持久化**：`src/core/arm/dynarmic/` 内
  `rg -i "serializ|SaveCache|LoadCache|DumpCache"` 零命中；只有容量配置
  （`arm_dynarmic_64.cpp:286-299`：`code_cache_size` 128/512 MiB，
  32 位 `arm_dynarmic_32.cpp:194-207` 同理）。JIT 缓存持久化需要新做，
  不在本原型内。

### 1.2 NUP 输入不存在——纹理枚举只能走“运行时捕获 + 离线转码”

`find /data -iname "*.nup"` **零命中**；时之笛 dump 同样找不到。
NUP 在 PS5 U 盘 `/mnt/usb1/games/`，游戏纹理在包内档案里，运行时才进
guest 显存。**不存在“直接从 NUP 拆出全部纹理”的流程**，原型按两阶段设计：

1. **捕获**：`EDEN_OFFLINE_CAPTURE=1` 的一次 PS5 运行中，
   `UploadImageContents` 命中 ASTC 上传时记 manifest、可 dump 原始字节。
2. **离线转码**：`eden_offline_pack.py` 读 manifest + dump，输出包。

### 1.3 BC3 并不比 ASTC 省显存——转码的收益在别处

ASTC 块恒为 16 字节，BC3 块也是 16 字节，BC1 块 8 字节。
每 texel 字节数（`src/video_core/surface.h` 的块尺寸表）：

| 格式 | B/texel |
|---|---|
| ASTC 4×4 | 1.00 |
| ASTC 6×6 | 0.44 |
| ASTC 8×8 | 0.25 |
| BC3 | 1.00 |
| BC1 | 0.50 |

离线转 BC3 后，4×4 不变，6×6 变大 2.25 倍，8×8 变大 4 倍；
只有不透明纹理走 BC1（4×4 减半）才省。E2E 实测也证实了这一点
（§6.3：ratio 1.19）。**转码的真正收益是去掉运行时的解码 + 重编码
工作和瞬时中间缓冲，不是省显存**（§7）。

## 2. 包格式 v1

### 2.1 布局

```
offline-pack-v1/
  manifest.json                 # 包清单（见 2.2），version: 1
  textures/
    <gpu_addr16hex>_L<level>.bc1|.bc3   # 每纹理每 mip 一文件，裸块流，无头
  shaders/
    vulkan.bin                  # 预编译 pipeline 缓存（CACHE_VERSION 19）
    vulkan_pipelines.bin        # Vulkan 驱动 pipeline 缓存
```

纹理文件是裸块流（BC3 每块 16 字节、BC1 每块 8 字节，行内紧排），
与捕获 dump 的约定一致，便于加载器直接送上传路径。

### 2.2 manifest.json 字段

顶层：`version` (=1)、`created_unix`、`capture_dir`、`tool`、
`entries[]`、`stats{}`。每条 entry（源自捕获 manifest.jsonl 的一行，
见 §3.3）：

| 字段 | 含义 |
|---|---|
| `gpu_addr` | guest 地址（hex 字符串，包内键） |
| `format` / `format_name` | PixelFormat 枚举值 / 名（见 2.3） |
| `width/height/depth/levels/layers/type` | ImageInfo 原值 |
| `guest_size_bytes`、`host_level_offset` | ImageBase 原值 |
| `astc_opaque` | 是否全不透明（决定 BC1/BC3，见 2.3） |
| `layout` | `linear` / `swizzled` / `swizzled-tail` |
| `dump` / `dump_bytes` | 捕获 dump 路径 / 字节数 |
| `status` | `ok` / `skipped-…` / `error-…`（见 2.4） |
| `levels_out[]` | 每 level：`level/width/height/file/bytes/sha256/codec` |

`stats`：`manifest_entries/transcoded/skipped/errors`、
`astc_bytes_in/bc_bytes_out/ratio_bc_over_astc`、
`transcode_ms_total`、`peak_child_rss_kib`（均为本次 build 实测）。

### 2.3 转码规则

- 格式表：28 个 ASTC 格式的枚举值 → (块宽, 块高)，从
  `src/video_core/surface.h` 的 `PIXEL_FORMAT_LIST` 按枚举顺序生成
  （工具内嵌该表，注释写明了生成命令；surface.h 变了要重生成）。
  例：33 = ASTC_2D_4X4_UNORM，78 = ASTC_2D_6X6_UNORM。
- `astc_opaque=true` → BC1（8 B/块），否则 BC3（16 B/块）。
  与运行时同一规则（`texture_cache.h` 约 2420 行的 opaque 扫描注释：
  "Opaque ASTC goes to BC1 rather than BC3"）。
- Mip 链：dump 按 level 顺序紧排，每 level 字节数 =
  `ceil(w_l/bw) * ceil(h_l/bh) * 16`，`w_l = max(1, w>>l)`；
  长度与 `dump_bytes` 不符的 entry 报错，不静默截断。

### 2.4 跳过规则（v1 范围外，如实记入 manifest，不硬做）

- `layout != linear`（`swizzled` / `swizzled-tail`）：离线反 swizzle
  未实现。注意 PS5 默认走 GPU 加速上传分支，staging 里正是 swizzled
  字节——**捕获到的多数 entry 在 v1 会是 swizzled 而被跳过**，
  这是 v1 最大的已知缺口（见 §8）。
- `layers != 1` 或 `depth != 1`：多层/3D 未实现。
- 无 dump（只开了 manifest 未开 dump）：跳过。
- 未知 format：跳过。

## 3. 离线三步流程

### 步骤 1 — 捕获（PS5 上跑一次游戏）

无需改代码。往 `dev-settings.txt` 加三行（`headless/main.cpp:457-458`
读该文件，`:833` 的 `env=NAME=VALUE` 条目会 `setenv`，在设备创建前
生效——与 `RADV_DEBUG` 等现有开关同一机制）：

```
env=EDEN_OFFLINE_CAPTURE=1
env=EDEN_OFFLINE_CAPTURE_DUMP=1
env=EDEN_OFFLINE_CAPTURE_DIR=/mnt/usb1/offline-capture
```

跑游戏（标题画面起步，尽量多逛）。退出后从 U 盘取回：

```
offline-capture/
  manifest.jsonl              # 每 ASTC 上传一行 JSON
  textures/<gpu_addr16hex>.astc
```

同时这趟运行也会写出预热好的 shader 缓存（§5）。

### 步骤 2 — 离线转码（253 上）

```bash
# 1) 编一次转码器（Eden 自己的编解码代码，见 §4）
tools/offline-pack/texcode/setup-deps.sh   # 取 boost/fmt 头文件 + astcenc（无 sudo）
tools/offline-pack/texcode/build-texcode.sh
# 2) 打包
python3 tools/offline-pack/eden_offline_pack.py \
  --texcode tools/offline-pack/texcode/build/eden_texcode \
  build --capture-dir <capture> --out <pack>
# 3) 校验（重编码比对哈希 + 往返 PSNR）
python3 tools/offline-pack/eden_offline_pack.py \
  --texcode tools/offline-pack/texcode/build/eden_texcode \
  verify --pack <pack> --capture-dir <capture>
# 4) 自测（无真实纹理时，合成图 + 真实 ASTC 块）
python3 tools/offline-pack/eden_offline_pack.py \
  --texcode tools/offline-pack/texcode/build/eden_texcode \
  selftest --workdir /tmp/offline-pack-selftest
```

### 步骤 3 — 加载（未实现，见 §6.5）

加载器改动是下一步工作，不在本原型内。包的消费方式在 §6.5 列出。

## 4. 捕获开关：改了什么（精确位置）

### 4.1 新文件：`Eglantine/src/video_core/texture_cache/offline_capture.h`

零 Eden 依赖（只用 `<std>` 头），可独立编译测试。内容：

- `VideoCommon::OfflineCapture::RecordAstcUpload(...)`：写 manifest +
  可选 dump。手写 JSON（字段全是数字/hex/固定 token，无需转义），
  `std::ofstream` 写字节，无第三方库。
- 环境变量（首次调用时读一次）：`EDEN_OFFLINE_CAPTURE=1`（总开关）、
  `EDEN_OFFLINE_CAPTURE_DUMP=1`（同时 dump 原始字节）、
  `EDEN_OFFLINE_CAPTURE_DIR`（默认 `offline-capture`，PS5 上用绝对路径）。
- 语义：每 guest 地址只记首次上传（`unordered_set` 去重）；全部文件
  操作包在 try/catch 里，失败静默——**捕获绝不能打断游戏**；
  未开启时每次调用只是一次静态 bool 检查就返回。

### 4.2 改动：`Eglantine/src/video_core/texture_cache/texture_cache.h`

`UploadImageContents`（第 1821 行）是全部 guest→host 上传的唯一漏斗，
四个分支各加一处调用（均以 `IsPixelFormatASTC(image.info.format)` 守卫，
共 +57 行，另 +1 行 include）：

| 分支 | 行（基底） | layout | dump 的字节 |
|---|---|---|---|
| AcceleratedUpload 且 `host_level_offset != 0` | ~1826 | `swizzled-tail` | staging：各层 mip 链尾部（swizzled） |
| AcceleratedUpload | ~1845 | `swizzled` | staging 全部（swizzled） |
| Converted（CPU 解码路径） | ~1855 | `linear` | `unswizzle_data_buffer`：线性 ASTC 块流 |
| 普通上传 | ~1873 | `linear` | `mapped_span`：线性 ASTC 块流 |

未开启时正常路径只多一次 `IsPixelFormatASTC` 判断（仅 ASTC 上传），
行为不变。应用补丁：工作树根目录 `apply-capture-patch.py`
（锚点精确匹配，任一锚点出现次数 != 1 即中止不写）。

### 4.3 相关锚点（已核实，供复核）

- ASTC→BC3 运行时转码：
  `Eglantine/src/video_core/renderer_vulkan/vk_texture_cache.cpp`
  `UseBc3ForSwitch2OnSmallCards()`（1213 行；`EDEN_NX2_KEEP_ASTC=1`
  时保留 ASTC，1221 行），转码走 `astc_decoder_pass`
  （`src/video_core/host_shaders/astc_decoder.comp`）+
  `bc_encoder_pass`（`bc_encoder.comp`）；
  `EDEN_NS2_MAX_TEXTURE`（342/383 行）、
  `EDEN_ASTC_PATH_DIAG`（2279 行，ASTC 路径诊断日志）。
- CPU 编解码（转码器复用的 Eden 自带逻辑）：
  `src/video_core/textures/astc.h`（`ASTC::Decompress`，FasTC）、
  `src/video_core/textures/bcn.h`（`BCN::CompressBC1/CompressBC3`，
  底层 `externals/stb/stb_dxt.h`）、
  `src/video_core/texture_cache/decode_bc.h`（`DecompressBCn`，
  底层 `externals/bc_decoder`）。
- ASTC 格式表：`src/video_core/surface.h`（`PIXEL_FORMAT_LIST`；
  `DefaultBlockWidth/Height` 169/178 行；`IsPixelFormatASTC` 216 行；
  `GetASTCBlockSize`、`TranscodedAstcSize`）。

## 5. Shader / pipeline 缓存：现成机制与打入包的方法

磁盘缓存位置与格式（已核实）：

- `PipelineCache::LoadDiskResources(u64 title_id, …)`
  （`src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:635`）：
  `base_dir = GetEdenPath(ShaderDir)/"{title_id:016x}"`（650 行），
  `vulkan.bin`（655 行，Eden 自有管线缓存，经
  `VideoCommon::LoadPipelines`，`src/video_core/shader_environment.cpp:862`）。
  **纠正**：它存的是 shader **环境**（guest 代码 + 状态），不是编译产物；
  启动时 `LoadPipelines` 在 worker 线程上重建全部管线。作用是把编译从
  “首次遇到时现编”移到“启动时批量做”，不是消除编译。真正跳过驱动侧
  编译的是 `vulkan_pipelines.bin`（驱动缓存）。
  + `vulkan_pipelines.bin`（657–660 行，驱动 pipeline 缓存）。
- `CACHE_VERSION = 19`（`vk_pipeline_cache.cpp:71`）。
  文件头：8 字节 magic `"yuzuvkch"` + u32 版本
  （`shader_environment.cpp` 的 `LoadPipelines`）；**版本不符时加载器
  会删掉该文件**——包里的缓存必须与构建的 CACHE_VERSION 一致。
- 退出时落盘：`~PipelineCache` 序列化 `vulkan_pipelines.bin`
  （590–593 行）；`vulkan.bin` 由序列化线程写（128 条 pipeline /
  30 秒一刷，见 744/756/790 行附近）。崩溃或硬断电可能丢尾巴——
  预热运行必须正常退出游戏。
- Linux 路径：`${XDG_CACHE_HOME:-~/.cache}/eden/shader/<title_id>/`
  （`EDEN_DIR "eden"`、`SHADER_DIR "shader"`，
  `src/common/fs/fs_paths.h:11,30`；`path_util.cpp:137-138,175`）。
  若可执行文件旁有 `user/` 目录则 portable 模式优先
  （`PORTABLE_DIR "user"`，`path_util.cpp:135-141`）。
- PS5 路径：发布包内无 `user/` 目录；PS5 数据目录为
  `/data/eglantineps5`（`headless/storage_paths.h:18,63`：
  `kDataDir`、`UserDir() = /data/eglantineps5/user`），
  因此最可能是 `/data/eglantineps5/user/cache/shader/<title_id>/`——
  **待一次真实 PS5 运行确认**，不要当成已证实。
- `vulkan_pipelines.bin` 是驱动 blob，只对同一 GPU/同一驱动构建有效；
  `vulkan.bin` 是 Eden 自己的预编译缓存，可随包走。

打入包：`tools/offline-pack/warm_caches.sh`（`--title-id` 自动找
Linux 缓存目录，或 `--shader-dir` 直接指定 PS5 拉回来的目录；
校验 magic + 版本 19 后拷到 `<pack>/shaders/`）。
预热本身不是脚本做的：跑一次游戏、正常退出，文件自然产生。

## 6. 实测（253 上，2026-11-07，全部标明来源）

### 6.1 捕获开关：独立编译 + 行为测试（实测）

`offline_capture.h` 用 `g++ -std=c++20 -Wall -Wextra -Werror` 独立编译
通过；测试 harness（`/tmp/offline-pack-deps/capture-harness/`，未进树）
模拟 `texture_cache.h` 的调用方式：

- manifest.jsonl 两行正确（含 `gpu_addr/format/宽高/levels/layers/
  astc_opaque/layout/dump/dump_bytes`），同一地址二次上传被去重；
- dump 文件字节与输入逐字节一致；
- 未设置 `EDEN_OFFLINE_CAPTURE` 时不创建任何输出。

### 6.2 转码正确性：合成图 → 真实 ASTC → Eden 往返（实测）

`eden_offline_pack.py selftest`（合成 TGA：gradient/checker/noise/
alpha；astcenc 4.7.0 编成真实 ASTC 块；`eden_texcode` 做
ASTC 解码 → BC 编码 → BC 解码；PSNR 相对 ASTC 解码值）：

| 用例 | BC | PSNR_RGB | max_err |
|---|---|---|---|
| gradient 256² ASTC 4×4 | BC3 | 46.82 dB | 4 |
| gradient 256² ASTC 4×4 | BC1 | 46.82 dB | 4 |
| checker 256² ASTC 4×4 | BC3/BC1 | 35.06 dB | 32 |
| alpha 256² ASTC 4×4 | BC3 | 42.32 dB | 4（alpha 99.00 dB） |
| gradient 512² ASTC 8×8 | BC3 | 47.79 dB | 5 |
| checker 512×256 ASTC 6×6 | BC3 | 36.01 dB | 57 |
| noise 256² / 128² | BC3/BC1 | 15.04 / 15.41 dB | 189 / 176 |

- noise 是 stress 用例：每像素随机色，BC 每 4×4 块只有 4 色板，
  15 dB 是编解码器的理论极限表现，不是链路 bug（同链路在
  gradient/checker/alpha 上 35–48 dB）；自测中 noise 的 20 dB 门限
  已豁免（`--no-psnr-bar`），只要求全程跑通。
- 交叉验证：同一 ASTC 文件，astcenc 解码 vs Eden 解码，
  PSNR 57.71 dB、最大绝对差 1（舍入级）。两个独立 ASTC 解码器一致，
  排除了 Eden 解码器系统性偏差被往返测试掩盖的可能。

耗时（253，4 核，texcode 单进程；每 256×256 level）：
ASTC 解码约 1.6–1.7 ms，BC 编码约 0.5–0.6 ms，BC 解码约 0.3 ms；
512² 8×8 level：解码 3.4 ms、编码 2.2 ms、解码 1.3 ms。
这是 253 x86 CPU 的数字，**不是 PS5 的转码耗时**。

### 6.3 打包 + 校验端到端（实测，合成捕获）

5 条合成 manifest：3 条可转码（BC1 单层、BC3 单层、BC3 三级 mip 链）、
1 条 swizzled、1 条双层。`build` 结果：ok=3、skipped=2、errors=0；
`astc_in=210720 B → bc_out=249856 B，ratio=1.19`（BC 比 ASTC 大，
见 §1.3）；转码总计 23.0 ms；子进程峰值 RSS 17536 KiB。
`verify`：5/5 level 往返通过（PSNR 36–47 dB），重编码哈希与包内
文件逐字节一致。

### 6.4 未做的实测（明说）

- **Eden 全量编译验证：已通过**（2026-11-07 05:40–06:50，容器内
  `make package`，`BUILD_EXIT:0`；日志
  `/home/swizard/offline-pack-build.log`）。此前未做的原因：253 主机
  上无系统级 Vulkan/fmt/Boost/SDL 开发头文件，直接在主机编会失败；
  正确做法是在容器（`gallant_banach`，已挂载 `/data/code`）内编，
  scratch 缓存按工作树分开（`owner` 校验只拒绝复用另一工作树的缓存，
  另起一套是预期行为）。产物：`build/release/PPSA98515/eboot.bin`
 （78,595,902 字节，sha256 `6d109098d2ebd1fc…`），`EDEN_BUILD
  scm_rev=5921ae1952`（即本分支子模块头）。加载器已编进包内：
  eboot 中含 `TryUploadPackedAstc` 符号与三条加载器日志字符串
  （`Offline pack loaded: {} textures from {}` 等）。自测全部 PASS
  （含 GPU worker / DMA fence / JIT / worker topology）。
  加载器头文件本身也已独立编译 + 单测通过（`offline_pack_loader.h`
  standalone 测试 + 真实包互通测试全过）；`texture_cache.h` 的补丁
  经逐行人工核对（字段名/类型均与 `image_base.h`、`image_info.h`、
  `types.h` 逐一对照）。**未部署**（本分支不部署；09:23 部署的是
  主线 build56，见定时任务）。
- **headless Eden Linux 原生构建未跑**：同上原因，未谎报。
- **PS5 实测未做**：无真实捕获数据（§1.2）；包的加载器不存在，
  故无 PS5 帧率/内存对比。

### 6.5 与 build55 基线的对比（只写站得住脚的）

| 对比项 | 结论 |
|---|---|
| 转码耗时/内存 | 未知。PS5 上无转码耗时实测；253 CPU 数字不可换算到 PS5 GPU compute 路径。 |
| 显存占用 | 推算：BC3 不省（§1.3）；净效果待捕获数据统计格式分布后才能算。 |
| 帧率（时之笛 16 帧） | 未知。瓶颈未定位，不可归因于转码。 |
| OOM（1122 秒、heap1 92%） | 本包不解决。内存增长源头未知。 |
| Shader 首次编译卡顿 | 预期改善（编译从首次遇到移到启动批量做；驱动侧编译仍靠 vulkan_pipelines.bin），未验证：无时之笛 shader 数量/编译耗时实测。 |

## 7. 预期收益表（每项标依据）

| 收益 | 预期 | 依据 |
|---|---|---|
| 去掉每次 ASTC 上传的 GPU 解码 + BC 重编码 dispatch 与 staging 往返 | 有，但量级未知 | 推算：`vk_texture_cache.cpp` 的转码路径确实存在（§4.3）；PS5 上每次上传的耗时无实测。253 CPU 参考：每 256² level 解码+编码约 2.3 ms（§6.2），不可换算到 PS5。 |
| 去掉 CPU 解码路径的瞬时 RGBA8 中间缓冲（4 B/texel） | 有 | 推算：例 4096² ASTC 8×8：ASTC 4 MiB → RGBA8 64 MiB 瞬时。GPU 路径的 staging 规模未实测。 |
| 显存占用下降 | 否（可能上升） | 推算+实测：§1.3 的块尺寸算术；E2E ratio 1.19。 |
| 帧率提升 | 未知 | 未验证：瓶颈未定位。 |
| OOM 修复 | 否 | 未验证：增长源头未知。 |
| Shader 首次编译卡顿消除 | 预期有（形式是“移到启动批量做”而非消除；见 §5 纠正） | 推算：机制存在（§5）；时之笛实测无。 |
| 确定性（同包每次同字节） | 有 | 实测：`verify` 重编码哈希一致（§6.3）。 |

## 8. 已知缺口与下一步

1. **swizzled 捕获的离线反 swizzle 未实现**（最大缺口）：PS5 默认
   GPU 加速分支产出的正是 swizzled 字节；v1 只转码 `linear`。
   下一步：把 `UnswizzleImage` 的 2D 块线性布局逻辑移植到离线工具
   （或在捕获运行中强制 CPU 解码路径以拿到 linear 字节——会改变
   捕获运行的行为，只适用于捕获，不适用于游玩）。
2. **加载器已实现**（2026-11-07，子模块 `5921ae1952`）：
   - `src/video_core/texture_cache/offline_pack_loader.h`（新增，
     仅依赖 `<std>` + `common_types.h`）：`OfflinePackLoader` 读
     `<pack>/manifest.json`（手写 JSON 解析，只认 v1 schema；坏
     manifest → 加载失败，坏 entry → 跳过），只收 `status == "ok"`
     的 entry，按 `gpu_addr` 建索引；`ReadLevelFile` 按 manifest
     的 `bytes` 校验文件尺寸。独立编译 + 单测 + 真实包互通测试全过。
   - `src/video_core/renderer_vulkan/vk_texture_cache.h/.cpp`：
     `TextureCacheRuntime` 持 `OfflinePackLoader offline_pack`；构造
     末尾若 `EDEN_OFFLINE_PACK=<pack dir>` 存在则加载；
     `AccelerateImageUpload` 的 ASTC 分支最前面先调
     `TryUploadPackedAstc(image, map)`：按 `image.gpu_addr` 查包，
     全量校验（format/尺寸/levels/layers/type/`host_level_offset`
     与当前 image 一致；每 level 的 `codec` 必须与运行时
     `IsOpaqueBc1Astc` 的 BC1/BC3 判定一致；文件尺寸等于
     `BcLevelSize` 期望值）——任一项不符即安全未命中；通过后读每
     host level 的 `.bc` 文件，拼进一个 staging buffer，一次
     `UploadMemory`。仅 2D、单层、depth 1 且 `WillUseGpuBcnTranscode`
     为真时才查包；其余走现有运行时转码。
   - 未做的加载器部分：`vk_pipeline_cache.cpp` 的 `LoadDiskResources`
     先试包内 `shaders/`（版本 19 才采用；`vulkan_pipelines.bin`
     仅限同驱动）——仍待接线，目前包内 shader 缓存需手动放到
     ShaderDir 才生效。
   - `UseBc3ForSwitch2OnSmallCards`（`vk_texture_cache.cpp:1213`）
     保留作未命中时的 fallback。
   - Dynarmic JIT 缓存持久化：需新做（§1.1），不在加载器任务内。
3. **全量编译验证**：并入主线前必须在 Eden 构建里编过
   `texture_cache.h` 的改动（§6.4）。
4. **真实捕获数据**：一次 PS5 捕获运行（§3 步骤 1）+ 格式分布统计，
   才能算包的真实体积与收益。
5. **多层/3D 纹理**：v1 跳过（§2.4），待捕获数据看占比再定。

## 9. 格式定稿：文件包为标准，二进制包已删

2026-11-07 05:30 树里曾出现另一套包格式：二进制单文件
`Eglantine/src/video_core/texture_cache/offline_pack.h`
（`PackHeader` 64 字节 + `PackTextureEntry` 48 字节 + BC blob，
**按内容键** `CityHash64`（元数据 + staged 源字节），自带
`PackReader`，magic `EDENPACK`、版本 1），配套设计文档
`docs/OFFLINE_PACK.md`（`caaabb714a`）。**已定稿：以本原型的
文件包（manifest.json + 每 level 一个 .bc 文件，按 guest 地址 key）
为标准格式；二进制包已于 2026-11-07 删除，不再提交。**
定稿理由：捕获 + 转码 + 打包三件套已按文件包建成，加载器按文件包
实现，避免两套格式并存；内容键更稳（跨运行稳定）是事实，但要
落地需工具对 staging 字节（guest block-linear 布局）哈希，而工具
输入是 linear 解码流——须先实现离线反 swizzle（§8.1），才能用
内容键。待反 swizzle 实现后，可再议是否把 key 从地址换成内容。
两套格式的共同点：ASTC→BC1/BC3、opaque→BC1、level-major、host 只
保留 `host_level_offset` 之后的 level。
另一独立发现（转码收益的补充机制，仍未量化）：GPU 转码有节流——
最多 2 个 pending 转码，超了 `scheduler.Wait` 直接堵 GPU 线程
（`TranscodeAstcToBcn`，`vk_texture_cache.cpp` 约 4234 行）——新
区域大量纹理涌入时的卡顿源头之一。离线包去掉的正是这部分每纹理
dispatch + 节流等待。
另：`docs/OFFLINE_PACK.md` 现为指向本文档的指针（不再是独立设计）。

## 附：文件清单（本原型新增/改动；已提交并推送，子模块 `5921ae1952`、
父仓库 `7ccec3b239`，分支 `jarvis/offline-pack`，未部署）

| 文件 | 改动 |
|---|---|
| `Eglantine/src/video_core/texture_cache/offline_capture.h` | 新增，捕获开关实现 |
| `Eglantine/src/video_core/texture_cache/texture_cache.h` | +58 行（include 1 行 + 4 处 hook） |
| `tools/offline-pack/eden_offline_pack.py` | 新增，打包/校验/自测工具 |
| `tools/offline-pack/texcode/main.cpp` | 新增，转码器（Eden 自带编解码） |
| `tools/offline-pack/texcode/build-texcode.sh` | 新增，构建脚本 |
| `tools/offline-pack/texcode/setup-deps.sh` | 新增，取 boost/fmt/astcenc（无 sudo） |
| `tools/offline-pack/texcode/build/eden_texcode` | 构建产物（273792 字节，可重建） |
| `tools/offline-pack/warm_caches.sh` | 新增，shader 缓存打入包 |
| `apply-capture-patch.py` | 新增，texture_cache.h 的补丁脚本 |
| `docs/offline-pack-design.md` | 本文档 |
| `Eglantine/src/video_core/texture_cache/offline_pack_loader.h` | 新增，包加载器（manifest.json 解析 + 索引 + 读 level 文件） |
| `Eglantine/src/video_core/renderer_vulkan/vk_texture_cache.h` | +10 行（include + `OfflinePackLoader` 成员 + `TryUploadPackedAstc` 声明） |
| `Eglantine/src/video_core/renderer_vulkan/vk_texture_cache.cpp` | +114 行（构造时加载包 + `AccelerateImageUpload` 钩子 + `TryUploadPackedAstc` 实现） |
| `docs/OFFLINE_PACK.md` | 改为指向本文档的指针（原二进制包设计已废弃，见 §9） |
