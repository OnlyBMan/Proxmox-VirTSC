# Proxmox VirTSC

Thunderstorm card support for Proxmox VE 9.2 on x86_64. The patched packages
add **Hardware → Add → Thunderstorm Card** to the VM GUI and include the QEMU
devices required by [VirTSC](https://github.com/VirTSC/virtsc).

## Install

The prebuilt packages target these base versions:

| Package | Version |
| --- | --- |
| `pve-qemu-kvm` | `11.0.3-3` |
| `qemu-server` | `9.2.8` |
| `pve-manager` | `9.2.20` |

Check all three with `pveversion -v`, then follow the
[install guide](pve9-release/INSTALL.md). Install the packages on every
cluster node that may run a Thunderstorm VM.

## Use

Use an i440fx VM. In the Thunderstorm card editor, **TS Input** takes an
MPEG-TS file or URL, and **TS Output** writes a TS file or network stream.
These paths and URLs are accessed from the Proxmox host. TS input and output
need `ffmpeg`; output also needs the included
[`is1ts` tool](tools/is1ts/README.md). Card version, timestamps, and raw I/O
are configurable in the editor's Advanced section.

The VM still needs the
[VirTSC guest setup](https://github.com/VirTSC/virtsc/blob/main/docs/GUEST.md).
