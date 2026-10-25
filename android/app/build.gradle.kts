plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// The board's firmware version is the app's too (one release, one number).
val fwVersion: String = run {
    val v = rootProject.file("../src/Version.h").readText()
    Regex("""#define MEGAOPEN_VERSION "([0-9.]+)"""").find(v)?.groupValues?.get(1) ?: "0.0.0"
}
val code: Int = fwVersion.split(".").map { it.toInt() }.let { (a, b, c) -> a * 10000 + b * 100 + c }

android {
    namespace = "ua.megaopen.voice"
    compileSdk = 34
    defaultConfig {
        applicationId = "ua.megaopen.voice"
        minSdk = 24
        // 33, not 34: a sideloaded app on head units (Android 10-13), and 34
        // would make the foreground service declare a type it does not fit.
        targetSdk = 33
        versionCode = code
        versionName = fwVersion
    }
    // The release key lives in the repo's secrets (release.yml writes it to
    // MO_KEYSTORE); one key for every release, so each installs over the last.
    // Without it (CI on a push, a local build) the debug key signs.
    val ks = System.getenv("MO_KEYSTORE")?.let { file(it) }?.takeIf { it.exists() }
    signingConfigs {
        if (ks != null) create("release") {
            storeFile = ks
            storeType = "pkcs12"
            storePassword = System.getenv("MO_KEYSTORE_PASS")
            keyAlias = "megaopen"
            keyPassword = System.getenv("MO_KEYSTORE_PASS")
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (ks != null) "release" else "debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    // Lint's Play-store rules (target SDK age) do not apply to a sideloaded app.
    lint {
        checkReleaseBuilds = false
        abortOnError = false
    }
}
