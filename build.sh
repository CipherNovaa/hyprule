#!/bin/bash
# HyprRule build + install script
# Run from the hyprule/ project root

set -e

echo "==> Checking dependencies..."
if ! pkg-config --exists gtk4; then
    echo "GTK4 not found. Install with: sudo pacman -S gtk4"
    exit 1
fi

echo "==> Building..."
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..

echo "==> Installing to ~/.local/bin/hyprule ..."
mkdir -p "$HOME/.local/bin"
cp build/hyprule "$HOME/.local/bin/hyprule"

echo "==> Installing the snap-category helper to ~/.local/bin/hyprule-snap-add ..."
install -m 755 daemon/hyprule-snap-add "$HOME/.local/bin/hyprule-snap-add"

echo "==> Creating .desktop entry..."
mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/hyprule.desktop" << EOF
[Desktop Entry]
Name=HyprRule
Comment=Hyprland workspace & window-rule manager
Exec=$HOME/.local/bin/hyprule
Icon=preferences-system-windows
Terminal=false
Type=Application
Categories=Settings;System;
EOF

update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true

echo ""
echo "Done! Run with: hyprule"
echo "Or launch from Rofi by searching 'HyprRule'"
