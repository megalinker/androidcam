import java.io.File

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.phonecam"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.phonecam"
        // 23, not 21. The USB video path calls MediaCodec.setCallback(Callback, Handler) — the
        // Handler overload is API 23 — and it MUST pass a Handler, because without one MediaCodec
        // dispatches encoder output on the main looper and the socket write throws
        // NetworkOnMainThreadException. On API 21/22 that call would have been a NoSuchMethodError at
        // runtime, i.e. the cable path was already broken there; declaring 21 only hid it.
        // Nothing is really lost: foregroundServiceType is API 29, the FOREGROUND_SERVICE permission
        // is 28, and this app is a Camera2 + WebRTC client. Android 6.0 is a floor it already had.
        minSdk = 23
        targetSdk = 34
        versionCode = 43
        versionName = "0.6.5"

        ndk {
            // Ship only ARM ABIs: the org.webrtc native lib is large and x86/x86_64 are emulator-only
            // for a phone-camera app, so this ~halves the APK. Drop this filter (or add an emulator
            // flavor) if you need to run on an x86/x86_64 Android emulator. (F-20)
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }
    }

    // Release signing: CI decodes the keystore secret to a file and points these env vars at it,
    // so every build shares one signature and users can update in place. Without the secret
    // (local builds / forks) we fall back to debug signing so the build still works.
    val ksPath = System.getenv("PHONECAM_KEYSTORE")
    val hasReleaseKey = ksPath != null && File(ksPath).let { it.exists() && it.length() > 0 }
    signingConfigs {
        if (hasReleaseKey) {
            create("release") {
                storeFile = File(ksPath!!)
                storePassword = System.getenv("PHONECAM_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("PHONECAM_KEY_ALIAS") ?: "phonecam"
                keyPassword = System.getenv("PHONECAM_KEY_PASSWORD") ?: System.getenv("PHONECAM_KEYSTORE_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName(if (hasReleaseKey) "release" else "debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        buildConfig = true   // expose BuildConfig.VERSION_NAME to show the version in-app
    }

    testOptions {
        unitTests {
            // The diagnostics ring and the status wire format are plain JVM logic, but they brush
            // against android.util.Log / SystemClock. Returning defaults instead of throwing lets them
            // be tested on the JVM (fast, no device) without introducing a mocking framework.
            isReturnDefaultValues = true
        }
    }
}

kotlin {
    // Kotlin 2.3 removed the old kotlinOptions DSL; set the JVM target here.
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("com.google.android.material:material:1.12.0")   // Material 3 UI
    implementation("com.journeyapps:zxing-android-embedded:4.3.0")  // scan the PC's pairing QR

    // WebRTC media stack (org.webrtc.*) — the phone's only transport. Pre-built and maintained;
    // handles Oboe/AAudio low-latency capture, Opus + H.264 (via MediaCodec), and DTLS-SRTP.
    implementation("io.getstream:stream-webrtc-android:1.3.10")

    testImplementation("junit:junit:4.13.2")
}
