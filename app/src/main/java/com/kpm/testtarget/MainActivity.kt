package com.kpm.testtarget

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.*
import kotlin.random.Random

// 内存扫描器测试靶场。所有靶子在 native 内存里，界面直接显示每个靶子的地址+当前值，
// 方便在 KPM-MemScanner-GG 里搜到后一一对照。
class MainActivity : Activity() {

    private val h = Handler(Looper.getMainLooper())
    private lateinit var root: LinearLayout
    private val valueViews = mutableListOf<Pair<Int, TextView>>()   // (id, 值TextView)
    private var writerOn = false
    private lateinit var writerBtn: Button
    private lateinit var pidView: TextView

    private data class T(val id: Int, val name: String, val type: String)
    private val numeric = listOf(
        T(Native.ID_DWORD_HEAP,   "DWORD 整数(堆)",     "DWORD"),
        T(Native.ID_FLOAT_HEAP,   "FLOAT 浮点(堆)",     "FLOAT"),
        T(Native.ID_DOUBLE_HEAP,  "DOUBLE 双精度(堆)",  "DOUBLE"),
        T(Native.ID_WORD_HEAP,    "WORD 短整(堆)",      "WORD"),
        T(Native.ID_BYTE_HEAP,    "BYTE 字节(堆)",      "BYTE"),
        T(Native.ID_QWORD_HEAP,   "QWORD 长整(堆)",     "QWORD"),
        T(Native.ID_DWORD_STATIC, "DWORD 整数(模块.bss)", "DWORD"),
        T(Native.ID_DWORD_ANON,   "DWORD 整数(匿名mmap)", "DWORD"),
    )

    override fun onCreate(s: Bundle?) {
        super.onCreate(s)
        Native.init()

        val scroll = ScrollView(this)
        root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(16), dp(16), dp(16))
        }
        scroll.addView(root)
        setContentView(scroll)

        title = "MemScanner 测试靶场"
        pidView = label("", 16f, true)
        root.addView(pidView)
        root.addView(hint("每个靶子都显示【地址 + 当前值】。在扫描器里搜到后对照地址即可验证。"))
        root.addView(space())

        // 各类型精确/模糊搜索靶
        section("① 各类型数值靶（精确搜索 / 类型验证）")
        numeric.forEach { addNumericCard(it) }

        // 模糊 / 快照靶
        section("② 模糊搜索 / 快照 Diff / 链式再筛 靶")
        addFuzzyCard()

        // AOB
        section("③ AOB 特征码搜索")
        addAobCard()

        // 字符串
        section("④ 字符串搜索")
        addStringCard()

        // 指针链
        section("⑤ 多级指针链（指针扫描 / deref）")
        addChainCard()

        // 观察点 + 栈回溯
        section("⑥ 硬件观察点 + 栈回溯")
        addWriterCard()

        root.addView(space())
        root.addView(button("🔄 刷新全部地址与数值") { refreshAll() })

        refreshAll()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (writerOn) Native.stopWriter()
    }

    // ── 卡片 ─────────────────────────────────────────────
    private fun addNumericCard(t: T) {
        val card = card()
        card.addView(label(t.name + "  [" + t.type + "]", 15f, true))
        val addr = label("地址: 0x" + java.lang.Long.toHexString(Native.addrOf(t.id)), 13f, false)
        val v = label("", 14f, false)
        valueViews.add(t.id to v)
        card.addView(addr); card.addView(v)
        card.addView(row(
            button("−1") { Native.bump(t.id, -1); refreshAll() },
            button("+1") { Native.bump(t.id, 1); refreshAll() },
            button("+100") { Native.bump(t.id, 100); refreshAll() },
        ))
        root.addView(card)
    }

    private fun addFuzzyCard() {
        val id = Native.ID_FUZZY_HEAP
        val card = card()
        card.addView(label("FUZZY 靶(堆 DWORD, 初值500)", 15f, true))
        card.addView(hint("玩法: 扫描器先「未知初值」首扫或对本靶所在页打快照; 点下面按钮改值后, 用 变大/变小/变了/没变 逐步收窄, 也可用快照『再筛』链式逼近。"))
        card.addView(label("地址: 0x" + java.lang.Long.toHexString(Native.addrOf(id)), 13f, false))
        val v = label("", 14f, false)
        valueViews.add(id to v); card.addView(v)
        card.addView(row(
            button("变大 +7") { Native.bump(id, 7); refreshAll() },
            button("变小 −5") { Native.bump(id, -5); refreshAll() },
            button("随机跳变") { Native.setInt(id, Random.nextInt(1, 9999).toLong()); refreshAll() },
        ))
        root.addView(card)
    }

    private fun addAobCard() {
        val card = card()
        card.addView(label("AOB 特征码", 15f, true))
        card.addView(label("特征码: " + Native.aobPattern(), 14f, false))
        card.addView(label("地址: 0x" + java.lang.Long.toHexString(Native.aobAddr()), 13f, false))
        card.addView(hint("在扫描器的 AOB 搜索里输入该特征码, 命中地址应等于上面这个。"))
        root.addView(card)
    }

    private fun addStringCard() {
        val card = card()
        card.addView(label("字符串靶", 15f, true))
        card.addView(label("内容: " + Native.stringValue(), 14f, false))
        card.addView(label("地址: 0x" + java.lang.Long.toHexString(Native.stringAddr()), 13f, false))
        card.addView(hint("在扫描器的字符串搜索里输入该内容, 命中地址应等于上面这个。"))
        root.addView(card)
    }

    private fun addChainCard() {
        val card = card()
        card.addView(label("多级指针链", 15f, true))
        val info = label(Native.chainInfo(), 13f, false)
        card.addView(info)
        card.addView(hint("在扫描器里以『最终地址』为目标做指针扫描, 应得到一条静态链: [libtesttarget.so 静态根] → 0x18 → 0x20。也可用 deref 从静态根按该偏移链解回最终地址。"))
        card.addView(row(
            button("最终值 +1") { Native.bumpChainFinal(1); info.text = Native.chainInfo() },
            button("最终值 −1") { Native.bumpChainFinal(-1); info.text = Native.chainInfo() },
            button("刷新链信息") { info.text = Native.chainInfo() },
        ))
        root.addView(card)
    }

    private fun addWriterCard() {
        val card = card()
        card.addView(label("写线程靶(观察点 + 栈回溯)", 15f, true))
        card.addView(label("目标地址: 0x" + java.lang.Long.toHexString(Native.writerTargetAddr()), 13f, false))
        card.addView(hint("开启后, 后台线程每 200ms 走 level_a→level_b→do_write 写这个地址。\n在扫描器里对该地址下【写】硬件观察点, 命中后做栈回溯, 应看到 do_write/level_b/level_a 三层(libtesttarget.so + 偏移)。"))
        writerBtn = button("▶ 开启写线程") { toggleWriter() }
        card.addView(writerBtn)
        root.addView(card)
    }

    private fun toggleWriter() {
        if (writerOn) { Native.stopWriter(); writerOn = false; writerBtn.text = "▶ 开启写线程" }
        else { Native.startWriter(200); writerOn = true; writerBtn.text = "⏸ 停止写线程" }
    }

    private fun refreshAll() {
        pidView.text = "本进程 PID = " + Native.currentPid() + "   包名 = " + packageName
        for ((id, tv) in valueViews) tv.text = "当前值: " + Native.valueStr(id)
    }

    // ── UI 小工具 ─────────────────────────────────────────
    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
    private fun section(t: String) { root.addView(space()); root.addView(label(t, 17f, true)) }
    private fun space() = View(this).apply { layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, dp(10)) }

    private fun label(t: String, sz: Float, bold: Boolean) = TextView(this).apply {
        text = t; textSize = sz; setTextIsSelectable(true)
        if (bold) setTypeface(typeface, android.graphics.Typeface.BOLD)
        setPadding(0, dp(2), 0, dp(2))
    }
    private fun hint(t: String) = TextView(this).apply {
        text = t; textSize = 12f; setTextColor(0xFF888888.toInt()); setPadding(0, dp(2), 0, dp(4))
    }

    private fun card(): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(12), dp(10), dp(12), dp(10))
        val lp = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT)
        lp.setMargins(0, dp(6), 0, dp(6)); layoutParams = lp
        setBackgroundColor(0xFFF2F2F2.toInt())
    }
    private fun row(vararg vs: View): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL; gravity = Gravity.START
        vs.forEach { addView(it, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).also { p -> p.rightMargin = dp(8) }) }
    }
    private fun button(t: String, onClick: () -> Unit) = Button(this).apply {
        text = t; isAllCaps = false; setOnClickListener { onClick() }
    }
}
