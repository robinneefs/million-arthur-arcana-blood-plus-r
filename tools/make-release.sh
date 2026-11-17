#!/bin/sh
# Builds a release: dist/ArcanaBlood-PlusR-v<version>.zip (unzip into the game's folder),
# dist/ArcanaBlood-PlusR-v<version>-Setup.exe (installer) and SHA256SUMS.txt.
# Needs: MinGW-w64 (i686), zip, nsis (apt install g++-mingw-w64-i686 zip nsis).
# The version comes from src/mod/version.h (MOD_VERSION, MOD_BUILD).
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^#define MOD_VERSION "\(.*\)"/\1/p' src/mod/version.h)
BUILD=$(sed -n 's/^#define MOD_BUILD \([0-9]*\).*/\1/p' src/mod/version.h)
[ -n "$VER" ] && [ -n "$BUILD" ] || { echo "no MOD_VERSION / MOD_BUILD in src/mod/version.h"; exit 1; }
VNUM=$(echo "$VER" | sed 's/-.*//')
NAME="ArcanaBlood-PlusR-v$VER"
sh build.sh > /dev/null
rm -rf "dist/$NAME" "dist/$NAME.zip" "dist/$NAME-Setup.exe" dist/SHA256SUMS.txt
mkdir -p "dist/$NAME/ArcanaPlusR/fonts"
cp build/dinput8.dll "dist/$NAME/"
cp build/ArcanaPlusR/arcana_rollback.dll assets/rollback_logo.png config/rollback.ini "dist/$NAME/ArcanaPlusR/"
cp fonts/* "dist/$NAME/ArcanaPlusR/fonts/"
# Windows line endings for the text files players open
sed 's/\r*$/\r/' config/README.txt > "dist/$NAME/ArcanaPlusR/README.txt"
sed 's/\r*$/\r/' LICENSE > "dist/$NAME/ArcanaPlusR/LICENSE.txt"
cp "dist/$NAME/ArcanaPlusR/README.txt" "dist/$NAME/README - Arcana Blood +R.txt"
sed -i 's/\r*$/\r/' "dist/$NAME/ArcanaPlusR/rollback.ini"
# the zip holds the files as they go into the game folder (no extra folder level)
(cd "dist/$NAME" && zip -qr "../$NAME.zip" .)
makensis -V2 -DVERSION="$VER" -DVERSION_RC="$VNUM.$BUILD" -DICON="$(pwd)/assets/icon/arcana_plusr.ico" -DSRC="$(pwd)/dist/$NAME" -DOUT="$(pwd)/dist/$NAME-Setup.exe" installer/installer.nsi
(cd dist && sha256sum "$NAME.zip" "$NAME-Setup.exe" > SHA256SUMS.txt)
echo "dist/$NAME.zip"
echo "dist/$NAME-Setup.exe"
cat dist/SHA256SUMS.txt
