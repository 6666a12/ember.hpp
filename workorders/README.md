# Vulkan 后端实施工单

本目录是 ember 的实施工单。WO-01~06 是 Vulkan 后端落地（已完成）；
WO-07 起是双后端功能拓展。每张工单自包含：目标、必读文件、实现要求、
验收命令、边界。执行者可以是任何模型——先读本文件，再读你那张工单。

## 项目背景（30 秒版）

ember 是基于 OpenGL 4.3 compute shader 的 GPU 粒子库。公共层 `ember_core`
（配置/发射器/出生请求编码/统计语义）与后端已分离，GL 后端是唯一现有实现，
Vulkan 后端作为第二个并列模块加入。架构、契约和设计决策全部已定型，
**不要重新设计**——按工单和 `docs/backends.md` 执行。

## 环境与命令（Windows + MSYS2 ucrt64 + Git Bash）

```bash
# 构建（全量，含 GL、示例、测试、SPIR-V）
cmake -S . -B build && cmake --build build -j

# 测试：必须先修 PATH（mingw64/ucrt64 运行时混用会导致 0xc0000139/SEGFAULT）
export PATH=/e/msys2/ucrt64/bin:$PATH
ctest --test-dir build --output-on-failure        # 基线：11/11 全绿，GPU 测试真实执行
ctest --test-dir build-core --output-on-failure   # 纯 core 构建：6/6

# 改了 shaders/ 任何文件之后，必须依次：
python tools/sync_embedded_shaders.py
python tools/amalgamate.py
# 改了 include/ 或 src/ 之后，必须：
python tools/amalgamate.py
```

- GPU：NVIDIA RTX 4060 Laptop（GL 与 Vulkan 驱动均可用）。
- glslangValidator 在 `/e/msys2/ucrt64/bin/`，构建期自动生成 `build/generated/spirv/*.spv`。
- **网络**：GitHub 直连不通；MSYS2 镜像可用（`/e/msys2/usr/bin/pacman`）。
  Vulkan-Headers 1.4.357 + 加载器（`include/vulkan`、`lib/libvulkan-1.dll.a`）
  已通过 pacman 安装；找不到时执行：
  `/e/msys2/usr/bin/pacman -S --noconfirm mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader`

## 硬性规则（每张工单都适用）

1. **不得修改 `ParticleBackend` 公共契约**（`include/ember/backend.hpp`）、
   数据协议（`core.hpp`/`emitters.hpp`/`particle_types.hpp`）和公共 facade
   的行为语义。新增能力走新文件，不改既有签名。
2. **GL 必须始终全绿**：每完成一步跑 `ctest --test-dir build`（11/11）与
   `build-core`（6/6）。任何一张工单合入时这两组不能红。
3. 命名空间：`ember::detail::vulkan`（后端内部）/ `ember`（公开适配 free function）。
4. 不引入 VMA、不用 dynamic rendering（经典 VkRenderPass）、不接管交换链、
   不多队列、不做 GL↔Vulkan 资源共享。Vulkan 基线 1.1（负 viewport 高度）。
5. 直接链接 `vulkan-1`（导入库已装），**不用 volk**（MSYS2 的 volk 包是
   GNU DSP 库，不是 zeux/volk）。
6. 代码风格跟随 GL 后端：C++17、少注释（只写非显然的为什么）、英文注释。
7. 仓库文件换行混杂（CRLF/LF）：编辑时保持各文件原有风格；Edit 工具对
   混合换行文件易失配，可用 Python 脚本做容错替换。
8. 单头文件版保持 GL-only，`tools/amalgamate.py` 不包含 vulkan 目录。
9. 不执行 git 提交/推送；改动留在工作区。
10. 能力诚实声明：`capabilities()` 只开已实现项；未实现的能力开启时必须
    按契约抛 `std::invalid_argument`（基类默认实现已做这件事）。

## 必读公共材料（所有工单）

- `docs/backends.md` —— 重点："后端必须保持的行为"、"Vulkan 渲染环境注入
  （设计定型）"、"Vulkan 的选择性接入顺序"
- `include/ember/backend.hpp` —— 后端契约（参数借用、统计、能力语义）
- `include/ember/particle_types.hpp` —— 每帧输入结构
- `src/backends/opengl/` —— 逐文件的对照实现（算法、屏障位置、协议布局照抄）
- `src/backends/opengl/params.hpp` —— 6 个 std140 块的 CPU 镜像（直接复用，
  布局与 GLSL/SPIR-V 一致）
- `src/backends/opengl/common.hpp` —— binding 编号（SSBO 0–13，UBO 14–19）
- `shaders/` —— 同一份 GLSL 已双 API 兼容；SPIR-V 产物在
  `build/generated/spirv/`（构建目标 `ember_spirv` 自动维护）
- `tests/behavior.hpp` —— 后端无关语义套件（工厂注入即可跑）

## 工单索引

| 工单 | 阶段 | 内容 | 验收核心 |
|---|---|---|---|
| WO-01 | S0 | 依赖/CMake/设备创建/SPIR-V 嵌入/空后端 | ember_vulkan 构建，空后端初始化 |
| WO-02 | S1 | 缓冲/描述符/仿真 phase 链/同步读回/clear/resize | behavior 套件全过 |
| WO-03 | S2 | 帧目标/render pass/billboard 管线/间接绘制 | 离屏像素测试 |
| WO-04 | S3 | fence 环统计/GPU 调度/间接 dispatch | GPU 调度回归 |
| WO-05 | S4 | 分块 bitonic 排序 | 排序颜色序回归 |
| WO-06 | S5 | 贴图/软粒子/折射/bloom | effects 精选移植，capabilities 全开 |
| WO-07 | 事件 | 事件契约：Emitter 字段/facade API/SpawnRequest 填充位/INI [event]/契约测试 | backend_contract_test 新用例 |
| WO-08 | 事件 | 事件 GPU 实现：事件队列/相位 4/双后端/链式 | behavior 事件用例 + cross_backend 事件相位 |
| WO-09 | 曲线 | 生命周期曲线：CPU 烘焙 64 项 LUT/VS 采样/INI [curves] | 双后端像素用例 + 烘焙契约测试 |
| WO-10 | 集成 | Vulkan 宿主 command buffer 录入（begin/endVulkanFrame） | 宿主/默认模式对拍 + 统计 H1-H5 |

## 功能工单执行顺序（WO-07+）

- **WO-07 → WO-08 严格顺序**（事件 GPU 实现依赖契约落地）。
- **WO-09 排在 WO-07 之后**（两者都改 `particle_system.cpp`/`config.hpp`，
  并行必冲突；shader 文件不同但可以同分支先后做）。
- **WO-10 只碰 Vulkan 后端**，与 WO-08/09 的源码面不重叠，但
  `tests/vulkan_test.cpp` 的 capabilities 断言三单都会加字段——并行执行时
  后合入者注意合并该断言。
- 基线测试数已变：WO-06 验收时是 10/10 + 6/6；cross_backend_test 落地后
  为 **11/11 + 6/6**。以后各单验收按当时基线写实际数字。

每张工单完成后：跑全量 ctest、更新 `docs/backends.md` 接入顺序的对应步骤、
在本目录工单文件末尾追加"完成记录"（日期、验收输出摘要、遗留问题）。

## WO-01~03 验收教训（WO-04+ 的额外门禁）

1. **资源生命周期分清"持久"与"每帧"**：销毁/重建函数必须明确服务对象，
   resize 类操作不得销毁 per-frame 资源且不重建（验收第 1 条事故）。
2. **任何 host 可见缓冲按需扩容**：不得假设上传数组的最大尺寸（调色板溢出事故）。
3. **描述符池按"每 set 各类描述符数 × 帧数 + 余量"精确核算**并在注释写出算式。
4. **validation layer 必须干净**：完工前用
   `EMBER_VK_DEBUG=1 VK_LAYER_PATH=/e/msys2/ucrt64/bin ./build/ember_vulkan_test.exe`
   跑通，零 validation 输出才算完成（层包：vulkan-validation-layers，已装）。
5. **工单的验收用例一条都不能少**：缺用例 = 未完成（WO-03 曾漏深度策略例）。
6. GL 语义对齐时逐条对照（如 depthFunc 默认 LESS），不要凭印象选枚举。

## WO-04~06 验收教训（后续工单/维护通用）

7. **描述符更新只能在命令缓冲录制之外**（本后端无 UPDATE_AFTER_BIND）：
   `vkUpdateDescriptorSets` 一律放在 `vkBeginCommandBuffer` 之前；录制区内
   只允许 vkCmd*。per-pass 变化的参数用 `vkCmdUpdateBuffer` 随命令流更新
   （UBO 需 TRANSFER_DST usage + transfer→shader 屏障）；per-pass 变化的
   纹理绑定用每 pass 独立的描述符集（创建期写好）。
8. **间接命令缓冲必须带 `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT`**。
9. **render pass 含深度附件时，图形管线必须给显式 `pDepthStencilState`**
   （即使全禁用）。
10. validation 零输出是硬门禁——功能测试全绿不代表规范合规。

## 完成状态（2026-09-18）

- WO-01~WO-10 全部完成并留在工作区；基线 `ctest --test-dir build` 11/11、
  `ctest --test-dir build-core` 6/6。
- `EMBER_VK_DEBUG=1 VK_LAYER_PATH=/e/msys2/ucrt64/bin` 下 `ember_vulkan_test` 与
  `ember_cross_backend_test` 零 validation 输出。
- WO-07~WO-10 的完成记录见各自工单文件末尾；事件/曲线/宿主 command buffer 的
  文档更新已写入 `docs/backends.md`、README 中英文与 INTEGRATION 中英文。
