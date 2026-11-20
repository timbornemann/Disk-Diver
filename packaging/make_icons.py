"""Regenerate the PNG and Windows ICO from the Disk-Diver SVG with resvg-py==0.5.0."""

from pathlib import Path
import struct

from resvg_py import svg_to_bytes


def main():
    assets = Path(__file__).resolve().parents[1] / "assets"
    sizes = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
    # ICO supports lossless PNG frames with full alpha. Render each size from
    # the source curves to keep every frame independent of the other rasters.
    frames = [
        svg_to_bytes(svg_path=str(assets / "disk-diver.svg"), width=size, height=size, skip_system_fonts=True)
        for size in sizes
    ]
    (assets / "disk-diver.png").write_bytes(frames[-1])
    entry = struct.Struct("<BBBBHHII")
    header = struct.pack("<HHH", 0, 1, len(frames))
    offset = len(header) + len(frames) * entry.size
    with (assets / "disk-diver.ico").open("wb") as icon:
        icon.write(header)
        for size, frame in zip(sizes, frames):
            dimension = size if size < 256 else 0
            icon.write(entry.pack(dimension, dimension, 0, 0, 1, 32, len(frame), offset))
            offset += len(frame)
        for frame in frames:
            icon.write(frame)


if __name__ == "__main__":
    main()
