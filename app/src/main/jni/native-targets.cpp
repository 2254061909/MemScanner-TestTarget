// native-targets.cpp —— 内存扫描器的「靶场」。
// 所有可扫描的靶子都放在 native 内存里：地址稳定、能显示、落在真实的
// 堆/匿名/模块(.bss) 区，正好覆盖 KPM-MemScanner-GG 的各项功能：
//   · 各类型精确/模糊搜索(DWORD/FLOAT/DOUBLE/WORD/BYTE/QWORD)
//   · 快照 Diff / 链式再筛(fuzzy 靶 + 一键随机/增减)
//   · AOB 特征码搜索、字符串搜索
//   · 多级指针链(静态指针 → 堆节点 → 堆节点 → 值)——指针扫描 / deref
//   · 硬件观察点 + 栈回溯(写线程走 level_a→level_b→do_write 三层调用)
//   · 区域过滤(值分别落在 堆 / 匿名 mmap / 模块.bss)
#include <jni.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <atomic>

// ───────────────────────── 静态/.bss 靶（落在 libtesttarget.so 模块区）──────────
static int32_t  s_static_int = 111111;      // id 6：模块区 DWORD

// ───────────────────────── 堆结构靶 ──────────────────────────────────────────
struct HeapTargets {
    int32_t i32;    // 1000
    float   f32;    // 3.14
    double  f64;    // 2.718281828
    int16_t i16;    // 12345
    int8_t  i8;     // 42
    int64_t i64;    // 9000000000
    int32_t fuzzy;  // 500 —— 专供模糊/快照测试
};
static HeapTargets* g_heap = nullptr;

// ───────────────────────── 匿名 mmap 靶 ──────────────────────────────────────
static int32_t* g_anon = nullptr;           // id 7：匿名区 DWORD = 222222

// ───────────────────────── 多级指针链 ────────────────────────────────────────
// g_root_holder(静态指针,模块.bss) → NodeA(堆) ; NodeA+0x18 → NodeB(堆) ; NodeB+0x20 = finalValue
struct NodeB { uint8_t pad[0x20]; int32_t finalValue; int32_t _tail; };
struct NodeA { uint8_t pad[0x18]; NodeB* toB;         int32_t _tail; };
static NodeA*  g_nodeA       = nullptr;
static NodeB*  g_nodeB       = nullptr;
static NodeA*  g_root_holder = nullptr;     // 位于 .bss 的指针变量，指针扫描的静态根
static NodeA** g_chain_root  = nullptr;

// ───────────────────────── AOB / 字符串靶 ────────────────────────────────────
static uint8_t*   g_aob    = nullptr;       // 堆缓冲，内含已知特征码
static const uint8_t AOB_BYTES[] = { 0xDE,0xAD,0xBE,0xEF,0x11,0x22,0x33,0x44 };
static char*      g_string = nullptr;       // 堆字符串
static const char TEST_STRING[] = "MEMSCAN_TEST_STRING_7788";

// ───────────────────────── 写线程靶（观察点 + 栈回溯）────────────────────────
static int32_t*  g_writer_target = nullptr; // 观察这个地址；命中后回溯应看到 do_write→level_b→level_a
static std::atomic<bool> g_writer_run{false};
static std::atomic<int>  g_writer_val{0};
static pthread_t g_writer_thread;

// noinline + 三层调用：给栈回溯一个稳定、可辨认的调用链。
static void __attribute__((noinline)) do_write(int32_t v) { if (g_writer_target) *g_writer_target = v; }
static void __attribute__((noinline)) level_b(int32_t v)  { do_write(v); }
static void __attribute__((noinline)) level_a(int32_t v)  { level_b(v + 1); }
static void* writer_loop(void* arg) {
    long ms = (long)(intptr_t)arg;
    while (g_writer_run.load()) {
        level_a(g_writer_val.fetch_add(1));
        usleep((useconds_t)(ms * 1000));
    }
    return nullptr;
}

// ───────────────────────── 数值靶描述表 ─────────────────────────────────────
// ARM64 指针标记(top-byte tagging)：malloc 返回的指针最高字节是分配器标记，
// 真正的虚拟地址是低 56 位。扫描器读 /proc/pid/maps 用的是去标记地址，
// 所以这里对外返回的所有地址都先去掉标记，界面显示才能和扫描器对上。
static inline jlong untag(const void* p) {
    return (jlong)((uintptr_t)p & 0x00FFFFFFFFFFFFFFULL);
}

struct Desc { void* ptr; char type; };   // type: i32 f d s(16) b(8) q(64)
static Desc g_desc[16];
static int  g_desc_n = 0;
static void reg(int id, void* p, char t) { g_desc[id] = { p, t }; if (id + 1 > g_desc_n) g_desc_n = id + 1; }

static bool g_inited = false;

extern "C" {
#define JNI(ret, name) JNIEXPORT ret JNICALL Java_com_kpm_testtarget_Native_##name

JNI(void, init)(JNIEnv*, jclass) {
    if (g_inited) return;
    // 堆靶
    g_heap = (HeapTargets*)malloc(sizeof(HeapTargets));
    g_heap->i32 = 1000; g_heap->f32 = 3.14f; g_heap->f64 = 2.718281828;
    g_heap->i16 = 12345; g_heap->i8 = 42; g_heap->i64 = 9000000000LL; g_heap->fuzzy = 500;
    // 匿名 mmap 靶
    g_anon = (int32_t*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_anon != MAP_FAILED) *g_anon = 222222; else g_anon = nullptr;
    // 指针链
    g_nodeA = (NodeA*)calloc(1, sizeof(NodeA));
    g_nodeB = (NodeB*)calloc(1, sizeof(NodeB));
    g_nodeB->finalValue = 777777;
    g_nodeA->toB = g_nodeB;
    g_root_holder = g_nodeA;          // 静态根(模块.bss) → NodeA
    g_chain_root = &g_root_holder;
    // AOB / 字符串
    g_aob = (uint8_t*)malloc(64);
    memset(g_aob, 0x00, 64);
    memcpy(g_aob + 16, AOB_BYTES, sizeof(AOB_BYTES));  // 特征码放偏移 16 处
    g_string = (char*)malloc(sizeof(TEST_STRING));
    memcpy(g_string, TEST_STRING, sizeof(TEST_STRING));
    // 写线程靶
    g_writer_target = (int32_t*)malloc(sizeof(int32_t));
    *g_writer_target = 0;

    // 数值靶登记：id 0..8
    reg(0, &g_heap->i32,  'i');
    reg(1, &g_heap->f32,  'f');
    reg(2, &g_heap->f64,  'd');
    reg(3, &g_heap->i16,  's');
    reg(4, &g_heap->i8,   'b');
    reg(5, &g_heap->i64,  'q');
    reg(6, &s_static_int, 'i');
    reg(7, g_anon,        'i');
    reg(8, &g_heap->fuzzy,'i');
    g_inited = true;
}

JNI(jlong, addrOf)(JNIEnv*, jclass, jint id) {
    if (id < 0 || id >= g_desc_n) return 0;
    return untag(g_desc[id].ptr);
}

JNI(jstring, valueStr)(JNIEnv* e, jclass, jint id) {
    char b[64] = "?";
    if (id >= 0 && id < g_desc_n) {
        void* p = g_desc[id].ptr;
        switch (g_desc[id].type) {
            case 'i': snprintf(b, sizeof b, "%d", *(int32_t*)p); break;
            case 'f': snprintf(b, sizeof b, "%g", *(float*)p); break;
            case 'd': snprintf(b, sizeof b, "%g", *(double*)p); break;
            case 's': snprintf(b, sizeof b, "%d", *(int16_t*)p); break;
            case 'b': snprintf(b, sizeof b, "%d", *(int8_t*)p); break;
            case 'q': snprintf(b, sizeof b, "%lld", (long long)*(int64_t*)p); break;
        }
    }
    return e->NewStringUTF(b);
}

// 给数值靶加 delta（浮点按 delta 的整数值加）。
JNI(void, bump)(JNIEnv*, jclass, jint id, jint delta) {
    if (id < 0 || id >= g_desc_n) return;
    void* p = g_desc[id].ptr;
    switch (g_desc[id].type) {
        case 'i': *(int32_t*)p += delta; break;
        case 'f': *(float*)p  += (float)delta; break;
        case 'd': *(double*)p += (double)delta; break;
        case 's': *(int16_t*)p = (int16_t)(*(int16_t*)p + delta); break;
        case 'b': *(int8_t*)p  = (int8_t)(*(int8_t*)p + delta); break;
        case 'q': *(int64_t*)p += delta; break;
    }
}

JNI(void, setInt)(JNIEnv*, jclass, jint id, jlong v) {
    if (id < 0 || id >= g_desc_n) return;
    void* p = g_desc[id].ptr;
    switch (g_desc[id].type) {
        case 'i': *(int32_t*)p = (int32_t)v; break;
        case 'f': *(float*)p  = (float)v; break;
        case 'd': *(double*)p = (double)v; break;
        case 's': *(int16_t*)p = (int16_t)v; break;
        case 'b': *(int8_t*)p  = (int8_t)v; break;
        case 'q': *(int64_t*)p = (int64_t)v; break;
    }
}

// AOB
JNI(jlong,   aobAddr)(JNIEnv*, jclass)   { return g_aob ? untag(g_aob + 16) : 0; }
JNI(jstring, aobPattern)(JNIEnv* e, jclass) { return e->NewStringUTF("DE AD BE EF 11 22 33 44"); }
// 字符串
JNI(jlong,   stringAddr)(JNIEnv*, jclass)  { return untag(g_string); }
JNI(jstring, stringValue)(JNIEnv* e, jclass) { return e->NewStringUTF(TEST_STRING); }

// 指针链信息：多行文本，含各级地址、偏移与最终值。供对照指针扫描结果。
JNI(jstring, chainInfo)(JNIEnv* e, jclass) {
    char b[512];
    snprintf(b, sizeof b,
        "静态根&holder = 0x%llx (模块.bss)\n"
        " -> NodeA = 0x%llx , +0x18 处存 NodeB*\n"
        " -> NodeB = 0x%llx , +0x20 处存 finalValue\n"
        "最终地址 = 0x%llx  值 = %d\n"
        "期望链: [libtesttarget.so 静态根] -> 0x18 -> 0x20",
        (unsigned long long)untag(g_chain_root),
        (unsigned long long)untag(g_nodeA),
        (unsigned long long)untag(g_nodeB),
        (unsigned long long)(g_nodeB ? untag(&g_nodeB->finalValue) : 0),
        g_nodeB ? g_nodeB->finalValue : 0);
    return e->NewStringUTF(b);
}
JNI(jlong, pointerFinalAddr)(JNIEnv*, jclass) { return g_nodeB ? untag(&g_nodeB->finalValue) : 0; }
JNI(void,  bumpChainFinal)(JNIEnv*, jclass, jint delta) { if (g_nodeB) g_nodeB->finalValue += delta; }
JNI(jint,  chainFinalValue)(JNIEnv*, jclass) { return g_nodeB ? g_nodeB->finalValue : 0; }

// 写线程（观察点 + 栈回溯靶）
JNI(jlong, writerTargetAddr)(JNIEnv*, jclass) { return untag(g_writer_target); }
JNI(void,  startWriter)(JNIEnv*, jclass, jint periodMs) {
    if (g_writer_run.load()) return;
    g_writer_run.store(true);
    pthread_create(&g_writer_thread, nullptr, writer_loop, (void*)(intptr_t)(periodMs <= 0 ? 200 : periodMs));
}
JNI(void, stopWriter)(JNIEnv*, jclass) {
    if (!g_writer_run.load()) return;
    g_writer_run.store(false);
    pthread_join(g_writer_thread, nullptr);
}
JNI(jint, currentPid)(JNIEnv*, jclass) { return (jint)getpid(); }

} // extern "C"
