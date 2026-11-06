# Eden 离线预处理包（Offline Pack）— 设计

分支：`jarvis/offline-pack`（父仓库 + 子模块），从 build55 头（父 `cfbcc19792` / 子模块 `08598c9984`）分出。
目标：253 上离线做重活（转码纹理、预编译 shader、预跑管线缓存），产出中间包；PS5 只加载中间包，运行时不再现做。

## 1. 现状调研：PS5 运行时在现做什么重活

### 1.1 ASTC → BC3 转码（每张纹理，首次上传时）

- 触发：`UseBc3ForSwitch2OnSmallCards`（`vk_texture_cache.cpp:1213`）。显存预算 6144 MiB < 12 GiB 时，本会话所有 ASTC 纹理强制重压缩为 BC3（`EDEN_NX2_KEEP_ASTC=1` 可保留不压缩，但 override 优先于用户设置）。
- 入口：`TextureCacheRuntime::AccelerateImageUpload(image, map, swizzles, ...)`（`vk_texture_cache.cpp:4198`）。ASTC 时：
  - `WillUseGpuBcnTranscode`（:285：ASTC + 非 optimal-ASTC + 非 huge array + 2D + depth 1 + `accelerate_astc==Gpu` + 重压缩 != Uncompressed）→ `TranscodeAstcToBcn`（:4234）。
  - 否则 → `astc_decoder_pass->Assemble`（GPU 解码为 RGBA8）。
- GPU 转码（`TranscodeAstcToBcn`）：分配一张临时 RGBA8 解码图，dispatch ASTC 解码 compute（`astc_decoder.comp`）→ dispatch BC 编码 compute（`bc_encoder.comp`，BC1/BC3）。**节流：最多 2 个 pending 转码，超了 `scheduler.Wait` 直接堵 GPU 线程**——新区域大量纹理涌入时的卡顿源头之一。
- CPU 路径（`texture_cache/util.cpp` ~1255）：`ASTC::Decompress` → RGBA8（0013 的 `ASTC_CPU_DECODE` 诊断即打在这里）。
- 输入：guest ASTC 原始字节（已在 staging buffer `map.mapped_span` 里）+ `ImageInfo`（format、尺寸、block 维、levels/layers、`astc_opaque`）。
- 输出：BC1（opaque 时）/BC3 GPU 图，可直接采样。
- 代价：每张纹理 2 次 GPU compute dispatch + 临时解码图分配 + 节流等待。

### 1.2 Shader 编译（每个新 shader）

- 流程：guest shader → lowering（`lower_to_maxwell.h`，CPU）→ IR 优化 → SPIR-V（`emit_spirv`，CPU）→ `vkCreateGraphics/ComputePipelines`（驱动，GPU 线程）。
- 日志实证：`Graphics pipeline built on the GPU thread in 2–15 ms` 每条；一个新区域几百条管线。
- 已有缓存（**不是 PPTC**，本代码库无 PPTC 系统）：
  - `vulkan.bin`（Eden 自有管线缓存，`ShaderDir/{title_id:016x}/vulkan.bin`，`CACHE_VERSION = 19`）：存的是 shader **环境**（guest 代码 + 状态），不是编译产物。启动时 `LoadPipelines` 在 worker 线程上重编全部管线；退出时 `SerializePipeline` 追加新记录。**只在干净退出时写盘**——崩溃/强制关 App 会丢本次新增。
  - `vulkan_pipelines.bin`（Vulkan 驱动管线缓存）：驱动编译产物，**驱动专属**。253（Intel Mesa）的缓存 PS5（RADV）用不了。
- 结论：shader 预编译的离线价值在于**预填 `vulkan.bin`**（让 PS5 启动时就批量编好，而不是进新区域时现编）。但生成它需要跑游戏（发现 shader），253 上没有游戏 NUP（在 PS5 U 盘上），所以本期只做格式预留 + 设计，生成器随后。

### 1.3 结论：离线包分两层

| 层 | 内容 | 离线生成 | PS5 加载 |
|---|---|---|---|
| 纹理（本期 MVP） | 预转码 BC1/BC3 blob | CPU 编解码即可，无需 GPU | 查包命中 → 直接上传，不走解码/编码 dispatch |
| 管线（phase 2） | 预填的 `vulkan.bin` 记录 | 需跑游戏发现 shader（待 NUP） | 已有 `LoadPipelines` 路径，包只需落到 ShaderDir |

## 2. 中间包格式 v1（`edenpack`）

文件：`offline-pack-{title_id:016x}.bin`，放 ShaderDir（与 `vulkan.bin` 同目录）或游戏目录旁。

```
Header (64 bytes):
  magic[8]      "EDENPACK"
  version u32   1
  title_id u64  游戏 title id（0 = 通用）
  entry_count u32
  flags u32     bit0: 有纹理段；bit1: 有管线段
  tex_count u32 / tex_offset u64
  pipe_count u32 / pipe_offset u64
  reserved[16]

Texture entry (48 bytes, 按 key 升序排，可二分查找）:
  key u64       CityHash64(见下）
  format u32    PixelFormat（ASTC 源格式）
  width u16, height u16, depth u16
  levels u8, layers u16
  block_w u8, block_h u8
  opaque u8     1 = BC1，否则 BC3
  bc_format u8  1 = BC1，3 = BC3
  blob_size u32
  blob_offset u64
  （blob 紧跟在 entry 表后，按 blob_offset 排）
```

**key 的定义**（离线与 PS5 必须完全一致）：
```
key = CityHash64( format(u32 LE) || width(u32 LE) || height(u32 LE) || depth(u32 LE)
                  || levels(u32 LE) || layers(u32 LE) || block_w(u8) || block_h(u8)
                  || astc_source_bytes )
```
只用内容 + 元数据，不用 guest 地址（地址跨运行可能变）。`CityHash64` 用树内 `src/common/cityhash.h`。

**管线段（phase 2 预留）**：直接复用 `vulkan.bin` 的记录格式（magic + version + `num_envs` + 序列化 env + key），包里单列一段；PS5 侧加载时把它当成一份额外的 `vulkan.bin` 喂给 `LoadPipelines`。

## 3. PS5 加载分支（最小改动）

- 启动时（`TextureCacheRuntime` 构造后）：若 ShaderDir 下存在 `offline-pack-{title_id:016x}.bin`，读入并建成 `unordered_map<u64, PackTexture>`（key → blob 视图）。文件不存在 → 静默跳过，行为零变化。
- 命中点（`AccelerateImageUpload`，ASTC 分支的最前面）：
  ```cpp
  if (IsPixelFormatASTC(image.info.format)) {
      if (const auto* packed = offline_pack.Lookup(image.info, map.mapped_span)) {
          return UploadPackedBc(image, *packed);  // 直接上传 BC blob
      }
      // 未命中：走现有路径（GPU 转码 / GPU 解码），行为不变
  }
  ```
- `UploadPackedBc`：把 BC blob 经 staging buffer 拷进 GPU 图（复用现有 `UploadImageContents` 的 BC 路径，不新写上传器）。
- 安全：包损坏/版本不符 → 整个包丢弃并打一条 warning；单个 entry 校验失败（blob 尺寸与 format/dims 对不上）→ 只跳过该 entry。**绝不让包的错误影响正常路径**。

## 4. 离线工具 `eden-offline-pack`

- 位置：`tools/eden-offline-pack/`（独立小工具，g++ 直接编译，不进 Eden 主 CMake——只依赖 `astc.cpp`、`bcn.cpp`、`cityhash.cpp`、`workers.cpp` + `common` 头文件）。
- 输入：一个 manifest（每行：`astc文件 format width height depth block_w block_h`）+ 对应的原始 `.astc` 文件（16 字节文件头：magic + block 维 + 尺寸；或由 manifest 指定）。
- 处理（每张）：读 ASTC 源字节 → `ASTC::Decompress` → RGBA8 → `ASTC::IsOpaque` → `BCN::CompressBC1` 或 `BCN::CompressBC3` → 算 key → 写 pack entry。
- 输出：`offline-pack-{title_id:016x}.bin`。
- 验证样例：用 `astcenc`（ARM 官方编码器，从源码编）把一张测试 PNG 编成 ASTC 4x4/8x8，再跑工具转 BC3，核对 BC3 尺寸与 round-trip 解码像素。

## 5. 下一步（未做的部分）

1. 管线段生成器：253 上跑游戏 headless 发现 shader、产出预填 `vulkan.bin`（需要游戏 NUP 在 253 上；现 NUP 只在 PS5 U 盘）。
2. `KEEP_ASTC=1` 对照实验（根因调研建议）：若条纹是转码的锅，包可改为对颜色关键小纹理（64x64x64 LUT）存 RGBA8 而非 BC3。
3. 包的增量更新：游戏更新后纹理变化，离线工具按 key 去重/追加。
4. PS5 侧包加载的实测（需 PS5 在线 + 有包可传；本期只做到编译通过）。
