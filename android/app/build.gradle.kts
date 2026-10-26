plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// The board's firmware version is the app's too: one release, one number.
val fwVersion: String = run {
    val v = rootProject.file("../src/Version.h").readText()
    Regex("""#define MEGAOPEN_VERSION "([0-9.]+)"""").find(v)?.groupValues?.get(1) ?: "0.0.0"
}
val code: Int = fwVersion.split(".").map { it.toInt() }.let { (a, b, c) -> a * 10000 + b * 100 + c }
// The release tag this APK is built for (release.yml sets RELEASE_TAG; "" for a
// local build): an APK put on by hand still knows it is an rc and sees the next.
val releaseTag: String = providers.environmentVariable("RELEASE_TAG").getOrElse("")

// The few things a fork changes, in one place (docs/design/companion-app.md).
val releasesRepo = "Barbedx/MegaOpen"
// GitHub over https only: the board's own list (src/net/GhRelease.cpp, trustedUrl).
val netHosts = listOf("api.github.com", "github.com", "objects.githubusercontent.com",
    "release-assets.githubusercontent.com")
// The board's USB device (variants/t2can/pins_arduino.h; res/xml/usb_device_filter.xml).
val usbVid = 0x303A
val usbPid = 0x80C3

// The board's settings page, the same files the board serves (tools/export_page.py
// over tools/gen_pages.py), in the app's assets under page/. The app shows its own
// copy when the board runs the same firmware or has no page (FEATURE_WEB_PAGE=0).
val pageDir = layout.buildDirectory.dir("generated/page")
val python = providers.environmentVariable("MO_PYTHON").getOrElse("python3")
val exportPage by tasks.registering(Exec::class) {
    val repo = rootProject.projectDir.parentFile
    inputs.dir(File(repo, "web"))
    inputs.files(File(repo, "tools/gen_pages.py"), File(repo, "tools/export_page.py"), File(repo, "src/Version.h"))
    outputs.dir(pageDir)
    commandLine(python, File(repo, "tools/export_page.py").path, pageDir.get().dir("page").asFile.path)
}
tasks.named("preBuild") { dependsOn(exportPage) }

android {
    sourceSets["main"].assets.srcDir(pageDir)
    namespace = "ua.megaopen.companion"
    compileSdk = 34
    defaultConfig {
        applicationId = "ua.megaopen.companion"
        minSdk = 24
        targetSdk = 34
        versionCode = code
        versionName = fwVersion
        buildConfigField("String", "RELEASES_REPO", "\"$releasesRepo\"")
        buildConfigField("String", "RELEASE_TAG", "\"$releaseTag\"")
        buildConfigField("String[]", "NET_HOSTS", netHosts.joinToString(",", "{", "}") { "\"$it\"" })
        buildConfigField("int", "USB_VID", "$usbVid")
        buildConfigField("int", "USB_PID", "$usbPid")
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
    composeOptions { kotlinCompilerExtensionVersion = "1.5.14" }

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
    lint {
        // Lint is a gate: an error fails the build. Warnings are read, not fatal.
        abortOnError = true
        checkReleaseBuilds = true
        // A sideloaded app: the Play store's rules on target SDK age do not apply.
        disable += setOf("ExpiredTargetSdkVersion", "OldTargetApi", "GradleDependency", "NewerVersionAvailable")
    }
    testOptions { unitTests.isReturnDefaultValues = true }
    packaging { resources.excludes += "/META-INF/{AL2.0,LGPL2.1}" }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.06.00")
    implementation(composeBom)
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.3")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.3")
    implementation("androidx.lifecycle:lifecycle-service:2.8.3")
    implementation("androidx.navigation:navigation-compose:2.7.7")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.datastore:datastore-preferences:1.1.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    debugImplementation("androidx.compose.ui:ui-tooling")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")   // the real org.json on the JVM
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.8.1")
}
