#!/bin/bash

git diff 858899e2b2da239f3ddf..$(git subtree split -P xf86-input-evdev 858899e2b2da239f3ddf..) > xf86-input-evdev.patch
git diff 512c58176a34d9e47980..$(git subtree split -P xf86-input-synaptics 512c58176a34d9e47980..) > xf86-input-synaptics.patch
git diff f993107c68111e74f5c4..$(git subtree split -P xserver f993107c68111e74f5c4..) > xserver.patch


