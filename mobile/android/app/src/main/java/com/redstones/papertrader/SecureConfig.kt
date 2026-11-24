package com.redstones.papertrader

import android.content.Context
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import org.json.JSONObject

/** KIS 모의투자 키/계좌를 Android Keystore 기반 암호화 저장소에 보관. 로그/알림에는 키를 쓰지 않음. */
object SecureConfig {
    private fun prefs(ctx: Context) = EncryptedSharedPreferences.create(
        ctx, "secure_cfg",
        MasterKey.Builder(ctx).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build(),
        EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
        EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
    )

    fun get(ctx: Context, k: String, def: String = "") = prefs(ctx).getString(k, def) ?: def
    fun put(ctx: Context, k: String, v: String) = prefs(ctx).edit().putString(k, v).apply()
    fun autoEnabled(ctx: Context) = get(ctx, "auto") == "1"
    fun maxNames(ctx: Context) = get(ctx, "max_names", "0").toIntOrNull() ?: 0
    fun complete(ctx: Context) = listOf("appkey", "appsecret", "cano").all { get(ctx, it).isNotBlank() }

    /** Python(rl/live/android_entry.py)으로 넘길 설정 JSON -- 모의투자 고정. */
    fun cfgJson(ctx: Context): String = JSONObject()
        .put("mode", "paper")
        .put("appkey", get(ctx, "appkey"))
        .put("appsecret", get(ctx, "appsecret"))
        .put("cano", get(ctx, "cano"))
        .put("acnt_prdt_cd", get(ctx, "acnt_prdt_cd", "01"))
        .toString()
}
