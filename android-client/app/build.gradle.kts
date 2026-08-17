plugins {
    id("com.android.application")
}

android {
    namespace = "com.androidwebcam"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.androidwebcam"
        minSdk = 31
        targetSdk = 37
        versionCode = 1
        versionName = "0.1"
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

    lint {
        abortOnError = false
    }
}
