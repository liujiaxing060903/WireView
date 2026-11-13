plugins {
    id("com.android.application")
}

android {
    namespace = "com.wireview.receiver"
    compileSdk = 36

    buildFeatures { buildConfig = true }

    defaultConfig {
        buildConfigField("boolean", "CURSOR_DIAGNOSTICS",
            providers.gradleProperty("wireviewCursorDiagnostics").orElse("false").get())
        applicationId = "com.wireview.receiver"
        minSdk = 23
        targetSdk = 36
        versionCode = 20000
        versionName = "2.0.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
