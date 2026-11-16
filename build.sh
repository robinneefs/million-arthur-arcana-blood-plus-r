#!/bin/sh
# Builds the mod DLL and the launcher as 32-bit Windows programs with no
# runtime dependencies (no .NET, no Visual C++ redistributable).
# Needs MinGW-w64 (i686), e.g. on Ubuntu: apt install g++-mingw-w64-i686
set -e
cd "$(dirname "$0")"
mkdir -p build/obj
CXX=i686-w64-mingw32-g++
FLAGS="-std=c++17 -O2 -fno-omit-frame-pointer -static -static-libgcc -static-libstdc++"
IMGUI="-Ithird_party/imgui -DIM_ASSERT(_EXPR)=((void)0) -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD -DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS"
# Dear ImGui (overlay), only rebuilt when its sources change
for f in imgui imgui_draw imgui_widgets imgui_tables backends/imgui_impl_dx9 backends/imgui_impl_win32; do
  o=build/obj/$(basename $f).o
  [ -f $o ] && [ $o -nt third_party/imgui/$f.cpp ] || $CXX $FLAGS $IMGUI -c third_party/imgui/$f.cpp -o $o
done
$CXX $FLAGS $IMGUI -Wall -Wno-attributes -shared -o build/arcana_rollback.dll \
  src/mod/mod.cpp src/mod/util.cpp src/mod/transport.cpp build/obj/*.o -lws2_32 -ldwmapi -lgdi32 -lole32 -lshell32
$CXX $FLAGS -Wall -municode -o build/arcana_launcher.exe src/launcher/launcher.cpp
i686-w64-mingw32-strip build/arcana_rollback.dll build/arcana_launcher.exe
cp config/rollback.ini assets/rollback_logo.png build/
mkdir -p build/fonts && cp fonts/* build/fonts/
ls -la build
