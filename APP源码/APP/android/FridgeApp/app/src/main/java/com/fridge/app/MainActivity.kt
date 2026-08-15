package com.fridge.app

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.graphics.drawable.ShapeDrawable
import android.graphics.drawable.shapes.RoundRectShape
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.*
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import kotlin.concurrent.thread

class MainActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())
    private var pollTask: Runnable? = null
    private lateinit var prefs: android.content.SharedPreferences

    private var baseUrl = "http://101.245.87.7:10087"
    private var token = "fridge-app-token"

    // ── 色彩常量 ──
    private val clrBg = Color.parseColor("#F0F9FF")
    private val clrSurface = Color.parseColor("#FFFFFF")
    private val clrOnSurface = Color.parseColor("#0F172A")
    private val clrOnSurfaceVar = Color.parseColor("#475569")
    private val clrPrimary = Color.parseColor("#0EA5E9")
    private val clrPrimaryLight = Color.parseColor("#E0F2FE")
    private val clrSuccess = Color.parseColor("#10B981")
    private val clrError = Color.parseColor("#EF4444")
    private val clrWarning = Color.parseColor("#F59E0B")

    private val modeColors = mapOf(
        "fridge"  to Triple(Color.parseColor("#3B82F6"), Color.parseColor("#EFF6FF"), Color.parseColor("#1D4ED8")),
        "detect"  to Triple(Color.parseColor("#8B5CF6"), Color.parseColor("#F5F3FF"), Color.parseColor("#6D28D9")),
        "storage" to Triple(Color.parseColor("#F59E0B"), Color.parseColor("#FFFBEB"), Color.parseColor("#B45309")),
        "health"  to Triple(Color.parseColor("#EF4444"), Color.parseColor("#FEF2F2"), Color.parseColor("#B91C1C"))
    )

    // ── 基础工具 ──
    private fun dp(v: Int): Int = (v * resources.displayMetrics.density + 0.5f).toInt()
    private fun sp(v: Int): Float = v * resources.displayMetrics.scaledDensity

    private fun roundedBg(radius: Int, fillColor: Int, strokeColor: Int = 0, strokeWidth: Int = 0): GradientDrawable {
        return GradientDrawable().apply {
            setColor(fillColor)
            setCornerRadius(dp(radius).toFloat())
            if (strokeColor != 0) setStroke(dp(strokeWidth), strokeColor)
        }
    }

    private fun rippleBg(content: GradientDrawable): RippleDrawable {
        return RippleDrawable(
            android.content.res.ColorStateList.valueOf(Color.parseColor("#1A000000")),
            content, null
        )
    }

    // ── 带图标的按钮 ──
    private fun iconButton(
        text: String, iconRes: Int, color: Int, onClick: () -> Unit,
        fill: Boolean = true, height: Int = 52
    ): Button {
        return Button(this).apply {
            setText(text)
            isAllCaps = false
            textSize = sp(11)
            setTextColor(if (fill) Color.WHITE else color)
            gravity = Gravity.CENTER
            setPadding(dp(16), 0, dp(16), 0)
            setCompoundDrawablesWithIntrinsicBounds(iconRes, 0, 0, 0)
            compoundDrawablePadding = dp(8)
            background = if (fill) roundedBg(12, color) else roundedBg(12, Color.TRANSPARENT, color, 2)
            typeface = Typeface.DEFAULT_BOLD
            minHeight = dp(height)
            stateListAnimator = null
            setOnClickListener { onClick() }
        }
    }

    // ── 卡片容器 ──
    private fun card(bgColor: Int = clrSurface): LinearLayout {
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(16), dp(16), dp(16))
            background = roundedBg(16, bgColor, Color.parseColor("#E2E8F0"), 1)
            val lp = LinearLayout.LayoutParams(-1, -2)
            lp.setMargins(0, 0, 0, dp(12))
            layoutParams = lp
        }
    }

    // ── 状态标签 ──
    private fun chip(text: String, color: Int, iconRes: Int = 0): LinearLayout {
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(8), dp(4), dp(8), dp(4))
            background = roundedBg(20, adjustAlpha(color, 0.15f))
            if (iconRes != 0) {
                val iv = ImageView(context).apply {
                    setImageResource(iconRes)
                    val s = dp(14)
                    layoutParams = LinearLayout.LayoutParams(s, s).apply { setMargins(0, 0, dp(4), 0) }
                }
                addView(iv)
            }
            addView(TextView(context).apply {
                setText(text)
                textSize = sp(10)
                setTextColor(color)
                typeface = Typeface.DEFAULT_BOLD
            })
        }
    }

    private fun adjustAlpha(color: Int, factor: Float): Int {
        val a = (Color.alpha(color) * factor).toInt().coerceIn(0, 255)
        return Color.argb(a, Color.red(color), Color.green(color), Color.blue(color))
    }

    // ── 带图标的标题 ──
    private fun sectionTitle(text: String, iconRes: Int = 0, color: Int = clrOnSurface): LinearLayout {
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(16), 0, dp(8))
            if (iconRes != 0) {
                val iv = ImageView(context).apply {
                    setImageResource(iconRes)
                    val s = dp(22)
                    layoutParams = LinearLayout.LayoutParams(s, s).apply { setMargins(0, 0, dp(8), 0) }
                }
                addView(iv)
            }
            addView(TextView(context).apply {
                setText(text)
                textSize = sp(11)
                setTextColor(color)
                typeface = Typeface.DEFAULT_BOLD
            })
        }
    }

    // ── 空行间距 ──
    private fun spacer(h: Int = 12): View {
        return View(this).apply { layoutParams = LinearLayout.LayoutParams(-1, dp(h)) }
    }

    // ── 分行（权重均分）──
    private fun hRow(vararg views: View): LinearLayout {
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(4), 0, dp(4))
            views.forEachIndexed { i, v ->
                val lp = LinearLayout.LayoutParams(0, -2, 1f)
                if (i > 0) lp.setMargins(dp(8), 0, 0, 0)
                addView(v, lp)
            }
        }
    }

    // ── 分行（自适应宽度）──
    private fun hRowWrap(vararg views: View): LinearLayout {
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(4), 0, dp(4))
            views.forEachIndexed { i, v ->
                val lp = LinearLayout.LayoutParams(-2, -2)
                if (i > 0) lp.setMargins(dp(8), 0, 0, 0)
                addView(v, lp)
            }
        }
    }

    // ═══════════════════════════════════════════
    //  生命周期 & 网络层（不变）
    // ═══════════════════════════════════════════

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = getSharedPreferences("fridge_app", Context.MODE_PRIVATE)
        baseUrl = prefs.getString("base_url", baseUrl) ?: baseUrl
        token = prefs.getString("token", token) ?: token
        showHome()
    }

    override fun onPause() {
        super.onPause()
        stopPolling()
    }

    private fun stopPolling() {
        pollTask?.let { handler.removeCallbacks(it) }
        pollTask = null
    }

    private fun startPolling(intervalMs: Long, task: () -> Unit) {
        stopPolling()
        val runnable = object : Runnable {
            override fun run() {
                task()
                handler.postDelayed(this, intervalMs)
            }
        }
        pollTask = runnable
        runnable.run()
    }

    private fun showToast(message: String) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
    }

    private fun apiRequest(
        method: String,
        path: String,
        body: JSONObject? = null,
        onSuccess: (JSONObject) -> Unit,
        onError: (String) -> Unit
    ) {
        val urlText = baseUrl.trimEnd('/') + path
        thread {
            try {
                val conn = (URL(urlText).openConnection() as HttpURLConnection).apply {
                    requestMethod = method
                    connectTimeout = 6000
                    readTimeout = 20000
                    setRequestProperty("X-Fridge-Token", token)
                    setRequestProperty("Accept", "application/json")
                    if (body != null) {
                        doOutput = true
                        setRequestProperty("Content-Type", "application/json; charset=utf-8")
                    }
                }
                if (body != null) {
                    conn.outputStream.use { it.write(body.toString().toByteArray(Charsets.UTF_8)) }
                }
                val code = conn.responseCode
                val stream = if (code in 200..299) conn.inputStream else conn.errorStream
                val text = stream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() }.orEmpty()
                val json = if (text.isBlank()) JSONObject() else JSONObject(text)
                handler.post {
                    if (code in 200..299) {
                        onSuccess(json)
                    } else {
                        onError(json.optString("error", "HTTP $code"))
                    }
                }
                conn.disconnect()
            } catch (exc: Exception) {
                handler.post { onError(exc.message ?: "网络请求失败") }
            }
        }
    }

    // ═══════════════════════════════════════════
    //  根布局
    // ═══════════════════════════════════════════

    private fun root(title: String, showBack: Boolean = true): Pair<LinearLayout, LinearLayout> {
        stopPolling()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(12), dp(16), dp(16))
            setBackgroundColor(clrBg)
        }

        // 顶部栏
        val topBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(4), 0, dp(8))
        }

        if (showBack) {
            val backBtn = ImageView(this).apply {
                setImageResource(R.drawable.ic_back)
                setPadding(dp(10), dp(10), dp(10), dp(10))
                background = roundedBg(12, clrSurface, Color.parseColor("#E2E8F0"), 1)
                setOnClickListener { showHome() }
                val s = dp(44)
                topBar.addView(this, LinearLayout.LayoutParams(s, s).apply { setMargins(0, 0, dp(10), 0) })
            }
        }

        topBar.addView(TextView(this).apply {
            text = title
            textSize = sp(11)
            setTextColor(clrOnSurface)
            typeface = Typeface.DEFAULT_BOLD
            gravity = Gravity.CENTER_VERTICAL
        }, LinearLayout.LayoutParams(0, dp(48), 1f))

        val settingsBtn = ImageView(this).apply {
            setImageResource(R.drawable.ic_settings)
            setPadding(dp(10), dp(10), dp(10), dp(10))
            background = roundedBg(12, clrSurface, Color.parseColor("#E2E8F0"), 1)
            setOnClickListener { showSettingsDialog() }
            val s = dp(44)
            topBar.addView(this, LinearLayout.LayoutParams(s, s))
        }

        root.addView(topBar)

        // 滚动内容区
        val scroll = ScrollView(this)
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(8), 0, dp(24))
        }
        scroll.addView(content)
        root.addView(scroll, LinearLayout.LayoutParams(-1, 0, 1f))
        setContentView(root)
        return Pair(root, content)
    }

    // ═══════════════════════════════════════════
    //  设置对话框（不变）
    // ═══════════════════════════════════════════

    private fun showSettingsDialog() {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), 0)
        }
        val serverEdit = EditText(this).apply {
            hint = "服务器地址"
            setText(baseUrl)
            textSize = sp(11)
            setPadding(dp(12), dp(10), dp(12), dp(10))
            setSingleLine(true)
            background = roundedBg(10, Color.parseColor("#F8FAFC"), Color.parseColor("#CBD5E1"), 1)
        }
        val tokenEdit = EditText(this).apply {
            hint = "Token"
            setText(token)
            textSize = sp(11)
            setPadding(dp(12), dp(10), dp(12), dp(10))
            setSingleLine(true)
            background = roundedBg(10, Color.parseColor("#F8FAFC"), Color.parseColor("#CBD5E1"), 1)
        }
        box.addView(serverEdit)
        box.addView(spacer(12))
        box.addView(tokenEdit)

        AlertDialog.Builder(this)
            .setTitle("服务器设置")
            .setView(box)
            .setNegativeButton("取消", null)
            .setPositiveButton("保存") { _, _ ->
                baseUrl = serverEdit.text.toString().trim().ifBlank { baseUrl }
                token = tokenEdit.text.toString().trim().ifBlank { token }
                prefs.edit().putString("base_url", baseUrl).putString("token", token).apply()
                showToast("已保存")
            }
            .show()
    }

    // ═══════════════════════════════════════════
    //  主页 — 彩色卡片网格
    // ═══════════════════════════════════════════

    private val modes = listOf(
        ModeCard("冰箱模式", "温控 · 风力 · 门锁", R.drawable.ic_fridge, "fridge", ::showFridgeMode),
        ModeCard("检测模式", "视觉识别 · 药盒检测", R.drawable.ic_detect, "detect", ::showDetectionMode),
        ModeCard("仓储模式", "药品管理 · 库存查询", R.drawable.ic_storage, "storage", ::showStorageMode),
        ModeCard("健康助手", "AI 用药推荐", R.drawable.ic_health, "health", ::showHealthMode)
    )

    data class ModeCard(
        val title: String, val desc: String, val icon: Int,
        val key: String, val action: () -> Unit
    )

    private fun showHome() {
        val (rootLayout, content) = root("智能药箱冰箱", showBack = false)

        // 服务器信息条
        content.addView(chip("  ${baseUrl}", clrOnSurfaceVar, R.drawable.ic_slot))
        content.addView(spacer(16))

        // 四个模式卡片
        modes.forEach { mode ->
            val (fg, bg, bgDark) = modeColors[mode.key]!!

            // 外层容器：左边彩色条 + 右边内容
            val outer = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                background = roundedBg(16, bg, fg, 1)
                setPadding(0, 0, 0, 0)
                val lp = LinearLayout.LayoutParams(-1, -2)
                lp.setMargins(0, 0, 0, dp(14))
                layoutParams = lp
                isClickable = true
                isFocusable = true
                setOnClickListener { mode.action() }
            }

            // 左侧彩色条
            val bar = View(this).apply {
                setBackgroundColor(fg)
                outer.addView(this, LinearLayout.LayoutParams(dp(6), -1))
            }

            // 右侧内容区
            val inner = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(16), dp(18), dp(16), dp(18))
            }
            outer.addView(inner, LinearLayout.LayoutParams(0, -2, 1f))

            // 图标行
            val iconRow = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
            }
            val iconBg = View(this).apply {
                background = roundedBg(14, adjustAlpha(fg, 0.18f))
                val s = dp(48)
                layoutParams = LinearLayout.LayoutParams(s, s)
            }
            val iconBgInner = FrameLayout(this).apply {
                val s = dp(48)
                layoutParams = LinearLayout.LayoutParams(s, s)
            }
            iconBgInner.addView(ImageView(this).apply {
                setImageResource(mode.icon)
                val s = dp(28)
                layoutParams = FrameLayout.LayoutParams(s, s, Gravity.CENTER)
            })
            iconRow.addView(iconBgInner)
            iconRow.addView(TextView(this).apply {
                text = mode.title
                textSize = sp(11)
                setTextColor(bgDark)
                typeface = Typeface.DEFAULT_BOLD
                setPadding(dp(14), 0, 0, 0)
            })
            inner.addView(iconRow)

            // 描述文字
            inner.addView(TextView(this).apply {
                text = mode.desc
                textSize = sp(11)
                setTextColor(clrOnSurfaceVar)
                setPadding(dp(62), dp(6), 0, 0)
            })

            content.addView(outer)
        }
    }

    // ═══════════════════════════════════════════
    //  冰箱模式
    // ═══════════════════════════════════════════

    private fun showFridgeMode() {
        val (_, content) = root("冰箱模式")

        // 状态卡片
        val statusCard = card(Color.parseColor("#EFF6FF"))
        val statusTitle = TextView(this).apply {
            text = "正在读取..."
            textSize = sp(11)
            setTextColor(clrOnSurfaceVar)
        }
        statusCard.addView(statusTitle)

        // 当前温度（大字）
        val currentTempLabel = TextView(this).apply {
            text = "当前温度"
            textSize = sp(11)
            setTextColor(clrOnSurfaceVar)
            setPadding(0, dp(6), 0, 0)
        }
        statusCard.addView(currentTempLabel)
        val currentTempText = TextView(this).apply {
            text = "--°C"
            textSize = sp(24)
            setTextColor(clrOnSurface)
            typeface = Typeface.DEFAULT_BOLD
        }
        statusCard.addView(currentTempText)

        // 目标温度（单独一行）
        val setTempRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(8), 0, 0)
        }
        val setTempIcon = ImageView(this).apply {
            setImageResource(R.drawable.ic_temp)
            val s = dp(18)
            layoutParams = LinearLayout.LayoutParams(s, s).apply { setMargins(0, 0, dp(6), 0) }
        }
        setTempRow.addView(setTempIcon)
        setTempRow.addView(TextView(this).apply {
            text = "目标温度"
            textSize = sp(12)
            setTextColor(clrOnSurfaceVar)
        })
        val setTempText = TextView(this).apply {
            text = "--°C"
            textSize = sp(18)
            setTextColor(Color.parseColor("#3B82F6"))
            typeface = Typeface.DEFAULT_BOLD
            setPadding(dp(10), 0, 0, 0)
        }
        setTempRow.addView(setTempText)
        statusCard.addView(setTempRow)

        // 风力/门/制冷 状态行
        val detailRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, dp(10), 0, 0)
        }
        val fanChip = chip("风力: --", clrOnSurfaceVar)
        val doorChip = chip("门: --", clrOnSurfaceVar)
        val coolChip = chip("制冷: --", clrOnSurfaceVar)
        detailRow.addView(fanChip)
        detailRow.addView(doorChip, LinearLayout.LayoutParams(-2, -2).apply { setMargins(dp(6), 0, 0, 0) })
        detailRow.addView(coolChip, LinearLayout.LayoutParams(-2, -2).apply { setMargins(dp(6), 0, 0, 0) })
        statusCard.addView(detailRow)

        // 在线状态
        val statusRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, dp(8), 0, 0)
        }
        val onlineChip = chip("等待连接", clrOnSurfaceVar)
        statusRow.addView(onlineChip)
        statusCard.addView(statusRow)

        content.addView(statusCard)
        content.addView(spacer(8))

        // 温度控制卡片
        val tempCard = card()
        tempCard.addView(sectionTitle("温度控制", R.drawable.ic_temp, clrPrimary))
        tempCard.addView(hRow(
            iconButton("-1°C", 0, Color.parseColor("#64748B"), { adjustSetTemp(-1) }, fill = false),
            iconButton("+1°C", 0, Color.parseColor("#3B82F6"), { adjustSetTemp(1) })
        ))
        content.addView(tempCard)

        // 风力控制卡片
        val fanCard = card()
        fanCard.addView(sectionTitle("风力大小", R.drawable.ic_fan, clrPrimary))
        fanCard.addView(hRow(
            iconButton("关闭", 0, Color.parseColor("#64748B"), { sendControl(JSONObject().put("fan", 0)) }, fill = false),
            iconButton("低风", 0, Color.parseColor("#0EA5E9"), { sendControl(JSONObject().put("fan", 1)) }),
            iconButton("强风", 0, Color.parseColor("#0284C7"), { sendControl(JSONObject().put("fan", 2)) })
        ))
        content.addView(fanCard)

        // 门控制卡片
        val doorCard = card()
        doorCard.addView(sectionTitle("门控制", R.drawable.ic_door, clrPrimary))
        doorCard.addView(hRow(
            iconButton("开门", 0, clrSuccess, { sendControl(JSONObject().put("door", true)) }),
            iconButton("关门", 0, clrError, { sendControl(JSONObject().put("door", false)) }, fill = false)
        ))
        content.addView(doorCard)

        // 制冷开关
        val coolSwitch = Switch(this).apply {
            text = "制冷片"
            textSize = sp(11)
            setTextColor(clrOnSurface)
            setPadding(dp(16), dp(10), dp(16), dp(10))
        }
        val coolCard = card()
        coolCard.addView(coolSwitch)
        content.addView(coolCard)

        // 制冷开关：仅首次同步，之后由用户控制
        var coolSynced = false
        coolSwitch.setOnCheckedChangeListener { _, checked ->
            if (coolSynced) sendControl(JSONObject().put("cool", checked))
        }

        // 水冷开关
        val waterCoolSwitch = Switch(this).apply {
            text = "水冷系统"
            textSize = sp(11)
            setTextColor(clrOnSurface)
            setPadding(dp(16), dp(10), dp(16), dp(10))
        }
        val waterCoolCard = card()
        waterCoolCard.addView(waterCoolSwitch)
        content.addView(waterCoolCard)

        var waterCoolSynced = false
        waterCoolSwitch.setOnCheckedChangeListener { _, checked ->
            if (waterCoolSynced) sendControl(JSONObject().put("water_cool", checked))
        }

        fun render(json: JSONObject) {
            val status = json.getJSONObject("status")
            val online = status.optBoolean("online")
            val setTemp = status.optInt("set_temp")
            val currentTemp = status.optDouble("current_temp")
            val fan = status.optInt("fan")
            val cool = status.optBoolean("cool")
            val door = status.optBoolean("door")
            val waterCool = status.optBoolean("water_cool")

            // 在线状态
            statusTitle.text = if (online) "设备在线" else "设备离线"
            statusTitle.setTextColor(if (online) clrSuccess else clrError)
            statusRow.removeAllViews()
            val dot = if (online) R.drawable.ic_online else R.drawable.ic_offline
            statusRow.addView(chip(if (online) "在线" else "离线",
                if (online) clrSuccess else clrError, dot))

            // 温度显示
            currentTempText.text = "${currentTemp}°C"
            setTempText.text = "${setTemp}°C"

            // 风力/门/制冷/水冷 状态
            detailRow.removeAllViews()
            detailRow.addView(chip("风力: $fan", clrOnSurfaceVar))
            detailRow.addView(
                if (door) chip("门: 开", clrSuccess) else chip("门: 关", clrOnSurfaceVar),
                LinearLayout.LayoutParams(-2, -2).apply { setMargins(dp(6), 0, 0, 0) }
            )
            detailRow.addView(
                if (cool) chip("制冷: 开", clrSuccess) else chip("制冷: 关", clrOnSurfaceVar),
                LinearLayout.LayoutParams(-2, -2).apply { setMargins(dp(6), 0, 0, 0) }
            )
            detailRow.addView(
                if (waterCool) chip("水冷: 开", clrSuccess) else chip("水冷: 关", clrOnSurfaceVar),
                LinearLayout.LayoutParams(-2, -2).apply { setMargins(dp(6), 0, 0, 0) }
            )

            // 制冷开关首次同步后不再覆写
            if (!coolSynced) {
                coolSwitch.isChecked = cool
                coolSynced = true
            }

            // 水冷开关首次同步后不再覆写
            if (!waterCoolSynced) {
                waterCoolSwitch.isChecked = waterCool
                waterCoolSynced = true
            }
        }

        startPolling(2000L) {
            apiRequest("GET", "/api/fridge/status", null, ::render) {
                statusTitle.text = "连接失败"
                statusTitle.setTextColor(clrError)
            }
        }
    }

    private fun adjustSetTemp(delta: Int) {
        apiRequest("GET", "/api/fridge/status", null, { json ->
            val setTemp = json.getJSONObject("status").optInt("set_temp") + delta
            sendControl(JSONObject().put("set_temp", setTemp))
        }, { showToast(it) })
    }

    private fun sendControl(body: JSONObject) {
        apiRequest("POST", "/api/fridge/control", body, {
            showToast("已发送")
        }, {
            showToast("发送失败：$it")
        })
    }

    // ═══════════════════════════════════════════
    //  检测模式
    // ═══════════════════════════════════════════

    private fun showDetectionMode() {
        val (_, content) = root("检测模式")

        // 摄像头图标区域
        val camCard = card(Color.parseColor("#F5F3FF"))
        val camIcon = ImageView(this).apply {
            setImageResource(R.drawable.ic_detect)
            val s = dp(64)
            layoutParams = LinearLayout.LayoutParams(s, s).apply { gravity = Gravity.CENTER_HORIZONTAL }
        }
        camCard.addView(camIcon)
        val resultText = TextView(this).apply {
            text = "正在读取视觉结果..."
            textSize = sp(12)
            setTextColor(clrOnSurface)
            gravity = Gravity.CENTER
            setPadding(0, dp(12), 0, 0)
        }
        camCard.addView(resultText)
        content.addView(camCard)

        content.addView(iconButton("刷新检测", R.drawable.ic_refresh, Color.parseColor("#8B5CF6"), {
            loadVision(resultText)
        }))

        startPolling(2000L) { loadVision(resultText) }
    }

    private fun loadVision(result: TextView) {
        apiRequest("GET", "/api/vision/latest", null, { json ->
            val vision = json.getJSONObject("vision")
            val slot = if (vision.isNull("slot")) "无" else vision.optInt("slot").toString()
            val name = vision.optString("name", "未识别")
            result.text = "槽位：$slot\n药品：${name.ifBlank { "未识别" }}"
        }, {
            result.text = "读取失败：$it"
        })
    }

    // ═══════════════════════════════════════════
    //  仓储模式
    // ═══════════════════════════════════════════

    private fun showStorageMode() {
        val (_, content) = root("仓储模式")

        // 搜索栏
        val searchBox = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = roundedBg(12, clrSurface, Color.parseColor("#CBD5E1"), 1)
            setPadding(dp(4), dp(4), dp(4), dp(4))
        }
        val queryEdit = EditText(this).apply {
            hint = "输入药名或药效搜索"
            textSize = sp(11)
            setPadding(dp(12), dp(10), dp(12), dp(10))
            background = null
            setSingleLine(true)
        }
        searchBox.addView(queryEdit, LinearLayout.LayoutParams(0, -2, 1f))
        content.addView(searchBox)
        content.addView(spacer(10))

        // 药品列表容器（先声明，后续按钮引用）
        val listContainer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }

        // 操作按钮
        content.addView(hRow(
            iconButton("查询", R.drawable.ic_search, clrPrimary,
                { loadMedicines(listContainer, queryEdit.text.toString()) }),
            iconButton("全部", 0, Color.parseColor("#64748B"),
                { loadMedicines(listContainer, "") }, fill = false),
            iconButton("新建", R.drawable.ic_add, clrSuccess,
                { showMedicineDialog(null, null) { loadMedicines(listContainer, queryEdit.text.toString()) } })
        ))

        content.addView(listContainer)

        loadMedicines(listContainer, "")
    }

    private fun loadMedicines(list: LinearLayout, query: String) {
        val path = if (query.isBlank()) "/api/medicines" else "/api/medicines?q=${java.net.URLEncoder.encode(query, "UTF-8")}"
        apiRequest("GET", path, null, { json ->
            list.removeAllViews()
            val records = json.getJSONArray("records")
            if (records.length() == 0) {
                val emptyCard = card(Color.parseColor("#F8FAFC"))
                val emptyText = TextView(this).apply {
                    text = "暂无药品记录"
                    textSize = sp(11)
                    setTextColor(clrOnSurfaceVar)
                    gravity = Gravity.CENTER
                    setPadding(0, dp(24), 0, dp(24))
                }
                emptyCard.addView(emptyText)
                list.addView(emptyCard)
            }
            for (i in 0 until records.length()) {
                val record = records.getJSONObject(i)
                val slot = record.optInt("slot")
                val valid = record.optBoolean("valid")
                val name = record.optString("name")

                val medicineCard = card(if (valid) clrSurface else Color.parseColor("#F8FAFC"))

                // 槽位号 + 名称
                val headerRow = LinearLayout(this).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                }
                // 槽位编号圆圈
                val slotBadge = TextView(this).apply {
                    text = slot.toString()
                    textSize = sp(12)
                    setTextColor(Color.WHITE)
                    gravity = Gravity.CENTER
                    typeface = Typeface.DEFAULT_BOLD
                    background = roundedBg(20, if (valid) clrPrimary else Color.parseColor("#94A3B8"))
                    val s = dp(36)
                    layoutParams = LinearLayout.LayoutParams(s, s)
                }
                headerRow.addView(slotBadge)
                headerRow.addView(TextView(this).apply {
                    text = if (valid) "  $name" else "  空槽位"
                    textSize = sp(12)
                    setTextColor(if (valid) clrOnSurface else clrOnSurfaceVar)
                    typeface = Typeface.DEFAULT_BOLD
                    setPadding(dp(10), 0, 0, 0)
                })
                // 状态标签
                if (valid) {
                    headerRow.addView(chip("有效", clrSuccess), LinearLayout.LayoutParams(-2, -2).apply { setMargins(dp(8), 0, 0, 0) })
                }
                medicineCard.addView(headerRow)

                if (valid) {
                    medicineCard.addView(TextView(this).apply {
                        text = "生产：${record.optString("production_date")}    保质期：${record.optString("shelf_life")}"
                        textSize = sp(11)
                        setTextColor(clrOnSurfaceVar)
                        setPadding(dp(46), dp(8), 0, 0)
                    })
                    medicineCard.addView(TextView(this).apply {
                        text = "药效：${record.optString("efficacy")}"
                        textSize = sp(11)
                        setTextColor(clrOnSurfaceVar)
                        setPadding(dp(46), dp(2), 0, 0)
                    })
                }

                // 操作按钮行
                medicineCard.addView(hRowWrap(
                    iconButton(if (valid) "修改" else "录入", R.drawable.ic_edit, clrWarning,
                        { showMedicineDialog(slot, record) { loadMedicines(list, query) } }, fill = false, height = 40),
                    iconButton("删除", R.drawable.ic_delete, clrError,
                        { deleteMedicine(slot) { loadMedicines(list, query) } }, fill = false, height = 40)
                ))

                list.addView(medicineCard)
            }
        }, {
            list.removeAllViews()
            val errCard = card(Color.parseColor("#FEF2F2"))
            errCard.addView(TextView(this).apply {
                text = "读取失败：$it"
                textSize = sp(12)
                setTextColor(clrError)
                gravity = Gravity.CENTER
            })
            list.addView(errCard)
        })
    }

    private fun showMedicineDialog(slot: Int?, record: JSONObject?, afterSave: () -> Unit) {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), 0)
        }
        val name = EditText(this).apply {
            hint = "药品名称"
            setText(record?.optString("name").orEmpty())
            textSize = sp(11)
            setPadding(dp(12), dp(10), dp(12), dp(10))
            setSingleLine(true)
            background = roundedBg(10, Color.parseColor("#F8FAFC"), Color.parseColor("#CBD5E1"), 1)
        }
        val productionDate = EditText(this).apply {
            hint = "生产日期，例如 2026-05-01"
            setText(record?.optString("production_date").orEmpty())
            textSize = sp(11)
            setPadding(dp(12), dp(10), dp(12), dp(10))
            setSingleLine(true)
            background = roundedBg(10, Color.parseColor("#F8FAFC"), Color.parseColor("#CBD5E1"), 1)
        }
        val shelfLife = EditText(this).apply {
            hint = "保质期，例如 24个月"
            setText(record?.optString("shelf_life").orEmpty())
            textSize = sp(11)
            setPadding(dp(12), dp(10), dp(12), dp(10))
            setSingleLine(true)
            background = roundedBg(10, Color.parseColor("#F8FAFC"), Color.parseColor("#CBD5E1"), 1)
        }
        val efficacy = EditText(this).apply {
            hint = "药效"
            setText(record?.optString("efficacy").orEmpty())
            textSize = sp(11)
            setPadding(dp(12), dp(10), dp(12), dp(10))
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            minLines = 2
            background = roundedBg(10, Color.parseColor("#F8FAFC"), Color.parseColor("#CBD5E1"), 1)
        }
        box.addView(name)
        box.addView(spacer(10))
        box.addView(productionDate)
        box.addView(spacer(10))
        box.addView(shelfLife)
        box.addView(spacer(10))
        box.addView(efficacy)

        AlertDialog.Builder(this)
            .setTitle(if (slot == null) "新建药品" else "修改槽位 $slot")
            .setView(box)
            .setNegativeButton("取消", null)
            .setPositiveButton("保存") { _, _ ->
                val body = JSONObject()
                    .put("name", name.text.toString())
                    .put("production_date", productionDate.text.toString())
                    .put("shelf_life", shelfLife.text.toString())
                    .put("efficacy", efficacy.text.toString())
                val method = if (slot == null) "POST" else "PUT"
                val path = if (slot == null) "/api/medicines" else "/api/medicines/$slot"
                apiRequest(method, path, body, {
                    showToast("已保存")
                    afterSave()
                }, {
                    showToast("保存失败：$it")
                })
            }
            .show()
    }

    private fun deleteMedicine(slot: Int, afterDelete: () -> Unit) {
        AlertDialog.Builder(this)
            .setTitle("删除药品")
            .setMessage("确定删除槽位 $slot 的药品信息吗？")
            .setNegativeButton("取消", null)
            .setPositiveButton("删除") { _, _ ->
                apiRequest("DELETE", "/api/medicines/$slot", null, {
                    showToast("已删除")
                    afterDelete()
                }, {
                    showToast("删除失败：$it")
                })
            }
            .show()
    }

    // ═══════════════════════════════════════════
    //  健康助手
    // ═══════════════════════════════════════════

    private fun showHealthMode() {
        val (_, content) = root("健康助手")

        val healthCard = card(Color.parseColor("#FEF2F2"))
        val iconView = ImageView(this).apply {
            setImageResource(R.drawable.ic_health)
            val s = dp(48)
            layoutParams = LinearLayout.LayoutParams(s, s).apply { gravity = Gravity.CENTER_HORIZONTAL }
        }
        healthCard.addView(iconView)
        healthCard.addView(TextView(this).apply {
            text = "AI 智能用药推荐"
            textSize = sp(12)
            setTextColor(clrOnSurface)
            gravity = Gravity.CENTER
            typeface = Typeface.DEFAULT_BOLD
            setPadding(0, dp(8), 0, 0)
        })
        content.addView(healthCard)

        val symptomEdit = EditText(this).apply {
            hint = "输入症状，例如：我有点发烧头疼"
            textSize = sp(11)
            setPadding(dp(16), dp(14), dp(16), dp(14))
            minLines = 3
            gravity = Gravity.TOP
            background = roundedBg(12, clrSurface, Color.parseColor("#FECACA"), 1.5f.toInt())
        }
        content.addView(symptomEdit, LinearLayout.LayoutParams(-1, dp(120)).apply {
            setMargins(0, dp(12), 0, dp(12))
        })

        val resultText = TextView(this).apply {
            text = "推荐结果将显示在这里"
            textSize = sp(11)
            setTextColor(clrOnSurfaceVar)
            setPadding(dp(16), dp(16), dp(16), dp(16))
            background = roundedBg(12, clrSurface, Color.parseColor("#E2E8F0"), 1)
        }

        content.addView(iconButton("获取 AI 推荐", R.drawable.ic_health, clrError, {
            val text = symptomEdit.text.toString().trim()
            if (text.isBlank()) {
                showToast("请先输入症状")
            } else {
                resultText.text = "正在咨询健康助手..."
                apiRequest("POST", "/api/health/recommend", JSONObject().put("symptom", text), { json ->
                    resultText.text = "推荐：${json.optString("result", "无对应药品")}"
                }, {
                    resultText.text = "推荐失败：$it"
                })
            }
        }))

        content.addView(resultText)
    }
}
