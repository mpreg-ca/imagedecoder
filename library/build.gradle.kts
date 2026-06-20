plugins {
    alias(libs.plugins.android.library)
    id("com.vanniktech.maven.publish") version "0.36.0"
}

// val gitCommitId = providers.exec {
//     commandLine("git", "rev-parse", "--short", "HEAD")
// }.standardOutput.asText.map { it.trim() }.getOrElse("unknown")

// val baseVersion = "1-$gitCommitId"

// val isTag = System.getenv("GITHUB_REF_TYPE") == "tag"

android {
    namespace = "ca.mpreg.imagedecoder"
    compileSdk = 37

    defaultConfig {
        minSdk = 24

        ndk { abiFilters += listOf("arm64-v8a") }

        externalNativeBuild {
            cmake {
                cppFlags("-O3 -flto")
                targets("ep_imagedecoder")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {
}
