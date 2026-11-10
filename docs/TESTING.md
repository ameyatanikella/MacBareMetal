# Testing on the Mac

**Nothing in this procedure has been run on an M5 Pro.** The boot-object steps come from Asahi's m1n1 hypervisor guide, which was written for M1 to M3. Treat every step as a first attempt and stop if something does not match.

The build itself is verified. What is unverified is installing it and what it does once booted.

## What can and cannot go wrong

- **Your main macOS volume is not modified.** The boot object goes on a second macOS volume. `install.sh` refuses the volume macOS was running from when you staged the image.
- **The second volume shares your APFS container.** Asahi's guide does this. Asahi's security page describes boot policy as "per container" and does not say how a lowered policy on one volume interacts with another volume in the same container. This is an open question; a separate container would avoid it but means resizing your main one.
- **A bad boot object is recoverable.** Hold the power button at start-up to reach Startup Options and pick your main volume. The worst documented case, corrupted firmware, needs a DFU restore from another Mac; nothing here writes firmware.
- **Each rung up the ladder adds risk** (see the last section). Rung 1 only reads.

## Before you start

1. Back up. Time Machine or a clone, completed and checked.
2. Connect AC power.
3. Know your main volume's name: `diskutil info / | grep "Volume Name"`.
4. Have a phone ready to film the screen. That recording is the log.
5. Keep a copy of this file on the phone; the Mac will be unavailable during the test.

## One-time setup

### 1. Record the device tree from macOS

```bash
ioreg -p IODeviceTree -l -w 0 > ~/m5-devicetree.txt
```

This is the reference the on-screen addresses are checked against later.

### 2. Create the test volume

Find the APFS container that holds your main volume (it was `disk3` on 9 November 2026; check):

```bash
diskutil list
```

Add a volume to it. Replace `disk3` if yours differs:

```bash
diskutil apfs addVolume disk3 APFS m5test
```

Do not give it a role. APFS volumes share free space, so this reserves nothing.

### 3. Install macOS on it

```bash
softwareupdate --fetch-full-installer
```

Run the installer from `/Applications`, choose **Show All Disks**, and select `m5test`. Let it finish and create a user. The Mac now boots `m5test` by default; that is expected for the next step.

Which macOS version to install on M5 Pro is not something Asahi's guide covers. Installing the same version as your main system keeps the firmware unchanged.

### 4. Lower security for the test volume only

1. Confirm `m5test` is the startup disk (System Settings, General, Startup Disk).
2. Shut down. Hold the power button until "Loading startup options" appears.
3. Choose **Options**, pick the `m5test` user, and open **Utilities, Terminal**. This environment is 1TR.
4. Run:

```bash
bputil -nkcas
```

Select the `m5test` volume when asked. Then, because `bputil` resets SIP:

```bash
csrutil disable
```

If either command reports that the OS is "not paired", you are in the recovery of a different volume. Reboot, make `m5test` the startup disk and try again.

## Each test run

### 5. Build and stage (in your main macOS)

```bash
make image
```

Note the build id it prints. Mount the test volume's data volume in Finder, then (the mount name may differ; check `ls /Volumes`):

```bash
make stage DEST="/Volumes/m5test - Data"
```

### 6. Install the boot object (in 1TR)

Enter 1TR as in step 4 and run, adjusting the path to where the data volume is mounted there:

```bash
sh "/Volumes/m5test - Data/m5rt/install.sh" /Volumes/m5test
```

The script prints the image, build id and target, refuses your main volume, and asks you to type the target volume's name. It then runs:

```
kmutil configure-boot -c m5rt.bin --raw --entry-point 2048 --lowest-virtual-address 0 -v /Volumes/m5test
```

### 7. Boot it

Start filming, then restart. With `m5test` as the startup disk the Mac boots the runtime directly.

Expected sequence, about two and a half minutes with default timings:

1. Apple logo, then m1n1's logo and a burst of m1n1 text on the left.
2. The screen clears to `m5rt 0.1  build <id>` and eight tiles appear on the right.
3. Each stage turns yellow, prints a page, turns green, orange, red or blue, and waits 12 seconds.
4. A summary page prints `MILESTONE 1: REACHED` or `NOT REACHED`.
5. After 45 seconds the Mac reboots.

### 8. Get back to macOS

The Mac will boot the runtime again unless you change the startup disk. At the reboot, hold the power button for Startup Options, select your main volume **while holding Option**, and click **Always Use**.

To run a test again without changing the default, pick `m5test` once from Startup Options instead. Whether a plain selection there is one-time on this machine is unverified; if the Mac keeps booting the runtime, repeat the Option-select.

## Reading failures

| What you see | What it means | What to do |
|---|---|---|
| Apple logo, then macOS or a recovery prompt | The boot object was not accepted or not installed | Recheck step 4 (`bputil`, `csrutil`), then step 6 |
| Black screen, or Apple logo that never changes | m1n1 itself did not reach its console on this firmware | Hold power 10 s. This is upstream m1n1 on `mBoot-18000.161.10`, not the runtime. Report it to Asahi |
| m1n1 logo and text, never `m5rt` | m1n1 stopped in its own initialisation | Photograph the last lines. Hold power 10 s |
| `m5rt` page with a different build id | The old image is still installed | Repeat steps 5 and 6 |
| A tile stays yellow | That stage hung | The last console line names the step. Hold power 10 s |
| A tile turns red with a register dump | An exception outside a guard | The screen holds 90 s. Photograph `ELR`, `ESR` and `FAR` |
| `ident` is orange | The chip id is not `0x6050` | Stop. This build was written for T6050 only |
| `devtree` or `gpu` is orange | Expected device nodes are missing | Compare with `~/m5-devicetree.txt` |
| Countdown ends and the screen stays | The watchdog reboot did not work | Hold power 10 s. Do not enable rung 2 |
| Everything green but text overlaps tiles | Display size the simulator did not cover | Note the resolution printed by `ident` |

## Milestone 1 is reached when

1. The screen shows `m5rt 0.1` with the build id you just built.
2. `ident` reports chip-id `0x6050` and is green.
3. `cpu`, `exceptions`, `memory` and `devtree` are green or orange, and you understand each orange one.
4. The summary prints `MILESTONE 1: REACHED`.
5. The Mac reboots by itself, and macOS starts normally after you select it.
6. A second run gives the same tiles and the same addresses.
7. The addresses on the `devtree` and `gpu` pages agree with `~/m5-devicetree.txt`.

## Risk ladder

Go up one rung at a time, and only after the rung below has passed twice.

| Rung | Build command | What it adds | Why it waits |
|---|---|---|---|
| 1 | `make image` | Reads RAM, CPU registers and the device tree | Starting point |
| 2 | `make image RT_WDT_GUARD=1` | Arms the watchdog so a hang reboots after a known time | Needs rung 1's final reboot to have worked, since it uses the same registers |
| 3 | add `RT_NVME_PROBE=1` | m1n1's NVMe driver, read commands only | m1n1's NVMe support is unreported on T6050 |
| 4 | add `RT_GPU_PROBE=1` | One guarded read at the start of each GPU register range | A powered-down block may hang instead of faulting |
| 5 | add `RT_GPU_POWER=1` | Switches the GPU power gates on and off through m1n1 | First write to GPU-related hardware |

If rung 3 passes, m1n1 can load a build from a FAT32 partition at boot ("chainloading"), and steps 6 and 8 stop being needed for each build. Setting that up means adding a small partition and is not done yet.

## Undo

1. Make your main volume the startup disk (step 8).
2. From your main macOS, delete the test volume group (the installer made a system volume and a data volume). List the groups and note the one named `m5test`:

```bash
diskutil apfs listVolumeGroups
```

Then delete it by its UUID. Check the UUID twice; this erases that group:

```bash
diskutil apfs deleteVolumeGroup <UUID of the m5test group>
```

Deleting the group removes its boot policy with it. Your main volume's security settings were never changed.
