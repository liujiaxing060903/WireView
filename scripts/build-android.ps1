[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$androidRoot = Join-Path $repoRoot 'android'
$gradleWrapper = Join-Path $androidRoot 'gradlew.bat'

$sdkCandidates = @(
    $env:ANDROID_HOME,
    $env:ANDROID_SDK_ROOT,
    $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'Android\Sdk' })
) | Where-Object { $_ } | Select-Object -Unique

$androidSdk = $sdkCandidates |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ 'platforms\android-36\android.jar') } |
    Select-Object -First 1

if (-not $androidSdk) {
    throw 'Android SDK platform 36 was not found. Set ANDROID_HOME or install it with Android Studio.'
}

$env:ANDROID_HOME = $androidSdk
$env:ANDROID_SDK_ROOT = $androidSdk

$gradleUserHome = if ($env:USERPROFILE) {
    Join-Path $env:USERPROFILE '.gradle'
} else {
    Join-Path $androidRoot '.gradle-user-home'
}
$env:GRADLE_USER_HOME = $gradleUserHome

& $gradleWrapper -p $androidRoot --gradle-user-home $gradleUserHome --offline :app:assembleDebug
if ($LASTEXITCODE -ne 0) {
    throw "Android build failed with exit code $LASTEXITCODE."
}

$apk = Join-Path $androidRoot 'app\build\outputs\apk\debug\app-debug.apk'
if (-not (Test-Path -LiteralPath $apk)) {
    throw "Gradle completed but the expected APK was not found: $apk"
}

Write-Host "Android APK: $apk"
