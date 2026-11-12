# Cabinet lights

The games drive their cabinet's lamps through the board's lamp registers. HIRAKU decodes them like MAME's `firebeat.cpp` does, and sends them to two places:
- **HID controller LEDs:** lit pop'n, IIDX and DIY controllers.
- **A local socket:** LED strips, microcontrollers, the launcher's live view, anything else.

It can also show them over the game's picture (below), with no hardware at all.

## The lights over the picture (F9)

Press **F9** in a game, or start it with `--lights-overlay` (launcher: Settings > Cabinet I/O > "Show the game's lights over its picture"), for a panel at the window's top left:
- every lamp HIRAKU decodes for that cabinet, grouped as in the table below;
- each lamp's output number in brackets: the bit in the board's output register (`[16]`), or the register and bit where the board has several (`[1.6]`), as `runtime/shell/cabinet.cpp` lists them;
- a square filled in the lamp's colour while the game has it lit (from its name: Blue, Red, Yellow, Pink...; amber otherwise), empty while it's off;
- how many times the lamps have changed since the game started, at the top: it moves whenever any lamp does.

It's for seeing what a game does with its lights: which output is which lamp, when each comes on, what a test mode's lamp check drives. Dance Maniax, for example: the 12 hand sensors' blue and red lamps (outputs 0–23, two per sensor), the three spotlights (28, 29, 31), the side lamps and the Start lamps (32–37, the LAMPS port). The panel goes on top of in-between pictures too (smooth motion), and changes nothing in the game.

**A log of every change:** `--lights-log FILE` (or `-` for the console) writes a line per lamp change, with or without a window: the frame, the machine's time in seconds, the lamp with its output number, and on or off (`f10233 170.6402 Spot Light Pink [31] on`). With a script (`--script`), the same moments come out on every run, so a lamp's pattern can be matched to what the game shows on those frames (`--snap-every`).

The easiest way to set this up is the launcher (`hiraku`, **Lights**). This page covers what the launcher writes and what it does under the hood.

## Lamps per cabinet

The names are the ones lights files and the socket use (table: `runtime/shell/cabinet.cpp`).

| Cabinet | Lamps |
|---|---|
| pop'n music | Button Lamp 1–9, Top Lamp 1–5 (1 = topmost), Side Lamp 1–4 (the pillars) |
| ParaParaParadise | Left Lamp, Right Lamp, Door Lamp, OK Lamp, Slim Lamp, Stage LED 1–8, Top LED 1–8, Lamp 1–4 |
| Keyboardmania | Start 1P Lamp, Start 2P Lamp, Door Lamp, Neon, Lamp 1–3 |
| beatmania III | none known yet: MAME does not decode its lamp registers |
| beatmania (DJ Main) | P1 Button Lamp 1–5, P2 Button Lamp 1–5, P1/P2 Start Lamp, Effect Lamp, Left/Right Red/Blue Highlight, Neon (SSR) (as MAME decodes them) |
| pop'n music 1–3 (DJ Main) | Button Lamp 1–9, Top Lamp 1–5, Side Lamp 1–4: the same names as Firebeat pop'n. Worked out from what pop'n music 1 does with the bits; which top lamp is topmost isn't known |
| pop'n stage (DJ Main) | not decoded yet: Light A0–A15, B0–B15 (the two registers' bits) |
| pop'n music 9–14 (Konami Python) | Button Lamp 1–9, Top Lamp 1–5, Side Lamp 1–4: the same names again. Read off the game's LAMP CHECK (test mode, I/O CHECK): SW-LAMP 1–9, HI-LAMP 1–5, LEFT-LAMP 1–2 then RIGHT-LAMP 1–2. Which top lamp is topmost isn't known |

Every cabinet also has **Status LED 1–8**: the board's diagnostic LEDs, which blink while the game runs, except the Konami Python ones (the interface board's LEDs aren't emulated).

## HID controller LEDs

```bash
build/cmake/hiraku-popn7 --window --lights ~/.config/hiraku/lights/popn.txt
```

A lights file maps lamps to HID outputs, one lamp per line, with `#` comments:

```
Button Lamp 1 = hid:1ccf:8048:0/0
Button Lamp 2 = hid:1ccf:8048:0/1, hid:1ccf:8048:0/9   # two LEDs for one lamp
```

`hid:<vendor>:<product>[:<interface>]/<n>` is the n-th output field (counting from 0) in the device's HID report descriptor. The IDs are in hex. The launcher's **Assign** dialog lists the fields by usage ("Ordinal 3", "LED: Generic Indicator", ...) and lights each one as you select it, so you don't have to count them yourself.

How HIRAKU drives the outputs:
- On is the field's logical maximum, off its minimum.
- Reports are written only when they change, from a thread of their own, so a slow device cannot hold up the game.
- Every bound LED is turned off at start and again at exit.

The launcher lists a device only if its outputs look like lamps: LED usages other than a keyboard's lock LEDs, Button and Ordinal usages, and vendor-defined bits. Vendor byte arrays (data channels such as QMK's raw HID) are left out. **Show every output** in the Assign dialog lifts that filter.

### Linux permissions

Writing to a controller needs read/write access to its `/dev/hidraw*` node, which is root-only by default. If the launcher finds a controller it cannot open, it says so and shows **How to allow access…**. That button gives the commands that install a udev rule for it, keeping the controllers already in the rule file. They work in sh, bash, zsh and fish:

```bash
printf '%s\n' \
  'KERNEL=="hidraw*", ATTRS{idVendor}=="1ccf", ATTRS{idProduct}=="8048", TAG+="uaccess"' \
  | sudo tee /etc/udev/rules.d/70-hiraku-lights.rules >/dev/null
sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=hidraw
```

Then press **Rescan**. If the controller is still refused, replug it. `TAG+="uaccess"` gives the logged-in user access, and only while they are logged in.

## Socket protocol

```bash
build/cmake/hiraku-popn7 --window --lights-socket $XDG_RUNTIME_DIR/hiraku-lights.sock
```

The launcher passes this socket path unless **Publish lamps** is off in Settings. It is a UNIX stream socket that sends lines of text. Clients only read, and any number of them may connect.

On connect, the server sends a header line, then one line per lamp with its current state:

```
hiraku-lights 1 popn7 popn
Button Lamp 1=0
...
Status LED 8=1
```

After that it sends one line per change, as it happens, in the same `<lamp>=<0|1>` form. The header's fields are:
- the protocol version (1);
- the game ID;
- the cabinet family.

Changes are collected once per video frame (60 Hz), as soon as the frame is emulated: before HIRAKU waits for the frame's time, not after (up to 0.9.0 they went out after the wait, most of a frame later on a PC that keeps up). A client that stops reading is dropped. The socket disappears when the game exits.

Watching from a shell:

```bash
socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/hiraku-lights.sock
```

A minimal Python client:

```python
import socket, os
s = socket.socket(socket.AF_UNIX)
s.connect(os.environ["XDG_RUNTIME_DIR"] + "/hiraku-lights.sock")
for line in s.makefile():
    name, _, value = line.strip().partition("=")
    if value: print(name, "on" if value == "1" else "off")
```
