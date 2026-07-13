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
        versionCode = 23
        versionName = "0.4.19"
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

    // RootEncoder — the phone acts as an RTSP *server* (phone hosts, the PC pulls).
    // RTSP-Server 1.4.1 is the tested pair with RootEncoder core 2.7.2. Both come from JitPack
    // (see the repositories block in ../settings.gradle.kts). Bump together after checking the
    // RTSP-Server release notes for the core version it was tested against.
    implementation("com.github.pedroSG94:RTSP-Server:1.4.1")
    implementation("com.github.pedroSG94.RootEncoder:library:2.7.2")
}
