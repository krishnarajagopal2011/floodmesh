plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "org.floodmesh.admin"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        applicationId = "org.floodmesh.admin"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        // 24 (Android 7.0): required by flutter_secure_storage and
        // shared_preferences_android; also the Flutter default.
        minSdk = 24
        targetSdk = flutter.targetSdkVersion
        // Uses the version code from pubspec.yaml. When using split APKs, 1000 * ABI_VERSION
        // is added automatically by Flutter. (https://developer.android.com/studio/build/configure-apk-splits#configure-APK-versions)
        // You can force using the value of versionCode by specifying the `-P force-version-code-ignoring-abi=true`
        // flag during build.
        versionCode = flutter.versionCode
        versionName = flutter.versionName
    }

    // Release signing. If FM_KEYSTORE_PATH points at a keystore (CI decodes
    // it from the FM_ANDROID_KEYSTORE_B64 repository secret), every build is
    // signed with that same key, so a new APK installs over the old one and
    // the stored admin identity survives. Without it the debug key is used:
    // fine for a first try, but each CI runner has a different debug key, so
    // updating means uninstalling, which deletes the admin identity unless it
    // was backed up first. See README.md.
    val fmKeystore = System.getenv("FM_KEYSTORE_PATH")?.let { file(it) }?.takeIf { it.exists() }
    signingConfigs {
        if (fmKeystore != null) {
            create("fmrelease") {
                storeFile = fmKeystore
                storePassword = System.getenv("FM_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("FM_KEY_ALIAS")
                keyPassword = System.getenv("FM_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = if (fmKeystore != null) {
                signingConfigs.getByName("fmrelease")
            } else {
                signingConfigs.getByName("debug")
            }
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}
