plugins {
    id("com.android.application")
}

android {
    namespace = "com.wireview.receiver"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.wireview.receiver"
        minSdk = 30
        targetSdk = 36
        versionCode = 20
        versionName = "1.1.20"
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
