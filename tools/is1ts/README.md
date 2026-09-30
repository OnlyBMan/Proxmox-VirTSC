# MPEG-TS output from a Thunderstorm VM

`is1ts` converts the Thunderstorm card's raw output to MPEG-TS through
FFmpeg. It can write a file or send the stream to a URL.

The Proxmox card editor's **TS Output** field starts `is1ts` automatically
with the VM after the utility is installed on the host. This guide also
covers the manual **Advanced → Output FIFO** path for custom encoder options.

## Build on the Proxmox host

```sh
apt install build-essential ffmpeg
cd tools/is1ts
make install
```

This installs `/usr/local/bin/is1ts`.

## Connect a VM through TS Output

Enter an absolute host path such as `/var/lib/virtsc/vm107.ts` or a URL such
as `udp://239.1.1.1:1234?pkt_size=1316` in **TS Output**, then fully stop
and start the VM. For a file, create its parent directory and choose a new
filename for each recording.

## Manual raw FIFO setup

For VM 107, create a FIFO on the host and set the card's **Advanced → Output
FIFO** field to its full path:

```sh
install -d -m 0755 /var/lib/virtsc
mkfifo -m 0600 /var/lib/virtsc/vm107-output
```

Set `/var/lib/virtsc/vm107-output` in the GUI. If adding a new card from the
CLI, the equivalent setting is:

```sh
qm set 107 --thunderstorm '1,output=/var/lib/virtsc/vm107-output'
```

For an existing card, edit its Output FIFO in the GUI so its other settings
are retained. Fully stop and start the VM after changing card options.

Record a TS file from a separate host shell while the VM runs:

```sh
is1ts -i /var/lib/virtsc/vm107-output /var/lib/virtsc/vm107.ts
```

The default encoding is H.264 video and AAC audio. `-r` waits for the VM to
return after a restart while keeping the same output session:

```sh
is1ts -r -i /var/lib/virtsc/vm107-output \
  'udp://239.1.1.1:1234?pkt_size=1316'
```

For MPEG-2 video and MP2 audio:

```sh
is1ts -i /var/lib/virtsc/vm107-output /var/lib/virtsc/vm107.ts \
  -c:v mpeg2video -b:v 8M -c:a mp2 -b:a 256k
```

`-k` encodes only the alpha key; `-K` puts fill and key into separate video
streams in one TS. `-a -6` lowers programme audio by 6 dB before encoding.

## Check the result

```sh
ffprobe -v error -show_entries stream=index,codec_name,codec_type \
  -of compact /var/lib/virtsc/vm107.ts
```
