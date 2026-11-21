plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "app.doors.companion"
    compileSdk = 36

    defaultConfig {
        applicationId = "app.doors.companion"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }

    // One debug and one release type, and no flavors: there is no variant in
    // which the trust rules differ (CONTRACTS.md section 2). SecurityLintTest
    // reads this file and fails if that changes.
    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlin {
        jvmToolchain(17)
    }
    buildFeatures {
        compose = true
    }
    testOptions {
        unitTests.all {
            // The integration suite talks to a real gatewayd when the harness
            // gives it one (harness/run-gateway.sh); without it, it is skipped.
            it.systemProperty("doors.gateway.uri", System.getProperty("doors.gateway.uri") ?: "")
            it.systemProperty("doors.gateway.ctl", System.getProperty("doors.gateway.ctl") ?: "")
            it.systemProperty("doors.project.dir", rootDir.absolutePath)
        }
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2025.06.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.animation:animation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.9.1")
    implementation("androidx.core:core-ktx:1.16.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")
    implementation("androidx.camera:camera-core:1.4.2")
    implementation("androidx.camera:camera-camera2:1.4.2")
    implementation("androidx.camera:camera-lifecycle:1.4.2")
    implementation("androidx.camera:camera-view:1.4.2")
    implementation("com.google.zxing:core:3.5.3")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.bouncycastle:bcpkix-jdk18on:1.79")
}
