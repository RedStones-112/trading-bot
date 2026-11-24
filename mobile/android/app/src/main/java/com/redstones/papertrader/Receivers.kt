package com.redstones.papertrader

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** 예약 시각 도달 -> 실행 서비스 시작 + 다음 평일 예약. */
class AlarmReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        TradeService.start(ctx, TradeService.ACT_SCHEDULED)
        if (SecureConfig.autoEnabled(ctx)) Scheduler.schedule(ctx)
    }
}

/** 재부팅/앱 업데이트 후에는 알람이 사라지므로 다시 예약. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        if (SecureConfig.autoEnabled(ctx)) Scheduler.schedule(ctx)
    }
}
