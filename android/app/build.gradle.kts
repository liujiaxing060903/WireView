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
        versionCode = 20002
        versionName = "2.0.2"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            isDebuggable = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
