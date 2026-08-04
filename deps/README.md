# 离线依赖包

本目录存放 ember 构建所需的第三方依赖源码包，用于**受限网络环境**下的离线构建：

| 文件 | 来源 | 用途 |
| --- | --- | --- |
| `glad-v2.0.8.tar.gz` | https://github.com/Dav1dde/glad (tag v2.0.8) | GL 加载器（构建期用 Python 生成 gl.c/gl.h） |
| `glfw-3.4.tar.gz` | https://github.com/glfw/glfw (tag 3.4) | 窗口封装（可选，示例用） |
| `glm-1.0.1.tar.gz` | https://github.com/g-truc/glm (tag 1.0.1) | 数学库（头文件式） |

使用方式（配置时无需联网，FetchContent 自动从本目录解包）：

```bash
cmake -S . -B build -DEMBER_DEPS_DIR=<本目录的绝对路径>
```

**注意**：glad 的生成器仍需要 Python 3 + `jinja2` 模块（`pip install jinja2`），以及首次运行时会从 Khronos 拉取 `gl.xml` 规范（生成的 gl.h 已缓存于构建目录，之后离线可用）。

依赖版本号与 `CMakeLists.txt` 中的 FetchContent 声明严格对应；升级依赖时请同步更新两处。
