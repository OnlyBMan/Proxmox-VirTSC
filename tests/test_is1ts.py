#!/usr/bin/env python3
"""End-to-end check of the raw Thunderstorm stream to MPEG-TS bridge."""

import json
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools" / "is1ts" / "is1ts.c"
WIDTH, HEIGHT = 720, 480
MAGIC = 0x32535449
PAIRS_PER_FRAME = 1602


def write_card_stream(path: Path) -> None:
    picture = bytes((0, 0, 200, 255)) * (WIDTH * HEIGHT)
    audio = struct.pack("<ii", 0, 0) * PAIRS_PER_FRAME
    with path.open("wb") as stream:
        for number in range(12):
            stream.write(
                struct.pack(
                    "<8I", MAGIC, WIDTH, HEIGHT, number, PAIRS_PER_FRAME, 48000, 0, 0
                )
            )
            stream.write(picture)
            stream.write(audio)


@unittest.skipUnless(
    all(shutil.which(executable) for executable in ("cc", "ffmpeg", "ffprobe")),
    "cc, ffmpeg, and ffprobe are required",
)
class TransportStreamTest(unittest.TestCase):
    def test_fill_and_audio(self) -> None:
        self.check_stream(
            ["-c:v", "mpeg2video", "-b:v", "3M", "-c:a", "mp2", "-b:a", "192k"],
            ["video", "audio"],
        )

    def test_fill_key_and_audio(self) -> None:
        self.check_stream(
            ["-K", "-c:v", "mpeg2video", "-b:v", "3M", "-c:a", "mp2", "-b:a", "192k"],
            ["video", "audio", "video"],
        )

    def check_stream(self, options: list[str], expected_types: list[str]) -> None:
        with tempfile.TemporaryDirectory(prefix="is1ts-test-") as directory:
            work = Path(directory)
            binary = work / "is1ts"
            source = work / "card.raw"
            output = work / "result.ts"
            write_card_stream(source)
            subprocess.run(
                ["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread", "-o", str(binary),
                 str(SOURCE), "-lm"],
                check=True,
                timeout=30,
            )
            flags = options[:1] if options and options[0] == "-K" else []
            encoders = options[len(flags):]
            subprocess.run(
                [str(binary), *flags, "-i", str(source), str(output), *encoders],
                check=True,
                capture_output=True,
                text=True,
                timeout=30,
            )
            probe = subprocess.run(
                ["ffprobe", "-v", "error", "-show_entries", "stream=codec_type,width,height,sample_rate",
                 "-of", "json", str(output)],
                check=True,
                capture_output=True,
                text=True,
                timeout=30,
            )
            streams = json.loads(probe.stdout)["streams"]
            self.assertEqual([stream["codec_type"] for stream in streams], expected_types)
            for stream in streams:
                if stream["codec_type"] == "video":
                    self.assertEqual((stream["width"], stream["height"]), (WIDTH, HEIGHT))
                else:
                    self.assertEqual(stream["sample_rate"], "48000")


if __name__ == "__main__":
    unittest.main()
