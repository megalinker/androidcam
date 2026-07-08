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
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")

    // RootEncoder — the phone acts as an RTSP *server* (phone hosts, the PC pulls).
    // RTSP-Server 1.4.1 is the tested pair with RootEncoder core 2.7.2. Both come from JitPack
    // (see the repositories block in ../settings.gradle.kts). Bump together after checking the
    // RTSP-Server release notes for the core version it was tested against.
    implementation("com.github.pedroSG94:RTSP-Server:1.4.1")
    implementation("com.github.pedroSG94.RootEncoder:library:2.7.2")
}
