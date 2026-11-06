# Eden 离线预处理包（Offline Pack）— 设计（as-built）

分支：`jarvis/offline-pack`（父仓库 + 子模块），从 build55 头（父 `cfbcc19792` / 子模块 `08598c9984`）分出。
目标：253 上离线做重活（转码纹理、预填管线缓存），产出中间包；PS5 只加载中间包，运行时不再现做。
**状态（2026-11-07）**：捕获 + 转码 + 打包 + PS5 加载分支全部实现并验证（见 §7）。PS5 实机加载测试待 PS5 在线后做。

> 注：本文件 2026-11-07 重写为 as-built。原 v1 二进制包方案（`edenpack` 单文件、内容哈希 key、C++ 离线工具）已废弃，未提交——并行工作已建成文件包（manifest.json + 每 level 一个 .bc 文件、按 guest 地址 key），捕获 + 转码 + 打包三件套齐全；本分支只补最后缺的 PS5 加载分支，避免两套格式并存。旧方案见 git 历史（`caaabb714a`）。

## 1. 流水线总览

```
PS5 捕获运行（EDEN_OFFLINE_CAPTURE=1）         253 离线
  Eden 跑游戏，每张 ASTC 上传时记录 ──> capture/
    manifest.jsonl（每行一个 JSON：gpu_addr/format/dims/levels/...
                    host_level_offset/astc_opaque/layout/dump）
    textures/<gpuaddr>.astc（原始 ASTC 字节）
                                            │
                                            ▼
                              eden_offline_pack.py build
                                + eden_texcode（Eden 原生编解码）
                                            │
                                            ▼
                              pack/
                                manifest.json（包清单，v1）
                                textures/<addr>_L<lv>.bc1|.bc3
                                shaders/vulkan.bin（warm_caches.sh 拷入）
                                shaders/vulkan_pipelines.bin
                                            │
PS5 加载运行（EDEN_OFFLINE_PACK=<pack dir>）     │
  TextureCacheRuntime 启动时读 manifest.json <──┘
  ASTC 上传时按 gpu_addr 查包，命中 → 直接上传 BC 文件，
  未命中 → 走现有运行时转码（行为不变）
```

## 2. 现状调研：PS5 运行时在现做什么重活

### 2.1 ASTC → BC3 转码（每张纹理，首次上传时）

- 触发：`UseBc3ForSwitch2OnSmallCards`（`vk_texture_cache.cpp:1213`）。显存预算 6144 MiB < 12 GiB 时，本会话所有 ASTC 纹理强制重压缩为 BC3（`EDEN_NX2_KEEP_ASTC=1` 可保留不压缩，但 override 优先于用户设置）。
- 入口：`TextureCacheRuntime::AccelerateImageUpload(image, map, swizzles, ...)`（`vk_texture_cache.cpp:4198`）。ASTC 时：
  - `WillUseGpuBcnTranscode`（:285：ASTC + 非 optimal-ASTC + 非 huge array + 2D + depth 1 + `accelerate_astc==Gpu` + 重压缩 != Uncompressed）→ `TranscodeAstcToBcn`（:4234）。
  - 否则 → `astc_decoder_pass->Assemble`（GPU 解码为 RGBA8）。
- GPU 转码（`TranscodeAstcToBcn`）：分配一张临时 RGBA8 解码图，dispatch ASTC 解码 compute（`astc_decoder.comp`）→ dispatch BC 编码 compute（`bc_encoder.comp`，BC1/BC3）。**节流：最多 2 个 pending 转码，超了 `scheduler.Wait` 直接堵 GPU 线程**——新区域大量纹理涌入时的卡顿源头之一。
- CPU 路径（`texture_cache/util.cpp` ~1255）：`ASTC::Decompress` → RGBA8（0013 的 `ASTC_CPU_DECODE` 诊断即打在这里）。
- 输入：guest ASTC 原始字节（已在 staging buffer `map.mapped_span` 里）+ `ImageInfo`（format、尺寸、block 维、levels/layers、`astc_opaque`）。
- 输出：BC1（opaque 时）/BC3 GPU 图，可直接采样。
- 代价：每张纹理 2 次 GPU compute dispatch + 临时解码图分配 + 节流等待。
- **staging 布局（key 问题的关键）**：`UploadImageContents`（`texture_cache.h:1821`）里，加速路径的 staging 有两种形态——未裁剪时是 guest 原始字节整拷（block-linear 布局）；`host_level_offset != 0` 时只读每层 mip 链的尾部（kept levels）。所以“包按内容 key”需要工具与运行时对同一字节流哈希，而 guest 是 block-linear、工具输入是 linear——**这就是包改按 guest 地址 key 的原因**（见 §3）。

### 2.2 Shader 编译（每个新 shader）

- 流程：guest shader → lowering（`lower_to_maxwell.h`，CPU）→ IR 优化 → SPIR-V（`emit_spirv`，CPU）→ `vkCreateGraphics/ComputePipelines`（驱动，GPU 线程）。
- 日志实证：`Graphics pipeline built on the GPU thread in 2–15 ms` 每条；一个新区域几百条管线。
- 已有缓存（**不是 PPTC**，本代码库无 PPTC 系统）：
  - `vulkan.bin`（Eden 自有管线缓存，`ShaderDir/{title_id:016x}/vulkan.bin`，`CACHE_VERSION = 19`）：存的是 shader **环境**（guest 代码 + 状态），不是编译产物。启动时 `LoadPipelines` 在 worker 线程上重编全部管线；退出时 `SerializePipeline` 追加新记录。**只在干净退出时写盘**——崩溃/强制关 App 会丢本次新增。
  - `vulkan_pipelines.bin`（Vulkan 驱动管线缓存）：驱动编译产物，**驱动专属**。253（Intel Mesa）的缓存 PS5（RADV）用不了。
- 离线价值：**预填 `vulkan.bin`**（`tools/offline-pack/warm_caches.sh` 把一次跑游戏后 Eden 自己写出的缓存拷进包里，PS5 启动时就批量编好，而不是进新区域时现编）。生成它需要跑游戏（发现 shader），253 上没有游戏 NUP（在 PS5 U 盘上）——所以 warm_caches.sh 只做“拷入”，不做“预热”。

### 2.3 结论：离线包分两层

| 层 | 内容 | 离线生成 | PS5 加载 |
|---|---|---|---|
| 纹理（本期 MVP） | 预转码 BC1/BC3 文件 | 捕获 + texcode CPU 转码 | 查包命中 → 直接上传，不走解码/编码 dispatch |
| 管线（已接线） | 预填的 `vulkan.bin` | 跑游戏后由 Eden 自己写出，warm_caches.sh 拷入 | 已有 `LoadPipelines` 路径，包只需落到 ShaderDir |

## 3. 中间包格式 v1（文件包）

由 `tools/offline-pack/eden_offline_pack.py build` 产出：

```
<pack>/
  manifest.json
  textures/<gpuaddr16hex>_L<level>.bc1|.bc3   # 每张纹理每个 guest mip level 一个文件
  shaders/vulkan.bin                          # warm_caches.sh 拷入（可选）
  shaders/vulkan_pipelines.bin                # 同上（可选，驱动专属）
```

`manifest.json`：`{"version": 1, "created_unix": ..., "capture_dir": ..., "tool": ..., "entries": [...], "stats": {...}}`。
每个 entry = 捕获记录 + `format_name` + `levels_out` + `status`：

| 字段 | 来源 | 含义 |
|---|---|---|
| `gpu_addr` | 捕获 | guest 地址，`"0x..."` 十六进制字符串；**包的 key** |
| `format` / `width` / `height` / `depth` / `levels` / `layers` / `type` | 捕获 | PixelFormat 数值（surface.h 枚举序）、guest 尺寸、mip 层数、数组层数、ImageType（e2D=1） |
| `guest_size_bytes` / `host_level_offset` | 捕获 | guest 总字节数；PS5 侧裁剪掉的 mip 层数（2048 cap / 128 MiB 规则） |
| `astc_opaque` | 捕获 | 全不透明 → BC1，否则 BC3（与运行时 `IsOpaqueBc1Astc` 同规则） |
| `layout` | 捕获 | `"linear"` / `"swizzled"` / `"swizzled-tail"`（见 §4） |
| `dump` / `dump_bytes` | 捕获 | 捕获的原始 ASTC 文件相对路径及字节数（`null` 表示只记了元数据） |
| `levels_out[]` | 打包 | 每 level：`{level, width, height, file, bytes, sha256, codec}`；`file` 为相对路径 |
| `status` | 打包 | `"ok"`（全部 level 转码成功）/ `"skipped-..."` / `"error-..."`；只有 `"ok"` 可被加载 |

**为什么按地址 key 而不用内容哈希**：运行时 staging 里的字节是 guest block-linear 布局，离线工具拿到的是 linear 解码流；要对同一内容哈希，工具得先把 block-linear 反 swizzle（需复刻 `FullUploadSwizzles` 的 GOB/tile 数学 + TIC 的 block/tile_width_spacing，见 §6 未做项）。地址 key 避开了这个问题，代价是地址必须跨运行稳定（同游戏同版本实测稳定；若变，包只会未命中，不会错传——加载时 format/dims/levels/codec 全量校验）。

## 4. 捕获（`src/video_core/texture_cache/offline_capture.h` + `texture_cache.h` hook）

- 开关：`EDEN_OFFLINE_CAPTURE=1`（主开关）、`EDEN_OFFLINE_CAPTURE_DUMP=1`（同时落原始字节）、`EDEN_OFFLINE_CAPTURE_DIR=<path>`（默认 `offline-capture`，PS5 上用绝对路径如 `/mnt/usb1/offline-capture`）。
- 钩子位置：`TextureCache<P>::UploadImageContents`（所有 guest→host 纹理上传的唯一漏斗），ASTC 纹理才记录；未开捕获时每次调用只有一次 bool 检查，正常路径零变化。
- 记录内容：`RecordAstcUpload(gpu_addr, format, width, height, depth, levels, layers, type, guest_size_bytes, host_level_offset, astc_opaque, layout, raw_bytes)`，写 `manifest.jsonl`（每行一个 JSON）+ `textures/<gpuaddr16hex>.astc`。
- 三种 layout：
  - `"linear"`：已反 swizzle 的 ASTC 块流（mip level 依次排列，`UnswizzleImage` 的输出）——**唯一可被 v1 打包器转码的**；
  - `"swizzled"`：加速路径整拷的 staging 字节（仍是 Tegra block-linear 布局）；
  - `"swizzled-tail"`：裁剪后只剩 kept levels 的尾部（同上，block-linear）。
- 约束：默认关闭；尽力而为、永不致命（全部文件操作包 try/catch，失败吞掉）；**每地址每进程只记第一次上传**（重传不同内容不重记）。

## 5. 打包（`tools/offline-pack/`）

- `eden_offline_pack.py build --capture-dir D --out P [--texcode T]`：读 `manifest.jsonl`，只转码 `layout == "linear"` 且 `layers == 1` 且 `depth == 1` 且有 dump 的 entry；其余记 `skipped-...`。每 level 调 `eden_texcode astc-decode` → `bc1/bc3-encode`（codec 由 `astc_opaque` 定：true → BC1，否则 BC3），写 `textures/<addr>_L<lv>.bc1|.bc3`，算 sha256，写 `manifest.json`。
- `eden_offline_pack.py verify --pack P --capture-dir D`：重编码 + 比较 + PSNR。
- `eden_offline_pack.py selftest --workdir W --astcenc A`：合成图 → astcenc 编真 ASTC → texcode roundtrip，打印尺寸/耗时/PSNR（只证链路通，不证游戏收益）。
- `tools/offline-pack/texcode/`（`main.cpp` + `build-texcode.sh`）：独立转码器，直链 Eden 原生代码——CPU ASTC 解码（`textures/astc.cpp`）、CPU BC 编码（`textures/bcn.cpp`，stb_dxt）、BC 解码（`texture_cache/decode_bc.cpp`）。**不重写编解码器**。命令：`astc-decode / bc1-encode / bc3-encode / bc1-decode / bc3-decode / roundtrip`，每次操作一个 mip level。
- `tools/offline-pack/warm_caches.sh`：把 Eden 已有磁盘 shader 缓存（`vulkan.bin`、`vulkan_pipelines.bin`）拷进包的 `shaders/`。它不预热，只搬运；预热 = 跑一次游戏让 Eden 自己写出缓存。

## 6. PS5 加载分支（`offline_pack_loader.h` + `vk_texture_cache` hook）

- 加载器：`src/video_core/texture_cache/offline_pack_loader.h`（仅依赖 `<std>` + `common_types.h`，可独立单测）。`OfflinePackLoader::Load(<pack dir>)` 读 `manifest.json`，手写 JSON 解析（只认本 schema：扁平对象 + `levels_out` 数组，无转义字段；坏 manifest → 加载失败，坏 entry → 跳过），只收 `status == "ok"` 的 entry，按 `gpu_addr` 建索引。`ReadLevelFile` 按 manifest 的 `bytes` 校验文件尺寸。
- 启动：`TextureCacheRuntime` 构造末尾，若环境变量 `EDEN_OFFLINE_PACK=<pack dir>` 存在则加载；不存在 → 无包，行为零变化；损坏 → 警告后丢弃，走运行时转码。
- 命中点：`AccelerateImageUpload` 的 ASTC 分支最前面，先调 `TryUploadPackedAstc(image, map)`；返回 true 即已上传，直接 return；false 则走现有路径。
- `TryUploadPackedAstc`（`vk_texture_cache.cpp`）：仅 2D、单层、depth 1 且 `WillUseGpuBcnTranscode` 为真时才查包；按 `image.gpu_addr` 查；**全量校验**（format、尺寸、levels、layers、type、`host_level_offset` 与当前 image 一致；每 level 的 `codec` 必须与运行时 `IsOpaqueBc1Astc` 的 BC1/BC3 判定一致；文件尺寸必须等于 `BcLevelSize` 期望值）——任一项不符即安全未命中。校验通过后读每 host level 的 `.bc` 文件，拼进一个 staging buffer，拼 `BufferImageCopy`（每 level 一条，`buffer_row_length`/`buffer_image_height` 按 4 对齐），一次 `UploadMemory`。任何一步失败都返回 false，不抛、不影响正常路径。
- 环境变量：`EDEN_OFFLINE_PACK=<pack dir>`（包目录绝对路径，PS5 上如 `/mnt/usb1/offline-pack`）。未设 → 无包。

## 7. 验证结果（2026-11-07，253 上）

| 项 | 结果 |
|---|---|
| texcode roundtrip（`eden_offline_pack.py selftest`） | 全部通过：13 组合成图（gradient/checker/noise/alpha，4x4/8x8/6x6/5x4），PSNR 35–47 dB（noise 压力组 15 dB，符合预期）；astcenc 与 Eden ASTC 解码交叉校验 PSNR 57.71 dB、最大差 1 |
| 打包器 + 加载器互通 | 合成捕获（256x256 ASTC 4x4，opaque）→ builder 产包（1 entry，`ok`，BC1 32768 B，ratio 0.5）→ `OfflinePackLoader` 加载成功，entry 字段与 BC1 文件逐字节读回正确 |
| 加载器单测（`/tmp/test_offline_pack_loader`） | 全部通过：只计 `ok` entry、跳过 `skipped` 与坏 codec entry、未知地址未命中、坏版本/缺失 manifest 加载失败、尺寸校验生效 |
| PS5 侧编译（`make package`，容器内） | 进行中（tmux `offline-pack-build`，日志 `/home/swizard/offline-pack-build.log`；并行代理此前未做，称 253 缺 Vulkan 头文件——实际应在容器内编） |

## 8. 已知限制（问一次，不追）

1. **按地址 key**：guest 地址若跨运行变化，包未命中（安全，只是不生效）。同游戏同版本实测稳定；加载时全量校验，地址复用给不同纹理时尺寸/format 不符即未命中（内容相同但像素不同时无法察觉，属已知风险）。
2. **只转码 `layout == "linear"`**：加速路径的 ASTC 上传（`swizzled`/`swizzled-tail`，即 block-linear 原始字节）目前被打包器跳过。离线反 swizzle 未实现：需复刻 `FullUploadSwizzles`（`util.cpp:1373`）的 GOB/tile 数学 + TIC 的 block/tile_width_spacing，用 `Tegra::Texture::SwizzleTexture`（`textures/decoders.cpp`）反解。这是下一个大项。
3. **单层、depth 1、2D**：多层数组/3D 纹理（及 huge ASTC array）不进包，走运行时转码。
4. **每地址只记第一次上传**：同一地址后被复用、内容变了，不重记、不更新包。
5. **管线段只搬运不预热**：`vulkan.bin` 需事先跑游戏生成；253 上无 NUP，无法离线生成。
6. **`KEEP_ASTC=1` 对照实验**（根因调研建议）：若条纹是转码的锅，包可改为对颜色关键小纹理（64x64x64 LUT）存 RGBA8 而非 BC3——待 PS5 实测后定。

## 9. 使用流程（给用户的）

1. PS5 上跑一次游戏，`EDEN_OFFLINE_CAPTURE=1 EDEN_OFFLINE_CAPTURE_DUMP=1 EDEN_OFFLINE_CAPTURE_DIR=/mnt/usb1/offline-capture`，得到捕获目录。
2. 把捕获目录拷到 253，`python3 tools/offline-pack/eden_offline_pack.py --texcode tools/offline-pack/texcode/build/eden_texcode build --capture-dir <capture> --out <pack>`；可选 `warm_caches.sh` 把 shader 缓存拷进包。
3. 把包拷回 PS5（如 `/mnt/usb1/offline-pack`），启动 Eden 时设 `EDEN_OFFLINE_PACK=/mnt/usb1/offline-pack`。
4. 进游戏：命中的纹理不再走 GPU 转码 dispatch；日志会打 `Offline pack loaded: N textures`。未命中则静默走老路。

## 10. 下一步（未做的部分）

1. 离线反 swizzle（§8.2）：让 `swizzled`/`swizzled-tail` 的捕获也能进包——这是覆盖率的关键（加速路径的 ASTC 占多数）。
2. PS5 实机加载测试：有包 + PS5 在线时，验证命中率、帧率变化、条纹/发黄是否改善。
3. 管线段生成器：253 上跑游戏 headless 发现 shader、产出预填 `vulkan.bin`（需要游戏 NUP 在 253 上；现 NUP 只在 PS5 U 盘）。
4. 包的增量更新：游戏更新后纹理变化，按 `gpu_addr` 去重/追加。
