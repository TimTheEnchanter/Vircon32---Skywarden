#!/bin/sh
# Builds Skywarden.v32 with the Vircon32 development tools (compile, assemble,
# png2vircon, wav2vircon and packrom must be on your PATH).
set -e
cd "$(dirname "$0")"
mkdir -p obj bin
compile src/main.c -o obj/main.asm
assemble obj/main.asm -o obj/main.vbin
png2vircon assets/sprites.png -o obj/sprites.vtex
for s in laser boom big_boom smart_bomb hyperspace abduct mutate catch extra_life wave_clear blip enemy_shot materialize; do
    wav2vircon assets/$s.wav -o obj/$s.vsnd
done
packrom RomDefinition.xml -o bin/Skywarden.v32
echo "Built bin/Skywarden.v32"
