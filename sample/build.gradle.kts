plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "ca.mpreg.imagedecoder.test"
    compileSdk = 37

    defaultConfig {
        applicationId = "ca.mpreg.imagedecoder.test"
        minSdk = 24

        versionCode = 1
        versionName = "1.0"

        defaultConfig {
            packaging {
                jniLibs.keepDebugSymbols.addAll(listOf("*/mips/*.so", "*/mips64/*.so"))
            }
        }
    }

    sourceSets {
        getByName("main").assets.directories.add("assets")
    }
    buildTypes {
        release {
            optimization {
                enable = false
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
    }
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(project(":library"))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
}