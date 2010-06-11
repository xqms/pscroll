#!/bin/bash
git diff --relative=xserver-xorg-input-evdev-2.3.2/ 992d32c20d766936aa6e1756d767c3f95830c338..HEAD xserver-xorg-input-evdev-2.3.2/src/*.{c,h} xserver-xorg-input-evdev-2.3.2/include/*.h > xorg-input-evdev-pscroll.patch
git diff --relative=xorg-server-1.7.6/ 5d3e8ddc3ff7f872f1e240f7fb83f68ff92a21e7..HEAD xorg-server-1.7.6/ > xorg-server-no_integration.patch
