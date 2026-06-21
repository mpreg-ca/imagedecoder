plugins {
    alias(libs.plugins.android.library)
    id("com.vanniktech.maven.publish") version "0.36.0"
}

val gitCommitId = providers.exec {
    commandLine("git", "rev-parse", "--short", "HEAD")
}.standardOutput.asText.map { it.trim() }.getOrElse("unknown")

val baseVersion = "2-$gitCommitId"

val isTag = System.getenv("GITHUB_REF_TYPE") == "tag"

android {
    namespace = "ca.mpreg.imagedecoder"
    compileSdk = 37

    defaultConfig {
        minSdk = 24

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

afterEvaluate {
    mavenPublishing {
        val version = if (isTag) baseVersion else "$baseVersion-SNAPSHOT"
        coordinates("ca.mpreg", "imagedecoder", version)

        pom {
            name.set("imagedecoder")
            description.set("imagedecoder")
            inceptionYear.set("2026")
            url.set("https://github.com/mpreg-ca/imagedecoder")
            licenses {
                license {
                    name.set("MIT License")
                    url.set("https://opensource.org")
                    distribution.set("repo")
                }
            }
            developers {
                developer {
                    id.set("wwww-wwww")
                    name.set("w")
                    url.set("https://github.com/wwww-wwww/")
                }
            }
            scm {
                url.set("https://github.com/mpreg-ca/imagedecoder/")
                connection.set("scm:git:git://github.com/mpreg-ca/imagedecoder.git")
                developerConnection.set("scm:git:ssh://git@github.com/mpreg-ca/imagedecoder.git")
            }
        }

        publishToMavenCentral(automaticRelease = true)
        signAllPublications()
    }
}
