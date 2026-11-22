#!/bin/sh
# Builds the mod DLL and its loader (dinput8.dll) as 32-bit Windows DLLs with no
# runtime dependencies (no .NET, no Visual C++ redistributable).
# Needs MinGW-w64 (i686), e.g. on Ubuntu: apt install g++-mingw-w64-i686
# Result in build/ArcanaPlusR/: the launcher folder (launcher, mod, a copy of dinput8.dll that
# the launcher puts into the game folder, fonts, logo, settings).
set -e
cd "$(dirname "$0")"
mkdir -p build/obj build/res build/ArcanaPlusR/fonts
CXX=i686-w64-mingw32-g++
RES=i686-w64-mingw32-windres
FLAGS="-std=c++17 -O2 -fno-omit-frame-pointer -static -static-libgcc -static-libstdc++"
# miniz (deflate for Pack logs, build 121)
MZ="-Ithird_party/miniz -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES"
IMGUI="-Ithird_party/imgui -DIM_ASSERT(_EXPR)=((void)0) -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD -DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS"
# Dear ImGui (overlay), only rebuilt when its sources change
for f in imgui imgui_draw imgui_widgets imgui_tables backends/imgui_impl_dx9 backends/imgui_impl_win32; do
  o=build/obj/$(basename $f).o
  [ -f $o ] && [ $o -nt third_party/imgui/$f.cpp ] || $CXX $FLAGS $IMGUI -c third_party/imgui/$f.cpp -o $o
done
mkdir -p build/mz
[ -f build/mz/miniz.o ] && [ build/mz/miniz.o -nt third_party/miniz/miniz.c ] || i686-w64-mingw32-gcc -O2 $MZ -c third_party/miniz/miniz.c -o build/mz/miniz.o
$RES src/mod/mod.rc -O coff -o build/res/mod_res.o
$CXX $FLAGS $IMGUI $MZ -Wall -Wno-attributes -shared -o build/ArcanaPlusR/arcana_rollback.dll \
  src/mod/mod.cpp src/mod/util.cpp src/mod/transport.cpp build/obj/*.o build/mz/miniz.o build/res/mod_res.o -lws2_32 -ldwmapi -lgdi32 -lole32 -lshell32 -lwinhttp
# the loader: small, only what Windows has anyway
$RES src/loader/loader.rc -O coff -o build/res/loader_res.o
$CXX -O2 -Wall -static -static-libgcc -shared -o build/dinput8.dll src/loader/dinput8.cpp src/loader/dinput8.def build/res/loader_res.o
cp build/dinput8.dll build/ArcanaPlusR/
# the launcher
$RES src/launcher/launcher.rc -O coff -o build/res/launcher_res.o
$CXX $FLAGS $IMGUI -Wall -municode -mwindows -o "build/ArcanaPlusR/Arcana Blood +R.exe" src/launcher/launcher.cpp \
  build/obj/*.o build/res/launcher_res.o -ld3d9 -lwinhttp -lbcrypt -lversion -lole32 -lwindowscodecs -lshell32 -luuid -lgdi32 -ldwmapi
i686-w64-mingw32-strip build/ArcanaPlusR/arcana_rollback.dll build/ArcanaPlusR/dinput8.dll "build/ArcanaPlusR/Arcana Blood +R.exe"
cp config/rollback.ini assets/rollback_logo.png build/ArcanaPlusR/
cp fonts/* build/ArcanaPlusR/fonts/
ls -la build build/ArcanaPlusR
