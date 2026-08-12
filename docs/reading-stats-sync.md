---
title: Reading Stats Sync
nav_order: 6
---

# Reading Stats Sync

inkMOD can sync all-time reading stats between nearby readers running inkMOD from **File Transfer > Nearby Stats Sync**. The sync is direct reader-to-reader; it does not use a server, account, or one "main" reader. This only works with inkMOD reading stats. This will *NOT* work with any other firmware's reading stats.

## What Gets Synced

Nearby Stats Sync shares only each reader's all-time counters:

- total reading sessions
- total reading time
- total pages turned
- completed books

It does not sync per-book reading position, bookmarks, recent books, files, KOReader progress, settings, WiFi passwords, or OPDS servers.

Each device owns one record, and the Reading Stats screen sums all the records it can read. The device does not merge another reader's totals into its local `global_stats.bin`.

## How Nearby Sync Works

1. Open **File Transfer > Nearby Stats Sync** on both readers.
2. Press **Sync** on one reader only.
3. The readers find each other over ESP-NOW and exchange their local
   `/.inkmod/global_stats.bin` payloads.
4. Each reader saves the other reader's payload under
   `/.inkmod/synced_stats/device_<mac>.bin`.
5. Future Reading Stats views show this reader's local stats plus the other
   valid files in `/.inkmod/synced_stats/`.

The sync screen creates `/.inkmod/synced_stats/` the first time you use it. That folder stores snapshots received from other readers. This reader's own all-time totals stay in `/.inkmod/global_stats.bin`; inkMOD does not write this reader's own `device_<mac>.bin` file into `synced_stats/`.

## SD Card Folder Structure

The reading-stats files live under `.inkmod` on the SD card:

```text
.inkmod/
├── global_stats.bin
├── global_stats.bin.bak
└── synced_stats/
    ├── device_aabbccddeeff.bin
    ├── device_112233445566.bin
    └── manual-copy.bin
```

`global_stats.bin` is this reader's local all-time stats. The `.bak` file is a
backup used if the main file is corrupt.

`synced_stats/` contains one contribution file per other synced reader. Each file is a snapshot from the last time this reader received that device's stats; it is not a live copy of that other reader's current `global_stats.bin`. Files created by nearby sync use the peer reader's hardware MAC address in the file
name: `device_<mac>.bin`, with no colons or dashes.

When displaying aggregated stats, inkMOD reads every non-folder file in `synced_stats/` that has a valid inkMOD stats payload. The file name does not have to match `device_<mac>.bin` for manual imports. If a manually copied file matches this reader's own `device_<local-mac>.bin`, inkMOD skips it so local stats are not counted twice.

## Manually Removing Synced Stats

Back up the SD card first if the stats matter to you.

To remove one old reader from the aggregate total, delete that reader's file from `/.inkmod/synced_stats/` on every device that has it. For example:

```text
/.inkmod/synced_stats/device_aabbccddeeff.bin
```

To stop using synced aggregate totals on a reader, delete the whole `/.inkmod/synced_stats/` folder from that reader. As long as the folder is gone, local stats saves will not recreate it. Opening Nearby Stats Sync again will recreate the folder.

To reset only this reader's local all-time totals, delete both:

```text
/.inkmod/global_stats.bin
/.inkmod/global_stats.bin.bak
```

Do not delete `/.inkmod/epub_<hash>/stats.bin` unless you also want to remove the per-book stats for that book.

If a sync was interrupted, a temporary `*.part` file may be left in `synced_stats/`. inkMOD ignores invalid stats files while aggregating, so it is safe to delete leftover `.part` files.

## Manual Copying Between Devices

You can manually copy a valid stats file into another reader's `/.inkmod/synced_stats/` folder. A descriptive file name such as `old_reader.bin` can work because inkMOD validates the file contents, not the name.

Use normal `device_<mac>.bin` names when possible. Do not copy this reader's own local `global_stats.bin` into `synced_stats/` under a different name; inkMOD will treat it like another reader and the all-time total will be too high.
