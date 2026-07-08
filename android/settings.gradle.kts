pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        // RootEncoder is published on JitPack (and recent versions on Maven Central).
        maven { url = uri("https://jitpack.io") }
    }
}

rootProject.name = "PhoneCam"
include(":app")
