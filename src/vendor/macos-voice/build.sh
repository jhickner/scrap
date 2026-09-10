#!/bin/sh
# Builds and signs ../VoiceHelper.app. An identity in SIGN_IDENTITY keeps TCC grants across
# rebuilds; the ad-hoc default re-prompts for the microphone after every build.
set -eu
cd "$(dirname "$0")"
APP=${1:-../VoiceHelper.app}
swift build -c release --product VoiceHelper
BIN=$(swift build -c release --show-bin-path)/VoiceHelper
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp Info.plist "$APP/Contents/Info.plist"
cp "$BIN" "$APP/Contents/MacOS/VoiceHelper"
codesign --force --sign "${SIGN_IDENTITY:--}" --identifier local.c-libs.macos-voice "$APP"
echo "built $APP"
