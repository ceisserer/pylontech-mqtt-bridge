#!/bin/sh

# all paths are relative to this script's directory (dpkg/ in the project)
cd "$(dirname "$0")" || exit 1

# build binary
make -C .. pylon2mqtt || { echo "Error building pylon2mqtt"; exit 1; }

cd content

# copy files to debian package structure
mkdir -p usr/bin/
mkdir -p etc/pylon2mqtt/
mkdir -p lib/systemd/system/
cp -v ../../pylon2mqtt                  usr/bin/
cp -v ../../pylon_template.json         etc/pylon2mqtt/
cp -v ../../systemd/pylon2mqtt.service  lib/systemd/system/

dpkg-buildpackage -uc -us

if [ $? -ne 0 ]; then
  echo "Error creating debian package"
  exit 1
fi

cd ..
