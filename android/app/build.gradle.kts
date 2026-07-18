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
        minSdk = 21
        targetSdk = 34
        versionCode = 37
        versionName = "0.5.4"
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
}
