package com.redstones.papertrader

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import com.chaquo.python.Python
import com.chaquo.python.android.AndroidPlatform
import org.json.JSONObject
import java.io.File
import java.time.LocalDateTime

/**
 * 포그라운드 서비스에서 Python 실행기(rl/live/android_entry.py)를 호출.
 * 화면이 꺼져 있어도 돌도록 부분 웨이크락을 잡고, 끝나면 결과를 알림으로 띄움.
 */
class TradeService : Service() {
    companion object {
        const val ACT_SCHEDULED = "scheduled"   // 알람: 실제 주문(정규장/휴장/데이터 검사 적용)
        const val ACT_EXECUTE = "execute"       // 수동 실제 주문(검사 적용)
        const val ACT_DRY = "dry"               // 드라이런(주문 없음, 시간 검사 무시)
        const val ACT_CHECK = "check"           // 연결 점검(조회만)
        const val ACT_PREFETCH = "prefetch"     // 첫 데이터 준비(KIS 3년치, ~1시간)
        private const val CH = "trade"
        @Volatile var running = false

        fun start(ctx: Context, act: String) {
            ctx.startForegroundService(Intent(ctx, TradeService::class.java).putExtra("act", act))
        }

        fun stateDir(ctx: Context): String = File(ctx.filesDir, "state").apply { mkdirs() }.absolutePath
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun notif(text: String, ongoing: Boolean): Notification =
        Notification.Builder(this, CH)
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setContentTitle("모의투자 봇")
            .setContentText(text.lineSequence().firstOrNull() ?: "")
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setOngoing(ongoing)
            .build()

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CH, "모의투자 실행", NotificationManager.IMPORTANCE_DEFAULT))
        val act = intent?.getStringExtra("act") ?: ACT_DRY
        val first = notif("실행 중: $act", true)
        if (Build.VERSION.SDK_INT >= 29) startForeground(1, first, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        else startForeground(1, first)
        if (running) {  // 이미 실행 중이면 겹치지 않게
            stopSelf(startId); return START_NOT_STICKY
        }
        running = true
        val wl = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "papertrader:run").apply { acquire(90 * 60 * 1000L) }
        Thread {
            val summary = try {
                if (!SecureConfig.complete(this)) "설정 없음: 앱에서 appkey/appsecret/계좌번호를 먼저 저장하세요"
                else callPython(act)
            } catch (e: Throwable) {
                "[앱 오류] ${e.javaClass.simpleName}: ${e.message}"
            }
            SecureConfig.put(this, "last_result", "${LocalDateTime.now().withNano(0)} [$act]\n$summary")
            nm.notify(2, notif(summary, false))
            if (wl.isHeld) wl.release()
            running = false
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }.start()
        return START_NOT_STICKY
    }

    private fun callPython(act: String): String {
        if (!Python.isStarted()) Python.start(AndroidPlatform(this))
        val m = Python.getInstance().getModule("rl.live.android_entry")
        val cfg = SecureConfig.cfgJson(this)
        val sd = stateDir(this)
        val n = SecureConfig.maxNames(this)
        val out = when (act) {
            ACT_CHECK -> m.callAttr("check", cfg, sd)
            ACT_PREFETCH -> m.callAttr("prefetch", cfg, sd)
            ACT_DRY -> m.callAttr("run", cfg, false, sd, n, true)
            else -> m.callAttr("run", cfg, true, sd, n, false)   // ACT_SCHEDULED, ACT_EXECUTE
        }.toString()
        return JSONObject(out).optString("summary", out)
    }
}
