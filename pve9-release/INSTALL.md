# Install Proxmox VirTSC

These packages add the Thunderstorm card to Proxmox VE 9.2 on x86_64. They
replace three Proxmox packages, so check all three installed versions before
using the binaries in this directory:

| Package | Required base version | Bundle file |
| --- | --- | --- |
| `pve-qemu-kvm` | `11.0.3-3` | `pve-qemu-kvm_11.0.3-3+virtsc3_amd64.deb` |
| `qemu-server` | `9.2.8` | `qemu-server_9.2.8+virtsc4_amd64.deb` |
| `pve-manager` | `9.2.20` | `pve-manager_9.2.20+virtsc2_all.deb` |

```sh
dpkg --print-architecture
pveversion -v | grep -E '^(pve-qemu-kvm|qemu-server|pve-manager):'
```

For other package versions, rebuild from the source patches. In a cluster,
install the same packages on every node that may run the VM. Stop affected
VMs before installing.

## Install the packages

Copy the contents of `pve9-release` to the host. From your local clone:

```sh
ssh root@HOST 'mkdir -p /root/virtsc-pve9'
scp pve9-release/* root@HOST:/root/virtsc-pve9/
```

On the Proxmox host, save the original packages for rollback, verify the
bundle, and preview the install:

```sh
cd /root/virtsc-pve9
mkdir -p rollback
cd rollback
apt download pve-qemu-kvm=11.0.3-3 qemu-server=9.2.8 pve-manager=9.2.20
cd ..
sha256sum -c SHA256SUMS
apt -s install ./pve-qemu-kvm_11.0.3-3+virtsc3_amd64.deb \
  ./qemu-server_9.2.8+virtsc4_amd64.deb \
  ./pve-manager_9.2.20+virtsc2_all.deb
```

If the preview would remove `proxmox-ve` or unrelated packages, stop here.
Keep the original packages for rollback, then install:

```sh
apt install ./pve-qemu-kvm_11.0.3-3+virtsc3_amd64.deb \
  ./qemu-server_9.2.8+virtsc4_amd64.deb \
  ./pve-manager_9.2.20+virtsc2_all.deb
systemctl restart pvedaemon pveproxy
```

Refresh the browser tab.

## Add the card

Use an **i440fx** VM; q35 is not supported. Stop it, then select
**Hardware → Add → Thunderstorm Card**. The **Card Version** field accepts a
32-bit hex value such as `0x011a0012`. Leave it blank for the default; a
mismatch with guest firmware may trigger a reflash and reboot.

Install the [VirTSC guest software](https://github.com/VirTSC/virtsc/blob/main/docs/GUEST.md)
inside the VM. In noVNC, **Ctrl+Alt+2** shows the Thunderstorm programme
screen and **Ctrl+Alt+1** returns to VGA. Add a SPICE Audio Device if you want
programme audio in the console.

## MPEG-TS input and output

**TS Input** accepts a `.ts` file on the Proxmox host or an FFmpeg-readable
URL. **TS Output** accepts a host output path or a network URL. Turn off
**TS Input Audio** if the source has no audio stream. The paths are on the
host, not inside the VM, and these fields are available only to `root@pam`.

Install the host tools on every node that may run the VM:

```sh
apt install build-essential ffmpeg
cd /root/virtsc-pve9
tar -xzf is1ts-source.tar.gz
make -C tools/is1ts install
```

Create the output directory before starting the VM and use a new filename
for each recording. Fully stop and start the VM after changing TS settings.

The Advanced raw FIFO fields are for manual pipelines; do not combine a TS
field with the corresponding raw FIFO field. The
[`is1ts` guide](../tools/is1ts/README.md) covers manual output and encoder
options.

## Roll back

Remove the Thunderstorm card from affected VMs and stop them. Then reinstall
the original packages saved above:

```sh
cd /root/virtsc-pve9
apt install --allow-downgrades ./rollback/pve-qemu-kvm_11.0.3-3_amd64.deb \
  ./rollback/qemu-server_9.2.8_amd64.deb \
  ./rollback/pve-manager_9.2.20_all.deb
systemctl restart pvedaemon pveproxy
```
