# 折射粒子（Refractive Particles）设计文档 v2

> 状态：已实现，2026-09-17 已修复审查问题。下文保留原始设计，当前 API/Shader 契约以 include/ember 和 INTEGRATION.md 为准。
> v2 修订：① config 键从 10 个精简到 3 个（遵循库"config 暴露高层、API 精细"的既有哲学）；② 抽出**通用 billboard 旋转/自旋**——不塞进折射，作为独立通用特性先做，折射顺带消费。

---

## 1. 背景与核心思路

普通 billboard 粒子正对相机，法线 ≈ 视线方向，折射偏移为零 → "透明但看不见"。玻璃渣的关键：

> **给每个粒子一个伪面片法线（facet normal）**，由 `hash(gl_InstanceID)` 推导——每片碎片朝向不同，折射偏移各不相同。

但**面片法线不是折射专属的**——它本质上是 billboard 旋转：给所有粒子加"随机静态角 + 年龄自旋"（落叶/纸屑/碎片都受益），折射只是把旋转后的朝向当法线用。**先做通用旋转，再在其上做折射**。

## 2. 通用 billboard 旋转（前置特性，造福所有粒子）

### API
```cpp
// 随机静态旋转角（hash(instanceID)）+ 随年龄自旋（弧度/秒）；speed=0 关闭
void setSpin(float speed = 1.f);
```
### 配置（遵循现有键风格：值即强度，0 = 关，同 `streak`）
```ini
[system]
spin = 1.0     # 0 = 关；>0 = 自旋速度（弧度/秒），随机静态角始终开启
```
### Shader（VS）
- `angle = hashAngle(instanceID) + age * spinSpeed`（`vel.w` = age 现成）
- quad 展开加 2D 旋转矩阵；折射时该旋转后的朝向即 facet 法线（视图空间）
- **零新粒子字段**；`uSpinSpeed` uniform（0 = 关，成本零）

## 3. 与现有架构的契合点

| 需要的机制 | 现状 | 说明 |
| --- | --- | --- |
| 屏幕 UV | ✅ `uViewportSize` | FS 已有 |
| 场景颜色纹理 | ➕ 宿主提供（同软粒子模式） | 宿主把背景渲染进 RGBA 纹理，每帧传入 |
| 深度感知折射 | ✅ `uSceneDepth` + `uInvViewProj` | 复用软粒子的场景点重建 |
| facet 法线 | ➕ = §2 的旋转朝向 | 零新字段 |
| 碎片出生 | ✅ SpawnRequest 架构 | 一帧几千片零成本 |
| 落地弹跳 | ✅ boundary bounce | 现成 |
| 碎片翻转 | ✅ §2 自旋 | 通用特性，折射免费获得 |

## 4. 合成策略（关键设计决策）

**折射粒子不参与混合，FS 内合成后以 alpha=1 替换像素**：

```glsl
vec3 original  = texture(uSceneColor, screenUV).rgb;
vec3 refracted = texture(uSceneColor, screenUV + off).rgb * uRefrTint;
vec3 rgb = mix(original, refracted, uRefrAbsorption);
frag = vec4(rgb, 1.0);   // 整像素替换，混合关闭
```

- 渲染时**关闭 GL_BLEND**（无双重混合）
- **排序不破坏画面**——折射路径可关排序（像素替换，重叠后画盖先画，玻璃可接受）
- **碎片形状来自 sprite alpha**（`texel.a < 0.02 → discard`），内容来自场景纹理——遮罩与内容解耦

## 5. 效果菜单（一套基础设施解锁）

| 效果 | 配方 | 暴露方式 |
| --- | --- | --- |
| **玻璃渣（主目标）** | facet 折射 + tint + fresnel + chroma + specular | API 预设 `RefractionSettings::glass()` |
| **热浪 / 失真** | 噪声偏移（FS 内置 hash 噪声）、absorption≈0 纯透镜 | 预设 `heat()` |
| **水珠** | fresnel 强 + 泛蓝 tint | 预设 `water()` |
| **棱镜彩虹** | chroma 拉满 | 预设 `prism()` |
| 落叶/纸屑/碎片（无折射） | 仅 §2 通用旋转 | `spin` 键 |

**预设走 API，不进 config**——config 只留开/关 + 强度（见 §7）。

## 6. API 设计

```cpp
// ---- 通用旋转（§2）----
void setSpin(float speed = 1.f);            // 0 = 关

// ---- 折射粒子 ----
struct RefractionSettings {
    bool enabled = false;
    GLuint sceneColorTex = 0;      // 宿主场景颜色纹理（RGBA，CLAMP_TO_EDGE）
    int   mode = 0;                // 0 简单偏移 | 1 深度感知（需 depthTex）| 2 噪声偏移（热浪）
    GLuint sceneDepthTex = 0;      // mode==1 需要；为 0 回退 mode 0
    float strength = 0.02f;        // 折射偏移强度（屏幕 UV 单位）
    float ior = 1.5f;              // 折射率（mode 1）
    glm::vec3 tint{1.f};           // 玻璃颜色吸收
    float absorption = 0.35f;      // mix(原色, 折射色, absorption)
    float fresnel = 2.f;           // 边缘增亮（0 = 关）
    float chroma = 0.f;            // 色散（0 = 关）
    float specular = 0.f;          // 高光闪烁（0 = 关）
    glm::vec3 lightDir{0.5f, 1.f, 0.3f};
    // 预设工厂（精细参数只经 API，config 不暴露）
    static RefractionSettings glass();  // tint 微蓝, chroma 0.15, specular 0.4
    static RefractionSettings heat();   // mode=2, absorption 0, 纯透镜
    static RefractionSettings water();  // fresnel 4, tint 蓝
    static RefractionSettings prism();  // chroma 1.0
};
void setOpenGLRefraction(ParticleSystem&, const RefractionSettings& s); // free function, ember/opengl.hpp
```

## 7. 配置键（精简到 3 个，与既有模式一致）

```ini
[system]
spin            = 1.0      # 通用旋转（§2）：0=关，>0=自旋速度
refraction      = true     # 折射开/关（默认关）
refraction_mode = simple   # simple | depth | noise（深度/噪声模式少用）
refraction_strength = 0.02 # 唯一常用调节项
```

对比 v1 砍掉的键：`ior / tint / absorption / fresnel / chroma / specular / light_dir / spin`——全部下沉到 API 预设。`refraction_spin` 取消（就是通用 `spin`）。

## 8. Shader 契约变更

### VS（particle.vert）
- 新增 `uniform float uSpinSpeed;` 与输出 `out vec3 vFacetNormal;`（视图空间）
- `angle = hashAngle(instanceID) + vel.w * uSpinSpeed` → quad 旋转矩阵；facet 法线 = 旋转后的朝向（折射关闭时输出占位）

### FS（particle.frag）
新增 uniform（`uSceneColor` 绑**纹理单元 2**；`uSceneDepth` 复用单元 1）：

```
uRefraction     int   0=关(普通pass) 1=折射pass（uniform 编码 = settings.mode + 1）
uRefrMode       int   0=简单 1=深度感知 2=噪声（settings.mode 原值）
uRefrStrength   float   uRefrIor float
uRefrTint       vec3    uRefrAbsorption float
uRefrFresnel    float   uRefrChroma float
uRefrSpecular   float   uRefrLightDir vec3（视图空间）
```

- mode 0：`off = normalize(vFacetNormal).xy * uRefrStrength`
- mode 1：采样 `uSceneDepth` → `uInvViewProj` 重建场景点 → `refract(视线, facetNormal, 1/ior)` → 投影回屏幕 UV
- mode 2：`off = noise(screenUV * k + t) * uRefrStrength`（热浪，无需 facet）
- 色散：R/B 通道偏移 `off×(1±chroma)` 三次采样；Fresnel：`pow(1-|dot(n,视线)|, power)`；高光：`pow(max(dot(reflect(-视线,n),lightDir),0),24)`
- 输出：`frag = vec4(mix(原色, 折射色, absorption), 1.0)`

### 渲染流程（render() 变更）
```
[现有] 普通粒子 → bloomFbo_（HDR）→ bright → blur → 叠加宿主 FB
[新增] 折射 pass → 直接画到宿主 FB（prevFbo，混合关闭，像素替换），在 bloom 合成后执行
```
折射粒子的高光靠 specular 项，不吃 bloom（v1 取舍，文档标注）。

### 纹理单元
| 单元 | 用途 |
| --- | --- |
| 0 | sprite（形状遮罩） |
| 1 | uSceneDepth（mode 1 / 软粒子复用） |
| 2 | uSceneColor（新增，折射） |

## 9. 实施计划

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| **step-1 通用旋转** | `setSpin` + `[system] spin`；VS 旋转矩阵 + facet 法线输出；所有粒子生效（落叶/纸屑立即可用） | 双测试绿；像素检查旋转生效；无 GL 错误 |
| **step-2 折射基建** | `RefractionSettings` + 预设工厂 + `setRefraction`；FS 简单模式（mode 0）+ tint/fresnel/chroma/specular；折射 pass（混合关闭、单元 2 绑定）；render() 布线 | 三测试绿；像素检查：折射像素 = 场景采样 × tint |
| **step-3 示例** | `example_glass` + `config/glass.ini`：ScenePass（背景→颜色纹理）+ 碎片雨（boundary 弹跳 + spin） | 像素检查 + 用户目视玻璃渣效果 |
| **step-4 深度/噪声模式** | mode 1（复用 uSceneDepth 重建，宿主无深度回退 mode 0）；mode 2（热浪噪声偏移） | 三模式像素检查 |
| **step-5 文档与测试** | README/INTEGRATION 双语折射+旋转章节、契约表更新（新 uniform/单元 2）；system_test / single_header_test 增补；重新生成单头文件 | 三测试绿；amalgamate --verify 通过 |
| **step-6 全量验证** | 三测试 + example_glass 冒烟 + 文档扫描 | 全部绿 |

## 10. 风险与待定决策

1. **场景耦合**：需宿主提供场景颜色纹理（与软粒子深度纹理同模式，示例展示最小集成）
2. **bloom 交互**：折射粒子绕过 HDR 链直出宿主 FB；高光靠 specular 项（v1 取舍）
3. **屏幕边缘越界**：场景纹理必须 `CLAMP_TO_EDGE`；偏移过大时边缘拉伸属正常
4. **深度感知精度**：折射平面近似在粒子视图深度，非物理精确——视觉够用
5. **排序**：折射路径像素替换语义下排序非必需（可选）；与普通粒子混排按需开启
