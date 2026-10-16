# METIS 离线几何依赖

METIS 5.1.0 来自官方仓库 c9032886f6073478d1013b4b2d75d59071530394。
GKlib 来自官方记录的依赖 62de20c7f97c719abdc96d7c3e266a1f7cb52bc5。
保留 METIS 的 include、libmetis 和 GKlib 的必要源文件及头文件。
sources.json 是 SCons 和原生接口目标共同使用的源文件清单。

该库只链接 Cooker 和几何接口测试，普通编辑器、模板和导出客户端不链接它。
索引为 int32，实数为 float32。构建定义 USE_GKRAND、USE_GKREGEX、NDEBUG 和 NDEBUG2。
MSVC 另使用原生 TLS，并在 GKlib.h 中补充 io.h、process.h 和 pid_t。
metis.h 的四个整数范围宏增加未定义检查，避免覆盖现代 MSVC 的 stdint.h。
除这两个头文件的补丁外，C／H 内容保持上游版本；Git 按引擎规则将换行保存为 LF。

USE_GKRAND 使用库内随机数实现，避免其他 Cooker 任务改变 CRT 随机状态。
调用端使用固定种子 4321，并串行保护随机数及信号注册的全局状态。
禁用库的磁盘交换和调试输出。图准备及子图重映射不持有该锁。
这些条件不能证明与 UE 的预编译 METIS 得到相同字节或相同切分。

METIS 和 GKlib 的主要许可为 Apache 2.0，见 LICENSE 和两份 NOTICE.txt。
gkregex.c 与 gkregex.h 保留 LGPL 2.1 或后续版本的声明及 COPYING.LESSER。
随机数实现及 Microsoft 整数兼容头保留各自 BSD 声明；LICENSE-MT.txt 保存随机数许可全文。
许可副本中的两份独立文本来自 GKlib 官方仓库 LICENSES 目录。
不要将这些文件重新标记为 Veya 原创代码，也不要统一格式化上游源文件。

升级时固定两个上游提交，核对源文件清单、32 位 ABI、许可及 MSVC 补丁，
再分别构建 Cooker 和原生接口目标。任何编译或接口通过都不代表 macOS、实际层次质量或性能已验证。
