# Skywarden for Vircon32

A side-scrolling planet-defense shooter for the Vircon32 console. The rules follow the 1982 Atari 2600 cartridge as documented in the commented disassembly by Nick Bensema & Manuel Polik.
The code, pixel art, sounds, enemy designs and names are all original to this project.

## Playing

Load `bin/Skywarden.v32` in the Vircon32 emulator or the RetroArch Vircon32 core.

| Control | Action |
|---|---|
| Left / Right | Thrust (pressing the other way turns the ship around) |
| Up / Down | Climb / dive |
| Hold A | Fire the laser |
| Dive into the city | Smart bomb: destroys everything on screen (one per dive) |
| Fly off the top of the sky | Hyperspace to a random spot (one jump per climb) |
| Start | Start / pause |
| D-pad on title | Choose game 1–6 |

The world wraps around, and the scanner at the top shows all of it. Grabbers hunt for colonists on the ground and carry them up. If one reaches the top of the sky, the colonist is lost and the grabber becomes a ravager.
Shoot a grabber that is carrying someone, then catch the falling colonist (500) and set them down (500). A colonist dropped from low enough lands safely on their own (250).
If every colonist is lost, the planet is destroyed and every grabber turns into a ravager.

## Taken from the disassembly

- **Point values** (tables `LFF00`/`LFEFA`): miner 250, hunter 200, hive 1000, stinger 500, grabber 150, ravager 150
- **Enemy slots per wave** (table `LFF18`): 1 miner, 2 hives, 4 grabbers and a dormant hunter. If the planet is gone, every slot is a ravager (the `$05` override).
- **Starting stock:** 3 ships and 3 smart bombs (`LFDE3`). A bonus ship and a smart bomb are always awarded together (`LFD25`).
- **Planet restored every 5 waves:** the reset runs when the wave number ends in 0 or 5 (`LFD99`).
- **Starting wave per game** (table `LFEE8`): wave 1, 3 or 5
- **"Barren" games** (table `LFECD`) start with no planet to protect.
- **Single shot:** the ship has one laser shot at a time, and is invisible (can't be hit) while that shot is in flight.
- **City:** the ship isn't drawn inside the city band, and diving into it is how the smart bomb is triggered.

## My choices where the disassembly is unclear or silent

- Bonus every 10,000 points, which is what I recall from the cartridge's manual; the threshold check isn't labeled in the disassembly
- 6 colonists
- The hunter wakes after 20 seconds and then returns every 10 seconds
- 3 stingers per hive
- Enemy speeds and fire rates, which rise each wave
- The 6 games combine starting wave 1/3/5 with a normal or barren planet. The cartridge had more variants, but I couldn't reliably decode what the others change.
- The ship is drawn faintly while it's invisible, so you can still steer.

## Building

With the Vircon32 dev tools on your PATH (`compile`, `assemble`, `png2vircon`, `wav2vircon`, `packrom`):

```
./build.sh
```

`tools/make_assets.py` (needs Pillow) regenerates `assets/` and `src/regions.h`. Edit the sprite strings or sound recipes there, rerun it, then rebuild:

```
python3 tools/make_assets.py assets src
```
