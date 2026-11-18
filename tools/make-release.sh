#!/bin/sh
# Builds a release in dist/:
#   ArcanaBlood-PlusR-v<version>-Setup.exe   installer
#   ArcanaBlood-PlusR-v<version>.zip         the same files in one folder (unzip anywhere, start
#                                            "Arcana Blood +R.exe"); the v0.1.0 launcher's updater
#                                            also takes this zip (first .zip of the release)
#   ArcanaBlood-PlusR-v<version>-files.tar   the same files, flat: what the launcher's updater
#                                            (build 120+) downloads; GitHub's asset digest checks it
#   SHA256SUMS.txt
# Upload all four to the GitHub release, the zip FIRST (older updaters take the first .zip).
# Needs: MinGW-w64 (i686), zip, tar, nsis (apt install g++-mingw-w64-i686 zip nsis).
# The version comes from src/mod/version.h (MOD_VERSION, MOD_BUILD).
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^#define MOD_VERSION "\(.*\)"/\1/p' src/mod/version.h)
BUILD=$(sed -n 's/^#define MOD_BUILD \([0-9]*\).*/\1/p' src/mod/version.h)
[ -n "$VER" ] && [ -n "$BUILD" ] || { echo "no MOD_VERSION / MOD_BUILD in src/mod/version.h"; exit 1; }
VNUM=$(echo "$VER" | sed 's/-.*//')
NAME="ArcanaBlood-PlusR-v$VER"
sh build.sh > /dev/null
rm -rf "dist/$NAME" "dist/$NAME.zip" "dist/$NAME-files.tar" "dist/$NAME-Setup.exe" dist/SHA256SUMS.txt
D="dist/$NAME/Arcana Blood +R"
mkdir -p "$D/fonts"
cp "build/ArcanaPlusR/Arcana Blood +R.exe" build/ArcanaPlusR/arcana_rollback.dll build/ArcanaPlusR/dinput8.dll \
   assets/rollback_logo.png config/rollback.ini "$D/"
cp fonts/* "$D/fonts/"
# Windows line endings for the text files players open
sed 's/\r*$/\r/' config/README.txt > "$D/README.txt"
sed 's/\r*$/\r/' LICENSE > "$D/LICENSE.txt"
sed -i 's/\r*$/\r/' "$D/rollback.ini"
(cd "dist/$NAME" && zip -qr "../$NAME.zip" "Arcana Blood +R")
# plain ustar, flat names, fixed owner and time, so the updater can read it without tar.exe
(cd "$D" && tar --format=ustar --owner=0 --group=0 --numeric-owner --mtime=@0 --sort=name -cf "../../$NAME-files.tar" \
   "Arcana Blood +R.exe" arcana_rollback.dll dinput8.dll rollback_logo.png rollback.ini README.txt LICENSE.txt fonts)
makensis -V2 -DVERSION="$VER" -DVERSION_RC="$VNUM.$BUILD" -DICON="$(pwd)/assets/icon/arcana_plusr.ico" -DSRC="$(pwd)/$D" -DOUT="$(pwd)/dist/$NAME-Setup.exe" installer/installer.nsi
(cd dist && sha256sum "$NAME-Setup.exe" "$NAME.zip" "$NAME-files.tar" > SHA256SUMS.txt)
ls -la "dist/$NAME.zip" "dist/$NAME-files.tar" "dist/$NAME-Setup.exe"
cat dist/SHA256SUMS.txt
