2026-09-17 审查修复记录

编号对应 result.json 的 comments 数组，从 1 开始。重复的单头文件报告随源文件修复、重新生成一起处理；原有工作区中的折射功能和示例保留。

| 报告编号 | 处理 |
|---|---|
| 22–24、57、70 | 独立维护 allocated 范围与 alive 数量；追加槽位使用独立 CAS；死亡标记传播到下一缓冲；phase 2 生成存活索引，渲染/排序/读回不再假设存活粒子占连续前缀 |
| 25、60 | burst 与 emitter 的合并请求列表限制为 4096 条，粒子数量与实际接受的请求保持一致 |
| 26、59 | 补 GPU 写入到 CPU 读回/缓冲更新所需的 barrier |
| 42、58 | Spring 的 std430 数组步长改为 32 B，并断言字段偏移；明确转换 CPU 边界枚举为 Shader 的 1=kill、2=bounce |
| 27–30、32、33、61–64、67 | 修正视空间深度及软粒子符号；Bloom 保持宿主 FBO/视口/深度状态并处理场景遮挡；Shader 加载事务化，失败当帧回退，排序加载失败不再 dispatch |
| 31、65、68、69 | 移动操作保留自旋/折射状态；GPU seed 采用配置种子；公开 emitter 调色板编辑在 update 前同步 |
| 34、39–41、44–46、48、56、66 | 局部配置保留未指定参数，支持单独修改 soft_radius；拒绝负整数、溢出、非有限数、非法折射模式和段名；发射计数校验输入；CPU 出生支持折射符号与零轴回退；Force 移位前检查范围 |
| 3、36、37 | GLFW 回调边界捕获异常，在同线程下一次 pollEvents/setTitle/setCursorPos/swapBuffers/setVsync 重新抛出；采用有类型的 GLADloadfunc；setVsync 临时切换所属上下文并恢复原上下文 |
| 6–9、18、49 | 示例 FBO/renderbuffer 由析构释放；Shader 句柄覆盖分配异常路径；文件读入检查中途失败；示例配置成功 apply 后才提交；不完整 FBO 停止后续使用 |
| 4、5、10、16、17、38、50–55、71 | 统一窗口坐标与 framebuffer 像素坐标用途；零尺寸跳过绘制；FPS 使用真实时间；软粒子/编辑器尊重配置；滚轮保留幅度，射线求交拒绝无效命中；右键可拖动，零 rate 可恢复，尺寸按 dt 调整，导出 box extents；debug 预设与 example 同步 |
| 1、2、11–15、19–21、35 | 致命前置断言、非空/数量/有限值检查；GPU 对象先于上下文销毁；只把已识别的环境缺失标为 SKIP；隔离受力与折射测试，初始化深度数据；新增生命周期和像素敏感回归及 GL 错误检查 |
| 43、47、72–74 | 补公共头文件直接 include，清理未使用变量和生成器参数 |

新增像素回归还发现了 Bloom 全屏三角形 UV 被错误缩小一半的问题，已修正。

验证：

- Release + 编辑器开启：构建通过，CTest 5/5 通过。
- Release + 编辑器关闭：构建通过，CTest 5/5 通过。
- GPU 测试实际执行，没有跳过；本机为 NVIDIA GeForce RTX 4060 Laptop GPU。
- 共享回归覆盖持续死亡/回收、容量边界、存活索引唯一性、间接绘制数量、多弹簧、seed、调色板修改、局部配置、排序颜色顺序、Bloom 光晕、宿主 FBO/深度、多重采样深度遮挡、软粒子、折射/深度/热浪、移动语义、回调异常、内嵌 Shader 和部分 Shader 加载失败回退。
- 临时关闭 Bloom、排序、折射的三个变异版本均被新测试检测到。临时源文件及日志位于 build/mutation-check/，未改动正式实现。
- Shader 同步和单头文件生成一致性加入 CTest。

接入变化：

- 自定义 Shader 新增 SSBO binding 11（liveIndices），binding 4 新增第五个 uint（uAllocated，总计 20 B）。phase 2 需要生成存活索引和间接实例数；宿主在各阶段前将 indirect instanceCount 清零。
- Spring 数组步长为 32 B；深度 uniform 改为 uInvProj，上传 inverse(proj)；排序填充哨兵使用 uCapacity。
- GL 加载入口采用 GLADloadfunc。GLFW 可直接传 glfwGetProcAddress；返回 void* 的加载器通过适配函数转换返回值。中英文接入文档已更新。
- 稳定槽位方案额外使用每容量 4 B 的存活索引。积分扫描历史已分配范围，clear/重建可重置；每帧同步读回 20 B 计数。旧性能数据不能直接代表修复后的实现。

验证范围：此次没有进行跨 GPU/跨平台运行、磁盘故障或内存分配失败注入；这些异常路径经过源码修复，但不声称已做相应故障实验。原 result.json 保持不变。
