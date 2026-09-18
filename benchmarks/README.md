# 性能基线

本目录定义 ember 的 workload v1、OpenGL 测量工具和第一份实测数据。后续 Vulkan、DirectX、Metal 后端应复用场景条件和结果格式；内部调度、内存布局和排序算法可以不同。

当前实测：[RTX 4060 Laptop 结果解读](baselines/2026-09-17-rtx4060/analysis.md) / [完整基线表](baselines/2026-09-17-rtx4060/report.md)。结果只代表记录的机器、驱动和工作负载，不作为跨机器的性能保证。

第一轮性能优化：[前后对比](baselines/2026-09-17-rtx4060-optimized/comparison.md) / [优化后完整数据](baselines/2026-09-17-rtx4060-optimized/report.md)。workload v1 与测量工具保持不变；内置积分改为遍历存活索引，出生批量分配，排序使用缓存深度键和 shared-memory tile。诊断阶段 `integrate` 现在包含幸存索引写入，`build_live` 只发布绘制参数；比较仿真收益时应结合全部仿真阶段及总完成时间。

GPU 调度对照使用 measurement v2：`--scheduling sync|gpu` 选择模式，所有模式逐帧将真实计数复制到 GPU 历史缓冲，最终 GPU drain 之后统一读取；采样循环不调用 `aliveCount()`。workload v1 保持不变，但不应把这批数据与旧 measurement v1 的差异全归因于调度方式。

最终实现的[GPU 调度与异步统计验证记录](baselines/2026-09-17-rtx4060-gpu-driven-final/comparison.md)包含相同源码的同步/异步模式、全部吞吐结果、CPU 调用时间和无时间戳对照。复测检测到其他程序占用 GPU，报告已明确标注竞争负载，不能把整轮耗时差异归因于实现。

## 运行

在仓库根目录，使用 Release 构建：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEMBER_BUILD_BENCHMARKS=ON
python tools/run_benchmarks.py --build-dir build --output out/baseline-local --scheduling sync
python tools/run_benchmarks.py --build-dir build --output out/baseline-gpu --scheduling gpu
```

离线构建沿用项目的 `EMBER_DEPS_DIR` 配置。Visual Studio 等多配置生成器由脚本构建并运行 Release 目录中的可执行文件。默认不构建基准；开启基准也不会改变正常 `ember` 库的编译宏或公共 API。

脚本自动构建两个可执行文件，串行运行三个测量组：

- `plain`：链接正常库，仅包围整帧与 render 的 GPU 时间戳；用于总耗时比较。
- `stages`：链接单独编译的插桩库；记录积分、出生、存活索引、计数读回、排序、基础绘制、Bloom 和折射阶段。用于定位瓶颈，不直接替代正常库数据。
- `plain_no_timers`：正常库，完全关闭 GPU 时间戳。用于检查计时器扰动，保留 CPU 时间和整个窗口的完成时间。

默认参数为 1280×720、容量 10,000 / 100,000 / 1,000,000、每个场景预热 120 帧、采样 240 帧、重复 3 次。三个测量组总计 297 个 case、71,280 个采样帧。结果目录必须不存在，避免把新旧数据混在一起。

快速检查所有场景，不作为正式性能结论：

```sh
python tools/run_benchmarks.py --output out/benchmark-quick --counts 10000 --warmup 5 --frames 24 --repeats 1
```

可以用 `--scenarios dense,sparse,sparse_control`、`--variants plain,stages` 缩小范围；用 `--note` 记录供电、功耗模式、后台任务等条件。正式采样建议接通电源、关闭其他 GPU 工作负载，保持分辨率和驱动设置一致。工具记录当前电源方案，但不修改电源设置、锁频或推断实际供电状态。

直接运行 `build/ember_benchmark_plain --help` 可查看可执行文件参数。Windows 下文件名带 `.exe`。环境变量 `EMBER_DEBUG` 必须取消设置，即使它的值为 `0`；库目前按变量是否存在启用日志。

## 工作负载契约 v1

共同条件：固定 `dt=1/60 s`，seed=20260917，每次重复重新构建粒子系统。粒子尺寸为 0.018 世界单位，出生区域为中心在原点、半边长 `(6,3,2)` 的 Box，初速度为零，颜色 `(2,1,0.5,0.25)`。使用程序光斑，非 sprite 图片；除专门说明外，力场、排序、Bloom、折射关闭，采用 additive 混合。

摄像机位于 `(0,0,12)`、看向原点、上方向 `(0,1,0)`；透视 FOV 60°，near=0.1、far=100。目标为线性 RGBA16F + D24，单采样、深度测试开启、深度写入关闭，无 sRGB 转换。固定尺寸的离屏 FBO 与窗口 DPI 无关；隐藏窗口只用于提供 GL 上下文，不交换缓冲、不等待垂直同步。

| 场景 | 工作负载 | 用途 |
|---|---|---|
| `dense` | 填满 N 个长寿命静止粒子，之后无出生/死亡 | 基础积分、存活索引、绘制成本 |
| `forces` | dense + gravity `(0,-0.1,0)`、linear drag 0.05、turbulence 0.25 | 力场计算成本 |
| `churn` | 出生率 N/2 每秒，寿命 `2+dt/2` 秒，预先运行 240 次 update 至稳态，再正常预热 | 持续出生、退休与槽位回收；实际 alive 逐帧记录 |
| `burst` | 每 120 帧一个周期，前 10 帧均匀发射共 N 个，寿命 0.5 秒，随后全部死亡 | 洪峰、退休、空系统；按周期位置保留原始数据 |
| `sparse` | 先填满 N，再让 99% 死亡，留下 N/100 个；历史已分配范围仍为 N | 历史高水位扫描成本 |
| `sparse_control` | 同样容量 N，仅填入 N/100 个长寿命粒子 | 与 sparse 对照，保持存活量和显存容量相同 |
| `sort` | dense + normal alpha 混合 + 每帧远到近排序 | 透明粒子排序；不能用近似 OIT 替换后直接比较 |
| `bloom` | dense + Bloom，阈值 0.5 | 深度复制/附件准备、额外粒子绘制、提亮/模糊/合成 |
| `refraction_simple` | 全部粒子 refractive，glass 预设、spin=1、mode=0 | 基础玻璃折射 |
| `refraction_depth` | 同上，mode=1，提供有效深度纹理 | 深度感知偏移；当前 IOR 未参与计算 |
| `refraction_noise` | 同样 glass 参数，仅 mode=2 | 隔离噪声偏移模式的成本；不是完整 heat 预设 |

长寿命为 1,000,000 秒；初始化每批最多出生 10,000 个，调用 `update(0)`，不进入测量。稀疏场景的临时粒子寿命 0.05 秒，通过一次 `update(0.1)` 清除。`maxSpawnPerFrame=max(10000,ceil(N/10))`。这些初始化调用也会推进 GPU 出生种子序列，移植时需要保留逻辑或单独注明不同的初始化方式。

折射输入是一张不可变的 RGBA8 渐变/棋盘纹理，深度纹理为 D32F、对应 view-space z=-16。输入纹理与绘制目标分离，避免采样正在写入的纹理。基础目标每帧清空为 `(0.02,0.025,0.04,1)`；不测量宿主场景制作成本。此场景测量折射代码路径，不代表完整游戏画面的合成性能。

burst 预热后清空，再从周期第 0 帧开始测量；每个完整 120 帧周期都有填充、存活、退休和空闲阶段。摘要额外给出 fill `[0,10)`、hold `[10,30)`、retire `[30,41)`、empty `[41,120)` 四段统计。不要只用 burst 的整段 P50 判断洪峰性能。若更改采样帧数，正式比较应使用完整周期。

重复之间交替反转场景顺序，减少固定顺序偏差；容量仍按参数顺序运行。默认的 3 次重复用于暴露运行波动，不能消除温度、动态频率和操作系统调度的影响。

## 指标解释

所有耗时单位为毫秒。原始数据的缺失阶段表示没有执行，不能当作 0。

| 指标 | 含义 |
|---|---|
| `cpu_frame_ms` | 从调用 update 前到 render 返回的 CPU 墙钟时间，包含基准包围计时开销；不含目标清空、结果采集和最终 drain |
| `cpu_update_ms` / `cpu_render_ms` | 对应 API 调用及少量场景调度代码的墙钟时间；包含驱动调用和等待，不是纯 CPU 计算时间 |
| `cpu_readback_ms` | sync 为同步计数读回；gpu 为零超时检查已完成快照、排队计数拷贝与 fence。均属于 cpu_update，不能等同于 GPU 仿真耗时 |
| `gpu_frame_span_ms` / `gpu_render_span_ms` | 命令流中的两个 GPU 时间戳之差；包含时间戳之间 GPU 等待 CPU 提交的空闲时间，不是 GPU busy time |
| `gpu_integrate_ms` / `gpu_spawn_ms` / `gpu_build_live_ms` | 对应仿真阶段及 barrier 的 GPU 时间戳区间；空 dispatch 的包围命令仍有成本 |
| `gpu_sort_ms` / `gpu_draw_ms` | 排序、宿主上的普通粒子绘制；折射场景的普通 pass 也会遍历并剔除折射粒子 |
| `gpu_bloom_prepare_ms` | Bloom 目标准备、深度附件处理/复制、清空；目前包含每帧深度附件分配 |
| `gpu_bloom_draw_ms` / `gpu_bloom_post_ms` | 离屏粒子绘制，以及提亮/降采样/模糊/合成 |
| `gpu_refraction_ms` | 单独折射 pass |
| `window_wall_ms / measured_frames` | 整个采样窗口到最后一帧 GPU 完成的平均墙钟时间；包含目标清空、glFlush、查询采集和最终 drain，measurement v2 另含计数历史拷贝，更适合比较持续处理成本 |

阶段时间可能嵌套，CPU/GPU 也会重叠，不能把它们相加当作帧耗时。特别是当前同步读回可能等待前一帧渲染及本帧仿真；数据不会把这种等待误标为纯提交成本。

GPU 时间戳查询对象在采样前分配，至少延迟 4 帧检查可用性。未就绪时继续运行，不阻塞读取、不复用未完成的查询。只在预热结束和测量窗口结束各调用一次 `glFinish`；测量期间没有为计时而逐帧等待 GPU。sync 模式保留库本身的同步计数读回；gpu 模式使用异步快照，精确计数校验由测量后的 GPU 历史读回完成。原始 case 的 `dropped_statistics` 记录整个系统生命周期（含预热）跳过的异步采样数。

P50/P95/P99 使用 nearest-rank，`sorted[ceil(p*n)-1]`，不删除离群值。逐帧指标跨重复汇总，同时保留每次重复的完成时间和 CPU 帧时间中位数。阶段分位数仅统计该阶段实际执行的帧，因此 burst 的绘制阶段 P50 可能大于整段帧时间 P50；请结合指标的 count 和周期分段读取。关闭计时器的对照组与其他组串行运行，组间差异同时包含硬件状态变化，不能直接精确扣除为“计时器成本”。

## 输出与校验

- `summary.json`：环境、源码 SHA-256 清单、Git 状态、构建命令/编译器选项、可执行文件哈希、驱动和可用的 NVIDIA 遥测，以及聚合指标。
- `report.md`：可直接阅读的总表、分阶段表和计时器对照表。
- `plain.json.gz` / `stages.json.gz` / `plain_no_timers.json.gz`：完整逐帧数据，无损 gzip；schema_version=1。
- `*.log`：各组执行日志。

没有 GL 上下文、Shader 文件缺失、功能回退、错误的存活量、空画面、GL 错误、查询数据异常、遗漏重复或运行中源码改变，都会使基准失败。失败目录不包含正式汇总；可查看日志和 `.partial.json` 定位。画面预检仅证明绘制发生且数值有限；效果正确性仍由 system/single-header 回归测试负责。

构建时启用测试后运行 `ctest --test-dir build --output-on-failure`。统计测试会验证分位数定义，并拒绝缺帧、重复 case、NaN、缺失插桩等损坏数据；开启基准构建时还会执行一个小规模 GPU 冒烟测试。CI 不设置跨硬件统一的耗时门槛。

## 后续后端的比较规则

1. 保留 workload_version、固定步长、场景尺寸、寿命、出生数量、相机、分辨率、目标格式和混合/效果语义。修改其中任一项应升级 workload_version。
2. 实现相同的公共指标名；GL 特有的 `readback`、`build_live` 等诊断阶段可由新后端扩展或省略。使用各 API 的 GPU timestamp 和异步结果采集机制，记录时间戳频率/有效位数。异步粒子计数应携带采样帧号、延迟附加到对应记录，避免为了统计额外同步 GPU。
3. 两边均采用 Release、关闭调试/validation 和垂直同步，保留预热与完成时间。先确认存活量和图像效果等价，再比较耗时；相同 seed 不承诺不同 API/驱动的浮点或原子执行顺序逐位相同。
4. 在同一 GPU 上比较 OpenGL/Vulkan/DirectX，才较能隔离后端差异。Apple 上的 Metal 数据应作为平台基线，注明硬件差异，不能直接声称差值全由 API 带来。
5. 本工具目前测量单队列、单粒子系统、固定粒径的离屏负载；不覆盖宿主渲染、呈现延迟、异步计算重叠、多系统竞争、大粒径高 overdraw、移动端带宽或显存峰值。那些需求应新增明确的场景版本。
