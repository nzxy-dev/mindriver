#!/usr/bin/env bash

if [ "$1" = "clean" ]; then
    if [ -f "bin/out/linux/mindriver" ]; then
        rm -f "bin/out/linux/mindriver"
    fi
    if [ -d "bin/00Temp" ]; then
        rm -rf "bin/00Temp"
    fi
    echo "[clean] done"
    exit 0
fi

echo "[build] compiling mindriver with gcc for Linux x64 (Release)..."
mkdir -p "bin/out/linux"
mkdir -p "bin/00Temp"

GCCEXE="${GCC:-gcc}"

"$GCCEXE" -O2 -DNDEBUG -Wall -I. -Ilinux -c main.c -o bin/00Temp/main.o || { echo "[build] FAILED with errorlevel $?"; exit 1; }
"$GCCEXE" -O2 -DNDEBUG -Wall -I. -Ilinux -c linux/initcore.c -o bin/00Temp/initcore.o || { echo "[build] FAILED with errorlevel $?"; exit 1; }
"$GCCEXE" -O2 -DNDEBUG -Wall -I. -Ilinux -c linux/resolver.c -o bin/00Temp/resolver.o || { echo "[build] FAILED with errorlevel $?"; exit 1; }
"$GCCEXE" -O2 -DNDEBUG -Wall -I. -Ilinux -c linux/servicefunc.c -o bin/00Temp/servicefunc.o || { echo "[build] FAILED with errorlevel $?"; exit 1; }

"$GCCEXE" bin/00Temp/main.o bin/00Temp/initcore.o bin/00Temp/resolver.o bin/00Temp/servicefunc.o -o bin/out/linux/mindriver -lpthread -lasound -ldl

if [ $? -ne 0 ]; then
    echo "[build] FAILED with errorlevel $?"
    exit 1
fi

echo "[build] SUCCESS: bin/out/linux/mindriver"
