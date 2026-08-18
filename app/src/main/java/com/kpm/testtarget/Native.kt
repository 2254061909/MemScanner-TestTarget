package com.kpm.testtarget

/** 与 native-targets.cpp 一一对应的 JNI 声明。所有靶子都在 native 内存里。 */
object Native {
    init { System.loadLibrary("testtarget") }

    // 数值靶 id（与 C 侧登记顺序一致）
    const val ID_DWORD_HEAP   = 0
    const val ID_FLOAT_HEAP   = 1
    const val ID_DOUBLE_HEAP  = 2
    const val ID_WORD_HEAP    = 3
    const val ID_BYTE_HEAP    = 4
    const val ID_QWORD_HEAP   = 5
    const val ID_DWORD_STATIC = 6
    const val ID_DWORD_ANON   = 7
    const val ID_FUZZY_HEAP   = 8

    external fun init()
    external fun addrOf(id: Int): Long
    external fun valueStr(id: Int): String
    external fun bump(id: Int, delta: Int)
    external fun setInt(id: Int, v: Long)

    external fun aobAddr(): Long
    external fun aobPattern(): String
    external fun stringAddr(): Long
    external fun stringValue(): String

    external fun chainInfo(): String
    external fun pointerFinalAddr(): Long
    external fun bumpChainFinal(delta: Int)
    external fun chainFinalValue(): Int

    external fun writerTargetAddr(): Long
    external fun startWriter(periodMs: Int)
    external fun stopWriter()
    external fun currentPid(): Int
}
