# WO-06 — S5：可选效果（sprite 贴图 / 软粒子 / 折射 / bloom）与收尾

前置：WO-05 完成。

## 目标

补齐全部可选渲染能力，`capabilities()` 全开 true，Vulkan 后端功能与 GL 对齐。
完成后更新全部用户文档（README 中英文、INTEGRATION 中英文、backends.md），
把"Vulkan 可运行后端"从路线变成现状。

## 必读

- `src/backends/opengl/render.cpp`（bloom 链、软粒子/折射的 uniform 与纹理单元流）
- `src/backends/opengl/resources.cpp`（`setSpriteTexture`/内置渐变、stb 路径）
- `shaders/particle.frag`（软粒子/折射分支、binding 0/1/2 采样器）
- `shaders/bloom_*.frag`、`shaders/bloom.vert`（三个 pass 的参数）
- `tests/effects.hpp`（软粒子/折射/bloom 的像素断言语义）
- `docs/refraction-design.md`（折射模式语义）

## 实现要求

### 1. sprite 贴图

- 内置 64×64 径向渐变：照 GL 公式 CPU 生成，经 staging 上传 RGBA8 图像
  （linear 过滤、clamp-to-edge，对照 GL 参数）。
- `setSpriteTexture`：`third_party/stb`（模块内 `EMBER_USE_STB` 与 GL 相同的
  编译定义）加载 PNG；失败/空路径回退内置（stderr 警告照 GL 文案）。
- sprite sheet 帧动画（`setSpriteSheet`）走 FragParams 既有字段，无需新机制。

### 2. 软粒子与折射（宿主纹理注入）

- 公开适配（`include/ember/vulkan.hpp` 增补）：
  `void setVulkanSoftDepth(ParticleBackend&, VkImageView depth, VkSampler sampler);`
  `void setVulkanRefractionInputs(ParticleBackend&, VkImageView color, VkImageView depth, VkSampler sampler);`
  借用语义、布局 `SHADER_READ_ONLY_OPTIMAL` 由宿主保证（注释写明）。
- 描述符：FragParams 所在 set 增补 binding 0/1/2 combined-image-sampler
  （或 sampled image + sampler 分离，任选并注释；注意 set 0/1 划分一致性）。
- `setRefractionParameters`/`setSoftParticleParameters` 已是公共层逻辑，
  本工单只需后端声明能力 + 渲染时绑定。折射 pass 状态对照 GL：
  像素替换（blend off）、depthTest 策略一致、两轮 drawParticles
  （refraction 位变体管线，WO-03 预留的 key 位启用）。

### 3. bloom

- 图像链：RGBA16F 全分辨率 ×1 + 半分辨率 ×2（对照 GL FBO 链），
  懒创建、尺寸变化重建；各自小 render pass（CLEAR load，无需保留）。
- 宿主深度遮挡：GL 用 `glBlitFramebuffer` 解析宿主深度到 bloom 深度附件；
  Vulkan 用 `vkCmdCopyImage`（深度格式一致时）否则 `vkCmdBlitImage`；
  宿主无深度附件时跳过（对照 GL 的 bits==0 分支）。
- 三 pass：threshold（BloomParams.threshold）→ blur ×2（方向交替）→
  composite 到宿主 target（blend ONE,ONE，管线变体独立）。
- 失败降级：任何资源创建失败 → 关闭 bloom + stderr 警告，不抛
  （对照 GL `ensureBloom` 的事务语义）。

### 4. 测试（`tests/vulkan_test.cpp` 扩展）

移植 effects.hpp 语义用例（每项像素级）：
- sprite：PNG 加载（`sprites/test.png`）与内置回退；sheet 帧推进。
- 软粒子：粒子贴近深度面淡出（对照 effects 的断言语义）。
- 折射：mode 0 偏移采样 + 覆盖度随寿命衰减；无深度时 mode 1 回退 0。
- bloom：亮斑产生光晕（阈值以上粒子周边像素增亮）；宿主深度遮挡保持。

### 5. 文档收尾

- `docs/backends.md`：接入顺序各步标记完成，"当前可运行的图形后端仍只有
  OpenGL"改为现状描述；模块表加 Vulkan 行。
- `README.md`/`README.en.md`："后端组织"节加 Vulkan 状态与
  `EMBER_BUILD_VULKAN` 选项；依赖表加 Vulkan-Headers/加载器（可选）。
- `INTEGRATION.md`/`INTEGRATION.en.md`：Vulkan 宿主接入节
  （makeVulkanDevice/setVulkanContext/setVulkanFrameTarget 的最小示例，
  坐标约定提醒：投影深度 [0,1]、负 viewport 已处理 Y）。

## 验收

```bash
cmake --build build -j
export PATH=/e/msys2/ucrt64/bin:$PATH
./build/ember_vulkan_test.exe        # 全部用例过，capabilities 全 true 断言
ctest --test-dir build --output-on-failure
ctest --test-dir build-core --output-on-failure
```

另做一次**双后端共存冒烟**：同一进程内先跑 GL 回归（ember_system_test）
再跑 vulkan_test，互不干扰（两个测试本就独立进程，这里写一个同进程
用例放 vulkan_test 尾部：GL context 与 VkDevice 同时存活各跑一帧）。

## 边界

- 不做 MSAA 深度解析（vkCmdResolveImage 的深度模式支持差，文档注明宿主
  需提供已解析深度）。
- 不做跨 API 互操作；不动单头文件（仍 GL-only，文档里写明）。
- 效果语义（公式、参数、降级行为）逐条对照 GL，不做"改进"。

## 完成记录

日期：2026-09-18

实现：

- **sprite 贴图**（`sprite.cpp`）：内置 64×64 径向渐变 CPU 生成经 staging 上传
  （linear/clamp）；`setSpriteTexture` 用模块内静态 stb（避免与 GL 后端链接冲突）
  加载 PNG，失败/空路径回退内置并打印与 GL 相同措辞的警告；sheet 帧动画走
  FragParams 既有字段。
- **软粒子/折射**：render set binding 4/5 接入借用宿主 view/sampler
  （`updateRenderSet`）；`drawParticles` 设 `useSoft`/`refr*`，折射模式 1 无深度
  时回退 0；折射 pass 在 bloom 合成后以像素替换管线叠加到宿主目标。
- **bloom**（`bloom.cpp`）：RGBA16F 全分辨率 bloomFull_ + 半分辨率 bloomHalf_/
  bloomBlur_ 图像链、各自 render pass/framebuffer/sampler，尺寸或宿主深度格式
  变化重建；粒子 pass 先画入 HDR 目标（宿主深度 view 直接作深度附件，`loadOp=LOAD`
  保留遮挡），再画宿主；threshold→blur×2（方向交替）→ composite（blend ONE,ONE）；
  资源失败降级关闭 bloom + 警告，不抛。
- `options.cpp`：`capabilities()` 全开 true。
- 文档：`docs/backends.md` 接入顺序 1-4 全部标记完成、模块表补 Vulkan 行；
  `README.md`/`README.en.md` 后端组织与依赖表更新；`INTEGRATION.md`/
  `INTEGRATION.en.md` 新增 Vulkan 宿主接入节（makeVulkanDevice/setVulkanContext/
  setVulkanFrameTarget 示例、坐标约定、借用语义、bloom 遮挡、单头文件 GL-only）。

验收输出摘要：

- `./build/ember_vulkan_test.exe`：新增 effects 5 条用例全过——PNG/内置/sheet、
  软粒子贴近深度淡出、折射简单模式采样+无深度回退、bloom 光晕+宿主深度遮挡、
  同进程 GL/Vulkan 双后端冒烟；`capabilities` 全 true 断言。
- `EMBER_VK_DEBUG=1` 零 validation 输出；`ctest --test-dir build` 10/10、
  `build-core` 6/6 全绿。

遗留问题：

- 不做 MSAA 深度解析：宿主需提供已解析深度（文档已注明）。
- bloom 的阈值/模糊只在 RGBA16F 上进行，不接管宿主交换链；单头文件保持 GL-only。
- 帧动画/折射贴图取自 `sprites/test.png` 等测试资源，未新增示例程序。


---

## 验收修复记录（2026-09-18，审查人：主模型）

交付时功能测试全绿，但 validation layer（`EMBER_VK_DEBUG=1`）抓到规范级问题，
逐一修复：

1. **scheduleBuf_ 缺 `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT`**：`vkCmdDispatchIndirect`
   的间接缓冲必须带 INDIRECT usage（WO-04）。
2. **录制中更新描述符（无 UPDATE_AFTER_BIND，命令缓冲失效）**：三处——
   `drawParticles` 内的 `updateRenderSet`、排序的 binding-12 重绑、bloom 的
   `setSource`。统一前移：`render()` 在 `vkBeginCommandBuffer` 之前完成
   `updateRenderSet` / `sortPrepare`（排序缓冲与描述符准备拆出）/
   `ensureBloom` / `updateBloomSets`；录制区内只留 vkCmd* 命令。
3. **bloom 单描述符集逐 pass 覆写（WAW 语义错误）**：四个 pass 共享一个 set、
   录制前全部覆写，执行时所有 pass 只会采到最后绑定的图。改为 4 个持久
   描述符集（full→half→blur→half），每帧录制前统一写入。
4. **drawParticles 多 pass 的 UBO 覆写**：host-visible memcpy 在单次提交前
   逐 pass 覆写，所有 pass 会读到最后一组值（折射/普通混合粒子场景出错）。
   改为 `vkCmdUpdateBuffer` 随命令流有序更新 + transfer→shader 屏障。
5. **bloom 合成管线缺 `pDepthStencilState`**：宿主 pass 带深度附件时
   规范要求显式（禁用的）深度状态。
6. 文档化限制：Vulkan 端软粒子与折射共用一个深度绑定，两者同时注入时
   必须使用同一深度图（GL 端允许不同纹理）。

验收结论：通过（ctest 10/10、build-core 6/6、validation 零输出）。
