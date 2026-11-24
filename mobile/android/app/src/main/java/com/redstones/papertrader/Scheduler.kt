package com.redstones.papertrader

import android.app.AlarmManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import java.time.DayOfWeek
import java.time.ZoneId
import java.time.ZonedDateTime

/** 평일(월~금) 23:40 KST = 미국 동부 09:40(서머타임 10:40), 정규장 중. 미국 휴장일 판정은 Python(runner)이 함. */
object Scheduler {
    private val KST = ZoneId.of("Asia/Seoul")
    private const val HOUR = 23
    private const val MINUTE = 40

    fun next(now: ZonedDateTime = ZonedDateTime.now(KST)): ZonedDateTime {
        var t = now.withHour(HOUR).withMinute(MINUTE).withSecond(0).withNano(0)
        if (!t.isAfter(now)) t = t.plusDays(1)
        while (t.dayOfWeek == DayOfWeek.SATURDAY || t.dayOfWeek == DayOfWeek.SUNDAY) t = t.plusDays(1)
        return t
    }

    private fun pending(ctx: Context) = PendingIntent.getBroadcast(
        ctx, 1, Intent(ctx, AlarmReceiver::class.java),
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
    )

    fun schedule(ctx: Context): ZonedDateTime {
        val am = ctx.getSystemService(AlarmManager::class.java)
        val at = next()
        val ms = at.toInstant().toEpochMilli()
        if (Build.VERSION.SDK_INT >= 31 && !am.canScheduleExactAlarms()) {
            am.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, ms, pending(ctx))  // 정확 알람 권한 없으면 근사 시각
        } else {
            am.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, ms, pending(ctx))
        }
        SecureConfig.put(ctx, "next_run", at.toString())
        return at
    }

    fun cancel(ctx: Context) {
        ctx.getSystemService(AlarmManager::class.java).cancel(pending(ctx))
        SecureConfig.put(ctx, "next_run", "")
    }
}
