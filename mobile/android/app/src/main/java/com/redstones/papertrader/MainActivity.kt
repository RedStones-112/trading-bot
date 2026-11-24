package com.redstones.papertrader

import android.app.Activity
import android.app.AlarmManager
import android.app.AlertDialog
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.text.InputType
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.File

/** 설정 입력 + 수동 실행 + 자동 실행 on/off + 진행 상황 표시. 레이아웃 XML 없이 코드로 구성. */
class MainActivity : Activity() {
    private lateinit var status: TextView
    private val ui = Handler(Looper.getMainLooper())

    private fun field(root: LinearLayout, hint: String, key: String, secret: Boolean = false, number: Boolean = false) =
        EditText(this).apply {
            this.hint = hint
            setText(SecureConfig.get(this@MainActivity, key))
            inputType = when {
                secret -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
                number -> InputType.TYPE_CLASS_NUMBER
                else -> InputType.TYPE_CLASS_TEXT
            }
            root.addView(this)
        }

    private fun button(root: LinearLayout, label: String, onClick: () -> Unit) =
        Button(this).apply { text = label; setOnClickListener { onClick() }; root.addView(this) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(40, 40, 40, 40) }
        setContentView(ScrollView(this).apply { addView(root) })

        TextView(this).apply {
            text = "KIS 모의투자 전용 (exp8r 모델, 미국 주식). 평일 23:40 자동 실행.\n" +
                "키는 기기 암호화 저장소에만 보관됩니다."
            root.addView(this)
        }
        val appkey = field(root, "모의투자 App Key", "appkey", secret = true)
        val secret = field(root, "모의투자 App Secret", "appsecret", secret = true)
        val cano = field(root, "모의계좌번호 앞 8자리 (5로 시작)", "cano", number = true)
        val maxN = field(root, "매수 종목 수 상한 (0 = 모델 전체, 예: 60)", "max_names", number = true)

        button(root, "저장") {
            SecureConfig.put(this, "appkey", appkey.text.toString().trim())
            SecureConfig.put(this, "appsecret", secret.text.toString().trim())
            SecureConfig.put(this, "cano", cano.text.toString().trim())
            SecureConfig.put(this, "max_names", maxN.text.toString().trim().ifEmpty { "0" })
            refresh("저장됨")
        }
        button(root, "1) 연결 점검 (조회만)") { TradeService.start(this, TradeService.ACT_CHECK) }
        button(root, "2) 데이터 준비 (처음 1회, 약 1시간)") { TradeService.start(this, TradeService.ACT_PREFETCH) }
        button(root, "3) 드라이런 (주문 없이 계획만)") { TradeService.start(this, TradeService.ACT_DRY) }
        button(root, "지금 실제 주문 (정규장 시간에만)") {
            AlertDialog.Builder(this).setTitle("모의계좌에 주문을 보냅니다")
                .setMessage("정규장(23:30~06:00 KST, 서머타임 22:30~05:00)이 아니면 아무것도 안 합니다.")
                .setPositiveButton("주문") { _, _ -> TradeService.start(this, TradeService.ACT_EXECUTE) }
                .setNegativeButton("취소", null).show()
        }
        button(root, "자동 실행 켜기/끄기") {
            if (SecureConfig.autoEnabled(this)) {
                SecureConfig.put(this, "auto", "0"); Scheduler.cancel(this); refresh("자동 실행 꺼짐")
            } else {
                SecureConfig.put(this, "auto", "1"); val at = Scheduler.schedule(this); refresh("자동 실행 켜짐, 다음: $at")
            }
        }
        button(root, "권한/배터리 설정 열기") { askPermissions(force = true) }
        status = TextView(this).apply { setTextIsSelectable(true); root.addView(this) }
        askPermissions(force = false)
    }

    private fun askPermissions(force: Boolean) {
        if (Build.VERSION.SDK_INT >= 33) requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), 1)
        val am = getSystemService(AlarmManager::class.java)
        if (Build.VERSION.SDK_INT >= 31 && !am.canScheduleExactAlarms()) {
            startActivity(Intent(Settings.ACTION_REQUEST_SCHEDULE_EXACT_ALARM, Uri.parse("package:$packageName")))
        }
        val pm = getSystemService(PowerManager::class.java)
        if (force || !pm.isIgnoringBatteryOptimizations(packageName)) {
            startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
        }
    }

    private fun refresh(msg: String? = null) {
        val progress = File(TradeService.stateDir(this), "progress.txt").takeIf { it.exists() }?.readLines()?.takeLast(12)
        status.text = buildString {
            msg?.let { append(it).append("\n\n") }
            append("자동 실행: ").append(if (SecureConfig.autoEnabled(this@MainActivity)) "켜짐" else "꺼짐")
            append(", 다음: ").append(SecureConfig.get(this@MainActivity, "next_run").ifEmpty { "-" })
            append(if (TradeService.running) "\n[실행 중]" else "").append("\n\n최근 결과:\n")
            append(SecureConfig.get(this@MainActivity, "last_result").ifEmpty { "-" })
            progress?.let { append("\n\n진행 로그:\n").append(it.joinToString("\n")) }
        }
    }

    private val ticker = object : Runnable {
        override fun run() { refresh(); ui.postDelayed(this, 3000) }
    }

    override fun onResume() { super.onResume(); ui.post(ticker) }
    override fun onPause() { super.onPause(); ui.removeCallbacks(ticker) }
}
