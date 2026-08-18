# MemScanner-TestTarget · 内存扫描器测试靶场

配合 [KPM-MemScanner-GG](https://github.com/2254061909/KPM-MemScanner-GG) 使用的安卓测试 App。
把各类「已知的靶子」放进 native 内存，界面上直接显示每个靶子的**地址 + 当前值**，
这样你在扫描器里搜到之后能一一对照，逐项验证功能是否正确。

所有可扫描数据都在 native 内存（`libtesttarget.so` 的 JNI 层分配），
分别落在 **堆 / 匿名 mmap / 模块 .bss** 三种区，地址稳定、不被 GC 搬动。

## 构建

- Android Studio 打开工程直接 Run；或命令行 `./gradlew :app:assembleDebug`
  （首次需 `gradle wrapper` 生成 wrapper，或用 AS 自带 Gradle）。
- 需要 NDK（工程锁 `26.3.11579264`）、compileSdk 34、arm64-v8a 真机（内存扫描要 root 环境去读）。
- 安装后打开 App，保持前台，用扫描器 attach 本进程（包名 `com.kpm.testtarget`，
  顶部也显示 PID）。

## 靶子清单与对应功能

| 区块 | 靶子 | 测什么 | 怎么验证 |
|---|---|---|---|
| ① | DWORD/FLOAT/DOUBLE/WORD/BYTE/QWORD 六种类型（堆） | 各类型**精确搜索** | 按类型搜对应初值（1000 / 3.14 / 2.718281828 / 12345 / 42 / 9000000000），命中地址应等于卡片上显示的地址 |
| ① | DWORD(模块.bss)=111111、DWORD(匿名mmap)=222222 | **区域过滤** | 分别在「模块区 / 匿名区」过滤下搜，验证落区正确 |
| ② | FUZZY 靶（堆 DWORD，初值 500） | **模糊搜索 / 快照 Diff / 链式再筛** | 未知初值首扫或对该页打快照 → 点「变大/变小/随机」改值 → 用 增大/减小/变了/没变 逐步收窄，或用快照 `snap_refine` 链式逼近 |
| ③ | AOB 特征码 `DE AD BE EF 11 22 33 44` | **AOB 搜索** | 在 AOB 搜索里输入该特征码，命中地址应等于卡片显示地址 |
| ④ | 字符串 `MEMSCAN_TEST_STRING_7788` | **字符串搜索** | 字符串搜索该内容，命中地址应等于卡片显示地址 |
| ⑤ | 多级指针链：静态根(.bss) → NodeA `+0x18` → NodeB `+0x20` → finalValue(777777) | **指针扫描 / deref** | 以「最终地址」为目标做指针扫描，应得到静态链 `[libtesttarget.so 静态根] → 0x18 → 0x20`；也可从静态根用 deref 按该偏移链解回最终地址。点「最终值 ±1」可让值变化再验证 |
| ⑥ | 写线程靶：后台线程每 200ms 走 `level_a → level_b → do_write` 写目标地址 | **硬件观察点 + 栈回溯** | 开启写线程 → 在扫描器里对该地址下【写】观察点 → 命中后栈回溯，应看到 `do_write / level_b / level_a` 三层（`libtesttarget.so + 偏移`）。这是验证「命中即冻结栈 + 栈扫描兜底」的关键靶 |

## 说明

- 每张卡片的地址会随进程重启变化（ASLR），但**同一次运行内稳定**；重开 App 后重新对照即可。
- 指针链的「静态根」在模块 `.bss` 里，跨重启偏移恒定，正是验证指针扫描「静态链」判定的标准靶。
- 写线程的三层调用都用了 `__attribute__((noinline))`，保证栈回溯能看到清晰、可辨认的调用链。
- **指针标记(v1.1 起已处理)**：ARM64 上 `malloc` 返回的指针最高字节是分配器标记
  （如 `0xb4....`），真实虚拟地址是低 56 位。本 App 显示的地址已去标记，
  与扫描器读 `/proc/pid/maps` 的规范地址一致，可直接对照。
- **native 库已强制解压(v1.1 起)**：`extractNativeLibs=true`，`libtesttarget.so`
  以真实文件出现在 maps 里。否则默认不解压时 so 会从 `base.apk` 内存映射，
  扫描器按 so 名找不到模块、指针链静态根判定失败。若要测试「so 映射在 base.apk
  内」这种主流场景，可临时把该项设回 false 复现。
- 仅供在自有设备上测试自己的扫描器使用。
