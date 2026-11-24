plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("com.chaquo.python")
}

android {
    namespace = "com.redstones.papertrader"
    compileSdk = 34
    defaultConfig {
        applicationId = "com.redstones.papertrader"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"
        ndk { abiFilters += listOf("arm64-v8a") }   // 실기기. 에뮬레이터(x86_64)로 시험하려면 "x86_64" 추가
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

// 저장소의 Python 실행기(rl/features.py, rl/live/*)와 모델/종목목록을 빌드 때 복사 -> PC/라즈베리파이와 같은 코드 사용
val repoRoot = rootDir.resolve("../..")
val pySrc = layout.buildDirectory.dir("python-src")
val syncPython by tasks.registering(Sync::class) {
    from(repoRoot) {
        include("rl/__init__.py", "rl/features.py", "rl/live/*.py",
                "rl/live/model/exp8r.npz", "rl/live/universe464.json")
        exclude("rl/live/paper_trade.py")
    }
    into(pySrc)
}
tasks.named("preBuild") { dependsOn(syncPython) }

chaquopy {
    defaultConfig {
        version = "3.11"
        pip {
            install("numpy")
            install("pandas")
            install("requests")
        }
    }
    sourceSets {
        getByName("main") { srcDir(pySrc.get().asFile) }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.security:security-crypto:1.1.0-alpha06")
}
