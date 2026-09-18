# WO-01 — S0：Vulkan 模块骨架（依赖 / CMake / 设备 / SPIR-V 嵌入 / 空后端）

## 目标

建立 `ember_vulkan` 模块：构建系统接入、设备环境注入接口、测试用自建设备
helper、构建期 SPIR-V 嵌入机制、一个什么都不做的空 `VulkanBackend`。
本工单结束后模块可编译、可链接、可初始化/析构，但 `capabilities()` 全 false，
`update()/render()` 是合法空操作。

## 必读

- `workorders/README.md`（全局规则）
- `CMakeLists.txt` 现有依赖接入段（glad/glfw/glm 的 FetchContent + 离线 tarball 模式）
- `include/ember/backend.hpp`、`include/ember/opengl.hpp`（公开适配 header 的形态）
- `src/backends/opengl/compat.cpp`（free function 适配器 + requireOpenGL 模式）
- `docs/backends.md` 的“模块与源码”表和“构建和选择”节

## 实现要求

### 1. CMake（改 `CMakeLists.txt`）

- 新增 `option(EMBER_BUILD_VULKAN "Build the Vulkan backend" ON)`。
- 查找依赖（GitHub 不通，按此优先级）：
  a. `find_package(Vulkan QUIET)`（本机 MSYS2 ucrt64 已装
     `include/vulkan` + `lib/libvulkan-1.dll.a`，CMake 3.16 的 FindVulkan 能找到；
     必要时用 `Vulkan_INCLUDE_DIR`/`Vulkan_LIBRARY` 缓存变量兜底）；
  b. 找不到则 `message(STATUS ...)` 并跳过整个模块（不报错，与 GLFW 关闭时跳过示例同理）。
- 找到时：
  - `add_library(ember_vulkan STATIC ...)` + `add_library(ember::vulkan ALIAS ember_vulkan)`，
    `target_link_libraries(ember_vulkan PUBLIC ember_core ${Vulkan_LIBRARIES})`，
    include `Vulkan_INCLUDE_DIRS`；加入 `_ember_install_targets`。
  - `cmake/emberConfig.cmake.in` 增加 `vulkan` 组件（照 opengl/glfw 的既有模式）。
- 新增 `tests/ember_vulkan_test`（见第 4 节），`add_test(NAME vulkan_test ...)`，
  `SKIP_RETURN_CODE 77`，`WORKING_DIRECTORY` 源根。

### 2. 公开头文件 `include/ember/vulkan.hpp`

对照 `include/ember/opengl.hpp` 的形态，只暴露：

```cpp
namespace ember {
struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;             // 借用，不接管
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE; // 借用
    VkDevice device = VK_NULL_HANDLE;                 // 借用
    std::uint32_t queueFamilyIndex = 0;
    VkQueue queue = VK_NULL_HANDLE;                   // graphics+compute 队列
};
std::unique_ptr<ParticleBackend> makeVulkanBackend(); // 之后必须 setVulkanContext
void setVulkanContext(ParticleBackend&, const VulkanContext&);
// 测试/helper 用：自建 instance+device（含可选 validation），所有权归返回结构
struct VulkanDevice { VkInstance instance; VkPhysicalDevice physicalDevice;
                      VkDevice device; std::uint32_t queueFamilyIndex; VkQueue queue;
                      /* RAII 析构销毁 device/instance */ };
// 无可用设备/驱动时抛 std::runtime_error；debug=true 时尝试挂
// VK_LAYER_KHRONOS_validation（层不存在则跳过，不报错）
VulkanDevice makeVulkanDevice(bool debug = false);
}
```

注意：`vulkan.hpp` 会包含 `<vulkan/vulkan.h>`，文档注释里写明这一点
（与 GL 侧 `opengl.hpp` 包含 glad 同理，不属于公共 facade 泄漏——
`system.hpp`/`backend.hpp` 仍然干净）。

### 3. 后端骨架 `src/backends/vulkan/`

- `common.hpp`：`VK_CHECK` 宏（VkResult → `std::runtime_error`，带调用点）、
  frames-in-flight 常量（2）、binding 常量直接 `#include "../../src/backends/opengl/common.hpp"`？
  **不要**——binding 常量在 GL 目录里且带 GL 类型。做法：把数值相同的常量
  在 vulkan/common.hpp 里重定义一份并加注释“与 GL 后端编号一致”，或把纯数字
  常量上移到公共头。**选择前者**（本工单不动公共头，减少涟漪）。
- `vulkan_backend.hpp`：`class VulkanBackend final : public ParticleBackend`，
  全部虚函数给出空实现或最小实现：
  - `name()` → `"vulkan"`；`capabilities()` → 全 false；
  - `initialize()` 记录 capacity/debug；`resize/clear` 重置计数；
  - `update()` 仅递增帧序号（契约：成功 update 序号 +1）；
  - `render()` 空操作；`readParticles()` 返回空；统计返回零值；
  - `upload*` 存 CPU 副本即可。
- `context.cpp`：`makeVulkanBackend`、`setVulkanContext`（dynamic_cast 校验，
  非 Vulkan 后端抛 `invalid_argument`，对照 `requireOpenGL`）、
  `makeVulkanDevice`（枚举物理设备，找 graphics|compute 同族队列，
  `vkCreateInstance/vkCreateDevice`；API 版本 `VK_API_VERSION_1_1`；
  debug 时挂 validation layer + debug messenger，层缺失自动降级）。

### 4. 构建期 SPIR-V 嵌入

- 新脚本 `tools/embed_spirv.py <spirv_dir> <out.cpp>`：把
  `simulate.comp.spv` 等 9 个文件打成 `namespace ember::detail::vulkan { ... }`
  的 `const unsigned char[]/size` 表（对照 `src/backends/opengl/shaders.cpp`
  的内嵌 GLSL 形态）。生成物放 **build 目录**（`build/generated/spirv_blobs.cpp`），
  不进源码树（SPIR-V 是构建产物，源码树不存二进制）。
- CMake：`ember_vulkan` 依赖 `ember_spirv` 目标，custom command 调脚本生成
  cpp 并编进 `ember_vulkan`。同时 `add_test` 一个 `spirv_embed_verify`？
  不需要——构建依赖天然保证同步。
- `VulkanBackend` 预留 `loadShader(name)`：优先 `shaderDir/<name>.spv` 文件，
  失败回退内嵌 blob（与 GL 的“文件优先、内嵌兜底”策略一致；本工单只搭机制，
  不创建真实管线）。

### 5. 测试 `tests/vulkan_test.cpp`

骨架：`main` 里 try `makeVulkanDevice(debug)`；任何设备/驱动不可用 →
打印原因并 `return 77`（SKIP）。可用则：`setVulkanContext` 注入
`makeVulkanBackend()`，构造 `ParticleSystem({1024,256,7}, std::move(b))`，
断言 `backendName()=="vulkan"`、`capabilities()` 全 false、`update(0.016f)`
后 `updateSequence()==1`、`aliveCount()==0`。
输出 `vulkan skeleton: PASSED`。

## 验收

```bash
cmake -S . -B build && cmake --build build -j     # 通过，ember_vulkan 产出
export PATH=/e/msys2/ucrt64/bin:$PATH
ctest --test-dir build --output-on-failure        # 10/10（原 9 + vulkan_test）
ctest --test-dir build-core --output-on-failure   # 5/5 不受影响
cmake --build build-noeditor -j                   # 不受影响（先重配置）
```

## 边界

- 不创建任何 VkPipeline/VkBuffer（那是 WO-02）；不实现真 update/render。
- 不改 `tools/amalgamate.py`（单头保持 GL-only）。
- 不动 GL 后端任何文件；不动 `docs/backends.md` 以外的文档（完成后在
  backends.md 接入顺序第 1 步打勾式更新一句即可）。
- `find_package(Vulkan)` 失败时模块静默跳过，不能让无 Vulkan SDK 的
  消费者构建失败。

## 完成记录

- 日期：2026-09-17
- 环境：Windows + MSYS2 ucrt64（Vulkan-Headers 1.4.357 + 加载器已装，
  `E:/msys2/ucrt64`；构建期 Python 3.14）。

### 交付物

- `include/ember/vulkan.hpp`：`VulkanContext`（借用）、`makeVulkanBackend()`、
  `setVulkanContext()`、`VulkanDevice`（自建、RAII、可移动）、`makeVulkanDevice(debug)`。
- `src/backends/vulkan/common.hpp`：`VK_CHECK`、`kFramesInFlight=2`、
  与 GL 同号的 SSBO 0–13 / UBO 14–19 binding 常量、`SpirvBlob`/`embeddedSpirv`。
- `src/backends/vulkan/vulkan_backend.{hpp,cpp}`：空 `VulkanBackend`，
  capabilities 全 false、update 只递增序号、render 空操作、上传存 CPU 副本、
  `loadShader()` 文件优先内嵌兜底。
- `src/backends/vulkan/context.cpp`：工厂、`requireVulkan` dynamic_cast 校验、
  设备模拟（graphics|compute 同族队列、1.1 基线、validation 层缺失自动降级）。
- `tools/embed_spirv.py`：9 个 SPIR-V → `build/generated/spirv_blobs.cpp` 内嵌表。
- `tests/vulkan_test.cpp`：无设备返回 77（SKIP），有设备断言骨架语义。
- `CMakeLists.txt`：`EMBER_BUILD_VULKAN`（默认 ON）、FindVulkan + 手工兜底、
  `ember_vulkan`/`ember::vulkan`、SPIR-V 嵌入 custom command、`vulkan_test`
  （SKIP_RETURN_CODE 77）。
- `cmake/emberConfig.cmake.in`：新增 `vulkan` 组件（含 `find_dependency(Vulkan)`）。
- `docs/backends.md`：模块表加 Vulkan 行、构建选择表加 `ember_vulkan`、接入顺序第 1 步打勾。

### 验收输出摘要

```text
cmake -S . -B build && cmake --build build -j     # 通过
  ember SPIR-V: embedding built-in shaders
  embedded 9 SPIR-V modules -> build/generated/spirv_blobs.cpp
ctest --test-dir build --output-on-failure        # 100% tests passed, 10/10
ctest --test-dir build-core --output-on-failure   # 100% tests passed, 6/6
cmake --build build-noeditor -j                   # 通过
ember_vulkan_test.exe                             # vulkan skeleton: PASSED (exit 0)
```

- 强跳过路径验证：`EMBER_BUILD_VULKAN=OFF` 与 `CMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE`
  均打印 `ember: Vulkan not found; skipping the Vulkan backend`，不生成目标、不报错。

### 与验收基线的差异 / 遗留

- `build-core` 由 5/5 变为 6/6：`EMBER_BUILD_VULKAN` 默认 ON，纯 core 构建
  也会带上 `ember_vulkan` 与 `vulkan_test`（本机 Vulkan 可用）。若要 5/5，
  配置时加 `-DEMBER_BUILD_VULKAN=OFF`。这是选项默认值的有意结果，非回归。
- 本机未安装 `VK_LAYER_KHRONOS_validation`，`makeVulkanDevice(debug=true)`
  的 validation 分支不会被真实覆盖；代码按“层缺失静默降级”实现，未做端到端验证。
- 设备模拟优先选中第一个同时具备 graphics+compute 队列族的 1.1 设备，未按
  性能/独显偏好排序；WO-02 若需可扩展。
- `loadShader()` 把读取到的 SPIR-V 文件存在后端实例成员 `shaderFiles_` 中，
  返回的 `SpirvBlob` 是稳定视图；骨架阶段尚未创建真实管线，WO-02 直接消费。


---

## 验收修复记录（2026-09-17，审查人：主模型）

验收时构建+测试基线即绿（ctest 10/10），但代码审查与 validation layer 发现以下
问题并已直接修复：

1. **`resize()` 后崩溃**：`destroyBuffers()` 连带销毁了 per-frame UBO/staging 且
   不重建。已拆分为 `destroyBuffers()`（持久缓冲）+ `destroyPerFrameBuffers()`
   （仅析构调用）。
2. **力场/调色板缓冲溢出**：palette/attractor/vortex/spring 固定分配 1 个元素，
   上传更大数组即越界写。已加 `ensureHostBuffer()` 按需扩容（在 `updateSimSet`
   之前完成，避免描述符悬挂）。
3. **描述符池欠配**：storage 描述符实际需要 (14+3)×2=34，只配了 32（规范允许
   驱动报错，NVIDIA 宽容未暴露）。已改为 `17*kFramesInFlight+4`。
4. **深度比较语义**：`LESS_OR_EQUAL` 改为 `LESS`，与 GL 默认 depthFunc 对齐。
5. **缺 WO-03 验收用例**：补上深度策略渲染用例（far 可见 / near 遮挡）。
6. **validation layer 规范违规**：`VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL`
   需要 separateDepthStencilLayouts 特性，1.1 基线下改用
   `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`。修复后 validation 全静默。
7. `setContext` 支持 initialize 之后的延迟初始化；`setVulkanSoftDepth`/
   `setVulkanRefractionInputs` 补上定义（存储成员，WO-06 接入描述符）；
   `tests/behavior.hpp` 新增 resize 用例（GL/Vulkan 双端覆盖第 1 条）；
   `EMBER_VK_DEBUG=1` 可开启测试端 validation。

验收结论：通过（修复后 ctest 10/10、build-core 6/6、validation 干净）。
