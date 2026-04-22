#!/data/data/com.termux/files/usr/bin/bash
cd unoapp

echo -e "Compiling...\n"
clang uno.c -o uno

cp uno $HOME/bin
