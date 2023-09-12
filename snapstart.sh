#!/bin/sh

snap remove --purge wpe-webkit-mir-kiosk
scp root@147.182.156.116:/root/cog/build/launcher/cog /root/snap-tree/squashfs-root/usr/bin/cog
snap try /root/snap-tree/squashfs-root
sleep 3
snap connect wpe-webkit-mir-kiosk:wayland
sleep 3
snap set wpe-webkit-mir-kiosk daemon=true
sleep 3
snap set wpe-webkit-mir-kiosk url=https://mir-server.io
sleep 3
snap start wpe-webkit-mir-kiosk
sleep 3
sudo snap set wpe-webkit-mir-kiosk devmode=true
