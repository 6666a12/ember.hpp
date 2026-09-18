# WO-10 — Vulkan：宿主 command buffer 录入

前置：WO-01~08 完成。本单落实 `docs/backends.md` "Vulkan 渲染环境注入（设计
定型）"第 4 条预留的扩展位（`docs/backends.md:111`）：宿主引擎把 ember 的
update/render 录进自己的 command buffer，后端在窗口内跳过自提交。不改动
`ParticleBackend` 公共契约与既有入口语义。

## 目标

新增一对公开 free function（与 `setVulkanContext`/`setVulkanFrameTarget`
同风格，实现在 `src/backends/vulkan/context.cpp`）：

```cpp
void beginVulkanFrame(ParticleBackend&, VkCommandBuffer cmd, std::uint32_t frameIndex);
void endVulkanFrame(ParticleBackend&);
```

`beginVulkanFrame` 与 `endVulkanFrame` 之间，`update()` 的 compute 分派链与
`render()` 的 render pass 录制进宿主 cmd；窗口内后端不 reset/begin/end/提交
自己的 command buffer、不 signal fence。`endVulkanFrame` 关闭窗口，恢复默认
模式（后端自提交）。宿主模式不是默认，不得改变不传 cmd 时的任何行为。

## 必读

- `docs/backends.md:104-112`（设计定型节，第 4 条是本单的定型依据）
- `include/ember/vulkan.hpp:33-55`（`VulkanFrameTarget` 布局契约注释，本单
  原样保留：进入 `render()` 时颜色 `COLOR_ATTACHMENT_OPTIMAL`、宿主深度
  `SHADER_READ_ONLY_OPTIMAL`，后端不发颜色/深度布局 barrier，返回时布局不变）
- `src/backends/vulkan/simulation.cpp:113-328`（update 全链路）
- `src/backends/vulkan/render.cpp:200-357`（drawParticles/render 全链路）
- `src/backends/vulkan/statistics.cpp:43-122`（fence 环统计，本单最大难点）
- `src/backends/vulkan/vulkan_backend.hpp:8-17`（FrameResources）、`:289-290`
  （frames_/frameIndex_）
- `src/backends/vulkan/resources.cpp:269-319`（createPerFrame：cmd 缓冲与 fence
  的创建点）、`:477-481`（waitFrame）
- `src/backends/vulkan/context.cpp:7-11,67-86`（requireVulkan 与公开适配模式）
- `src/backends/vulkan/sort.cpp:85`（sortParticles 已按 cmd 参数化）
- `src/particle_system.cpp:408`（`aliveCount()` 经 `synchronizeStatistics()`
  实现——宿主同步契约实际落在 synchronizeStatistics/readParticles 上）
- `workorders/README.md:39-57`（硬性规则）、`:87-109`（验收教训，第 1/4/5/7/10
  条与本单直接相关，见下）

## 设计定型（不要偏离）

### 录制路径改动（全部以 `hostMode()` 分支实现）

命令缓冲分配/录制/提交/fence 等待的现有位置（改动点就这些）：

| 位置 | 现有行为 | 宿主模式 |
|---|---|---|
| `simulation.cpp:124-126` | update 开头 `waitFrame(frameIndex_)` + `vkResetFences` | 跳过；改由 begin 内部做（见状态机） |
| `simulation.cpp:181-186` | `vkResetCommandBuffer` + `vkBeginCommandBuffer` | 跳过（宿主 cmd 已由宿主 begin） |
| `simulation.cpp:280-297` | 统计拷贝 → `vkEndCommandBuffer` → `vkQueueSubmit` + `submittedFrame` | 只录拷贝（照常进宿主 cmd）；不 end/submit，不写 `submittedFrame` |
| `simulation.cpp:299-306` | 同步模式等待 fence 并读回计数 | 不等待不读回；`countsFresh_=false`，`statistics_` 不前进 |
| `simulation.cpp:318` | `frameIndex_` 轮换 | 不轮换（槽位由宿主 frameIndex 决定）；`:319` 的 `++frameCount_` **保留**，updateSequence 契约不变 |
| `render.cpp:304-305` | render 开头 `waitFrame(slot)` + `vkResetFences` | 跳过 |
| `render.cpp:319-324` | reset + begin | 跳过 |
| `render.cpp:347-356` | end + submit + `vkWaitForFences` | 只跳过；录制内容全部进宿主 cmd |
| `simulation.cpp:47-48`（scheduleGpu 内部取 `frames_[slot].command`）、`render.cpp:202-203`（drawParticles 取 `frame.command`）、`render.cpp:319`（render 局部 cmd） | 从 FrameResources 取 cmd | 改经私有辅助 `VkCommandBuffer activeCmd(slot)`：宿主模式返回 `hostCmd_`，默认返回 `frames_[slot].command`。`sortParticles`（sort.cpp:85）与 `runBloom` 已按 cmd 参数化，无需改签名 |

澄清（对初稿的修正）："跳过 command buffer 分配"指跳过每帧的
reset/begin/end/submit/fence 操作；command pool 与 per-frame cmd buffer
照常创建（默认模式仍用，`createPerFrame` 与销毁路径不动）。

不需要新描述符、新缓冲、新管线；描述符池不扩（门禁第 3 条不涉及）。

### 描述符更新的宿主模式例外（门禁第 7 条的精确适用）

门禁第 7 条（`workorders/README.md:101-106`）：`vkUpdateDescriptorSets` 一律
在 `vkBeginCommandBuffer` 之前。宿主模式下宿主 cmd 在 `beginVulkanFrame`
前已由宿主 begin，而 update()/render() 在窗口内仍要执行
`updateSimSet`（simulation.cpp:136）、`updateRenderSet`/`sortPrepare`/
`updateBloomSets`（render.cpp:312-317）。这是**有意的例外**，规格上合法：
录制中（未提交）的 cmd 引用的 set 可以更新；唯一禁止的是更新仍被已提交未
完成 cmd 使用的 set。该前提由两条保证，缺一不可：

1. `beginVulkanFrame` 内部对 `hostFrameSlot_ = frameIndex % kFramesInFlight`
   调 `waitFrame`（resources.cpp:477-481），把宿主编录与后端最后一次自提交
   序列化（gpuDriven 默认提交可能仍在飞，不 wait 就更新同槽 set = 门禁第 7
   条违规）。宿主模式**不** `vkResetFences`——fence 保持上一次自提交后的
   signaled 状态，默认模式恢复后 waitFrame 立即返回，再 reset/submit，语义
   闭环。
2. 宿主遵守 `kFramesInFlight=2` 帧环纪律（公开契约，写进
   `beginVulkanFrame` 的注释）：槽位 S 第 N 帧的宿主 cmd 执行完成前，不得为
   第 N+2 帧录制同槽资源（`spawnStaging` 在录制期被 memcpy，
   simulation.cpp:178；per-frame UBO 内容被覆写，resources.cpp:310-317）。

### 状态机契约（非法时序一律抛 `std::logic_error`）

- `endVulkanFrame` 无对应 begin → `logic_error`。
- `beginVulkanFrame` 时窗口已开（嵌套/并行录制）→ `logic_error`；
  `cmd == VK_NULL_HANDLE` → `logic_error`。一次只能录一条宿主 cmd。
- `frameIndex` 任意 uint32 合法，槽位取 `frameIndex % kFramesInFlight`。
- 窗口内（begin 之后、end 之前）调用以下入口 → `logic_error`：
  `setVulkanFrameTarget`、`setVulkanContext`、`setGpuDriven`、`setSortEnabled`、
  `resize`、`clear`、`setSpriteTexture`、`synchronizeStatistics`、
  `readParticles`（经 facade 的 `aliveCount()` 同理）。**允许**的窗口内入口
  只有 `update()`、`render()`、`pollStatistics()`。
  注意修正初稿表述：宿主模式的 `render()` 仍需要帧目标，所以
  `setVulkanFrameTarget` 只在窗口外禁止/允许关系上要分清——**窗口外
  （begin 前或 end 后）允许且必需，窗口内 logic_error**。
- 槽位一致性：`render()` 用 `target_.frameIndex % kFramesInFlight`
  （render.cpp:302，注意它与 update 的内部 `frameIndex_` 是两个来源）选
  帧槽；宿主模式下 `target_.frameIndex % kFramesInFlight != hostFrameSlot_`
  → `logic_error`。
- 宿主在 `update()`/`render()` 调用期间不得持有 active render pass
  （render pass 嵌套非法）。无法在后端廉价校验，写进公开注释为契约违反，
  validation layer 会抓。
- 析构/backend 销毁前窗口必须已关闭；未关闭销毁是宿主契约违反（UB）。

### 同步契约

- 宿主模式下 `readParticles`/`synchronizeStatistics`/`aliveCount` 需要 GPU
  完成，而后端不提交命令、无从等待——**宿主责任：调用这些入口前先提交并
  等待宿主 cmd**（写进 `beginVulkanFrame`/`vulkan.hpp` 注释与
  INTEGRATION 文档）。后端在宿主模式下对这三个入口不自己提交任何命令。
- `pollStatistics()` 非阻塞语义不受影响（见下节退化规则，不等待、不撒谎）。

### 统计 fence 环在宿主模式下的退化规则（本单核心设计，照做）

现状（`statistics.cpp`）：gpuDriven 时 `enqueueStatistics`（:102-122）预订
4 槽环的一槽，`slot.frame=frameCount_+1`（:111）、`slot.frameSlot=frameIndex_`
（:112）、`slot.submission=frameCount_+1`（:117）；`drainStatistics`（:77-100）
用 `frames_[slot.frameSlot].fence` 的 signal 状态或槽位复用（`submittedFrame
<= slot.submission`，:80-88）判定拷贝完成。这套判定在宿主模式下**全部失效**：
fence 只在自提交时 signal，宿主提交后端不可见；更危险的是 fence 预置
SIGNALED（resources.cpp:278-280）且 `submittedFrame` 停在旧值，
`vkGetFenceStatus` 会对未完成的拷贝返回成功 → 读到垃圾数据。规则：

- **H1（样本分型）**：`StatisticsSlot` 增加 `bool hostSample`（或等价哨兵）。
  `enqueueStatistics` 按当前模式置位。环中允许默认样本与宿主样本共存。
- **H2（poll 永不判定宿主样本）**：`drainStatistics(false)`（poll 路径）对
  `hostSample` 一律跳过完成判定；`pollStatistics()` 返回最后一次已 drain 的
  `statistics_`。滞后是契约允许的（"样本可以滞后"），撒谎不是。
- **H3（精确查询 = 宿主完成观察点）**：`synchronizeStatistics()`（
  `aliveCount()` 经它实现，particle_system.cpp:408）与 `readParticles()` 在
  宿主模式下无条件 drain 全部 `hostSample`（不查 fence、不等待——等待已由
  宿主在调用前完成，见同步契约）。gpuDriven 路径 drain 后按
  `slot.frame > statistics_.frame` 取最新（:90-97 逻辑照旧成立）。
  **同步（非 gpuDriven）路径**：update 只录制计数器拷贝
  （simulation.cpp:280-288 照常进宿主 cmd），置 `countsFresh_=false`、
  `statistics_` 不前进；H3 drain 时 `refreshCounts()`（:43-52）读
  `readbackBuf_`——队列按序执行 + 宿主已等待，缓冲里必是最后一次录制的
  拷贝（即最新帧），`:51` 的 `statistics_={frameCount_,...}` 天然正确。
  `refreshCounts` 在宿主模式下只允许在 H3 门内执行（readParticles 在门内；
  窗口内的模式切换已被状态机禁止，setGpuDriven(false) 的 refreshCounts 调
  用不会撞上未等待的宿主拷贝）。
- **H4（end 不 drain）**：`endVulkanFrame` 不做任何统计 drain——此刻宿主
  cmd 尚未提交，读了必错。宿主样本只能被 H3 drain。
- **H5（满环丢样不变）**：宿主模式下若宿主从不调精确查询，4 槽用满后
  `droppedStatistics_` 照增（:121），与默认行为一致。

### 屏障与布局

- update 的 buffer 屏障（`simulation.cpp` 的 memoryBarrier/transferToShader/
  `HOST_WRITE→TRANSFER` :190-196）照常录进宿主 cmd，宿主队列按序执行即正确
  （基线单队列，门禁第 4 条）。
- 图像布局契约不变（`vulkan.hpp:33-38`）：宿主在 cmd 里自行把目标迁到
  `COLOR_ATTACHMENT_OPTIMAL`；ember 的 render pass
  initial/finalLayout 同为 `COLOR_ATTACHMENT_OPTIMAL`（render.cpp:19-20），
  begin/end 录进宿主 cmd 合法且不产生布局变化。宿主深度附件同理
  （render.cpp:31-32）。
- bloom 的中间 pass（`runBloom`，bloom.cpp）与折射 pass 同在
  `drawParticles` 内 begin/end render pass，一并进宿主 cmd，无额外规则。

## 实现要求

1. `include/ember/vulkan.hpp`：声明两个 free function，注释写清窗口语义、
   帧环纪律（kFramesInFlight=2）、同步契约（精确查询前宿主须提交并等待）、
   render pass 禁止嵌套、槽位一致性、非法时序抛 `std::logic_error`。
2. `context.cpp`：`requireVulkan` 后转发到 backend 私有方法
   `beginFrame(cmd, frameIndex)` / `endFrame()`，状态校验在 backend 内做。
3. `vulkan_backend.hpp`：新增成员 `VkCommandBuffer hostCmd_ = VK_NULL_HANDLE`、
   `std::uint32_t hostFrameSlot_ = 0`；`bool hostMode() const`；私有
   `activeCmd(slot)`；`StatisticsSlot` 加 `hostSample`。
4. `beginFrame`：窗口已开 → `logic_error`；`cmd` 空 → `logic_error`；
   `waitFrame(frameIndex % kFramesInFlight)`；记录 `hostCmd_`/`hostFrameSlot_`。
   `endFrame`：未开窗 → `logic_error`；清 `hostCmd_`。
5. update()/render()/drawParticles()/scheduleGpu() 按上表加 `hostMode()`
   分支；`activeCmd` 统一取 cmd。宿主模式跳过处不得留下半提交状态：
   `submittedFrame`/`recorded`/`frame.fence` 一律不碰。
6. 统计按 H1-H5 改 `statistics.cpp`；`enqueueStatistics` 置 `hostSample`。
7. `debug_` 帧日志（simulation.cpp:321-327）在宿主模式下跳过（计数未读回，
   打印必是垃圾）。
8. 不改 `ParticleBackend` 契约、不改 GL、不碰 shaders/（无
   `sync_embedded_shaders.py`）；改了 include/ 照例
   `python tools/amalgamate.py`（vulkan.hpp 不在单头内，运行只为守门禁习惯）。
9. 代码风格跟随现有后端：C++17、英文注释只写非显然的 why、命名空间
   `ember::detail::vulkan`（内部）/ `ember`（公开适配）。

## 测试

全部加在 `tests/vulkan_test.cpp`，复用 OffscreenTarget 的
beginOneShot/submitOneShot（tests/vulkan_test.cpp:142-165）：

1. **宿主 update 对拍**：默认/宿主两个系统同种子同指令流（含短寿命死亡
   帧）跑 16 帧；宿主每帧 `beginOneShot` → `beginVulkanFrame(sys.backend(),
   cmd, f)` → `sys.update(dt)` → `endVulkanFrame` → `submitOneShot(cmd)`
   （= 提交+等待）→ 两系统 `readParticles()` 多重集一致、`aliveCount()` 一致。
2. **宿主 render 像素一致**：宿主模式把 `sys.render(...)` 录进宿主 cmd，
   提交等待后 `pixels()` 与默认模式渲染结果逐字节一致（同目标先 clear）。
3. **非法时序**：未 begin 就 end、begin 嵌套、窗口内
   `setVulkanFrameTarget`、窗口内 `synchronizeStatistics`、
   窗口内 `readParticles`、`target_.frameIndex` 与 begin frameIndex 槽位
   不一致 → 各抛 `std::logic_error`（逐条 check）。
4. **混合模式**：单系统第 f 帧宿主（frameIndex=f）/ 第 f+1 帧默认交替 8 帧，
   与全默认参照系统逐帧比 `aliveCount()`/`readParticles()` 多重集一致；最终
   渲染像素一致；默认模式帧渲染/统计行为无回归。
5. **宿主 gpuDriven 统计**：`setGpuDriven(true)` + 宿主模式录 6 帧（期间
   窗口内 `pollStatistics()` 序号不得越过窗口起点）；提交等待后
   `synchronizeStatistics()` 返回最新样本且 alive 正确；满环丢样计数值
   单调不减。
6. 现有用例全绿（行为套件只跑默认模式，不应受影响）。

## 验收

```bash
cmake --build build -j && cmake --build build-core -j
export PATH=/e/msys2/ucrt64/bin:$PATH
ctest --test-dir build --output-on-failure      # 全绿，含新用例
ctest --test-dir build-core --output-on-failure
EMBER_VK_DEBUG=1 VK_LAYER_PATH=/e/msys2/ucrt64/bin ./build/ember_vulkan_test.exe
EMBER_VK_DEBUG=1 VK_LAYER_PATH=/e/msys2/ucrt64/bin ./build/ember_cross_backend_test.exe
```

前两条 validation 必须零输出（门禁第 4/10 条）。

文档：

- `docs/backends.md`：设计定型节第 4 条改为已落地（一句话语义 +
  `beginVulkanFrame`/`endVulkanFrame` 签名）；"Vulkan 的选择性接入顺序"
  追加第 5 步（WO-10 宿主 cmd 录入）。
- `INTEGRATION.md:347` 起 "Vulkan 宿主接入" 与 `INTEGRATION.en.md:347` 起
  对应节：各加一段宿主 command buffer 接入示例（begin → update/render →
  end → 宿主提交等待 → 精确查询），并写明同步契约与帧环纪律。

## 边界

- 不做多队列、不做并行录制（一次只能录一条宿主 cmd，嵌套 begin 即拒绝）、
  不做交换链接管、不做 GL 侧对应物（GL 天然宿主即时模式）。
- 不做宿主 fence/semaphore 注册（保持 `endVulkanFrame(backend)` 无参签名；
  完成通知就是宿主自己的提交+等待）。
- 不把宿主模式做成默认；不传 cmd 的行为逐位不变。
- 不改渲染路径、不改排序、不改统计槽数（4 槽定型值）。

---

## 完成记录

- 日期：2026-09-18
- 环境：Windows + MSYS2 ucrt64（Vulkan 1.4 驱动，NVIDIA RTX 4060 Laptop）。

### 交付物

- `include/ember/vulkan.hpp`：`beginVulkanFrame(backend, cmd, frameIndex)` /
  `endVulkanFrame(backend)` 声明 + 窗口语义、帧环纪律、同步契约与非法时序注释。
- `src/backends/vulkan/context.cpp`：两个 free function 转发到
  `VulkanBackend::beginFrame/endFrame`。
- `src/backends/vulkan/vulkan_backend.hpp`：`hostCmd_`/`hostFrameSlot_`、`hostMode()`、
  `activeCmd(slot)`/`activeSlot()`、`beginFrame`/`endFrame`、`setFrameTarget` 移出内联；
  `StatisticsSlot::hostSample`。
- `resources.cpp`：`activeCmd`/`beginFrame`（对 `frameIndex%2` `waitFrame`，不 reset fence）/
  `endFrame`/`setFrameTarget`；`resize`/`clear` 窗口内抛 `logic_error`。
- `simulation.cpp`：`update()` 宿主分支（用 `activeSlot()`、跳过 fence/wait/begin/end/submit、
  只录计数器拷贝、`countsFresh_=false`、不轮换 `frameIndex_`、不打印 debug）；
  `scheduleGpu` 经 `activeCmd`；`setGpuDriven` 窗口内抛 `logic_error`。
- `render.cpp`：宿主分支（槽位一致性检查、跳过 wait/begin/end/submit）；
  `drawParticles` 经 `activeCmd`。`sort.cpp`：`setSortEnabled` 窗口内抛 `logic_error`。
  `sprite.cpp`：`setSpriteTexture` 窗口内抛 `logic_error`。
- `statistics.cpp`：H1 `hostSample` 分型；H2 `poll` 跳过宿主样本；H3
  `synchronizeStatistics`/`readParticles` 宿主外无条件 drain 宿主样本
  （同步路径 `refreshCounts` 读宿主已等待的 `readbackBuf_`），窗口内抛 `logic_error`；
  H4 `end` 不 drain；H5 满环丢样照旧。
- `tests/vulkan_test.cpp`：宿主 update 逐帧 alive + 多重集对拍、宿主 render 逐字节一致、
  非法时序逐条例、混合模式（宿主/默认交替）、宿主 gpuDriven 统计。

### 验收输出摘要

```text
ctest --test-dir build --output-on-failure         # 11/11
ctest --test-dir build-core --output-on-failure     # 6/6
EMBER_VK_DEBUG=1 ember_vulkan_test / cross_backend_test：零 validation 输出
```

### 遗留

- 宿主模式不接管宿主 fence/semaphore（完成通知就是宿主自己的提交+等待）；
  不做多队列、并行录制或交换链接管。
- `readParticles()` 仍会为设备本地粒子做一次一次性 staging 拷贝提交（工具路径）；
  这是窗口关闭后调用的，不受宿主模式“不自提交”约束影响。

---

## 验收修复记录（2026-09-18，审查人：主模型）

交付时全绿。深审发现并修复：

1. **render() 的 `alive_==0` 早退在宿主模式用陈旧 CPU 计数**：宿主录制的
   update 不同步读回，alive_ 停在 0 时宿主 render 静默不画。早退加
   `!hostMode()` 条件，附回归用例（stale-alive render）。
2. **`setGpuDriven(false)` 的 refreshCounts 可读未执行宿主拷贝**（静默垃
   圾）：新增 `hostDirty_` 标记——endFrame 置位，同步模式 update 的 fence
   等待/clear/resize/readParticles 的 queueWaitIdle 清除；hostDirty 时模式
   切换跳过 refreshCounts。
3. `setVulkanContext` 补窗口内 logic_error（契约字面一致，原本仅靠
   "context already set" 兜底）。

测试补强：混合模式补最终渲染像素一致断言（并把该用例粒子移到相机前方，
原 fixture 的像素比较会是空真）、宿主 gpuDriven 统计的 poll 断言改为有判
别力的形式（窗口内 polled.frame 必须停在窗口前基线，原断言对 H2 失效无
判别力）、新增 stale-alive render 回归。

验收结论：通过（ctest 11/11、build-core 6/6、validation 零输出）。
