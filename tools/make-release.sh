#!/bin/sh
# Builds a release: dist/ArcanaBlood-PlusR-v<version>.zip (what the launcher's update check
# downloads), dist/ArcanaBlood-PlusR-v<version>-Setup.exe (installer) and SHA256SUMS.txt.
# Needs: MinGW-w64 (i686), zip, nsis (apt install g++-mingw-w64-i686 zip nsis).
# The version comes from src/mod/version.h (MOD_VERSION).
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^#define MOD_VERSION "\(.*\)"/\1/p' src/mod/version.h)
[ -n "$VER" ] || { echo "no MOD_VERSION in src/mod/version.h"; exit 1; }
NAME="ArcanaBlood-PlusR-v$VER"
sh build.sh > /dev/null
rm -rf "dist/$NAME" "dist/$NAME.zip" "dist/$NAME-Setup.exe" dist/SHA256SUMS.txt
mkdir -p "dist/$NAME/fonts"
cp build/arcana_launcher.exe build/arcana_rollback.dll assets/rollback_logo.png config/rollback.ini "dist/$NAME/"
cp fonts/* "dist/$NAME/fonts/"
# Windows line endings for the text files players open
for f in config/README.txt LICENSE; do
  b=$(basename "$f" .txt)
  sed 's/\r*$/\r/' "$f" > "dist/$NAME/$b.txt"
done
sed -i 's/\r*$/\r/' "dist/$NAME/rollback.ini"
(cd dist && zip -qr "$NAME.zip" "$NAME")
makensis -V2 -DVERSION="$VER" -DICON="$(pwd)/assets/icon/arcana_plusr.ico" -DSRC="$(pwd)/dist/$NAME" -DOUT="$(pwd)/dist/$NAME-Setup.exe" installer/installer.nsi
(cd dist && sha256sum "$NAME.zip" "$NAME-Setup.exe" > SHA256SUMS.txt)
echo "dist/$NAME.zip"
echo "dist/$NAME-Setup.exe"
cat dist/SHA256SUMS.txt
