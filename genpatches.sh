#!/bin/bash
git diff --relative=xserver-xorg-input-evdev-2.3.2/ 992d32c20d766936aa6e1756d767c3f95830c338..HEAD xserver-xorg-input-evdev-2.3.2/src/*.{c,h} xserver-xorg-input-evdev-2.3.2/include/*.h > xorg-input-evdev-pscroll.patch
git diff --relative=xorg-server-1.7.6/ 9c37fd15597771efa575af878ee3d88e4ab3b7a0..HEAD xorg-server-1.7.6/ > xorg-server-no_integration.patch
git diff --relative=xserver-xorg-input-synaptics-1.2.2/ e82c3b1a2a3b51fc0275dcb824f6ff41be083834..HEAD xserver-xorg-input-synaptics-1.2.2/ > xorg-input-synaptics-pscroll.patch
