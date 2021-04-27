#!/usr/bin/env bash
cd /home/michaelb/dev/obsidian
./create.sh
cd -
cd build
cmake ..
sudo make install
cd ..
