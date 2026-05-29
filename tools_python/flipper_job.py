import argparse
import os
from pathlib import Path

import pr


BYTES_PER_FRAME = 20
BITS_PER_FRAME = BYTES_PER_FRAME * 8


def _image_convert(image, color_pass, color_mode):
    pixels = []
    for row in image:
        for rgb in row:
            r, g, b = rgb[0] / 255, rgb[1] / 255, rgb[2] / 255
            is_red = r > 0.5 and g < 0.5 and b < 0.5
            is_yellow = r > 0.5 and g > 0.5 and b < 0.5
            if color_pass:
                pixels.append(0 if (is_red or is_yellow) else 1)
            elif is_red and color_mode:
                pixels.append(1)
            elif is_yellow and color_mode:
                pixels.append(0)
            else:
                luma = 0.21 * r + 0.72 * g + 0.07 * b
                pixels.append(0 if luma < 0.5 else 1)
    return pixels


def _record_run(run_count, compressed):
    bits = []
    while run_count:
        bits.insert(0, run_count & 1)
        run_count >>= 1
    for _ in bits[1:]:
        compressed.append(0)
    if bits:
        compressed.extend(bits)


def _barcode_to_plid(barcode):
    return pr.get_plid(barcode)


def _sanitize_name(value):
    return "".join(ch if ch.isalnum() or ch in ("-", "_", ".") else "_" for ch in value)


def _frame_repeats(frame):
    return frame[-2] + (frame[-1] << 8)


def serialize_job(frames, pp16, output_path):
    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)

    with output.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("Filetype: PrecIR Job\n")
        handle.write("Version: 1\n")
        handle.write(f"Protocol: {'PP16' if pp16 else 'PP4'}\n")
        for frame in frames:
            # The last two bytes added by terminate_frame() store the repeat count and are
            # metadata for the job/app, not payload bytes that should be transmitted.
            handle.write(f"Frame: {_frame_repeats(frame)} {bytes(frame[:-2]).hex().upper()}\n")

    return output


def build_image_job(image_path, barcode, page=1, color=0, pos_x=0, pos_y=0, force_pp4=False):
    from imageio.v2 import imread

    pp16 = 0 if force_pp4 else 1
    image = imread(image_path)
    width = image.shape[1]
    height = image.shape[0]

    pixel_count = width * height
    if pixel_count & 7:
        raise ValueError(
            f"The image pixel count ({pixel_count}) must be a multiple of 8. Adjust its size."
        )

    plid = _barcode_to_plid(barcode)
    pixels = _image_convert(image, 0, color)
    if not pixels:
        raise ValueError("The image must contain at least one pixel.")
    if color:
        pixels += _image_convert(image, 1, color)

    compressed = [pixels[0]]
    run_pixel = pixels[0]
    run_count = 1
    for pixel in pixels[1:]:
        if pixel == run_pixel:
            run_count += 1
        else:
            _record_run(run_count, compressed)
            run_count = 1
            run_pixel = pixel
    if run_count > 1:
        _record_run(run_count, compressed)

    if len(compressed) < len(pixels):
        data = compressed
        compression_type = 2
    else:
        data = pixels
        compression_type = 0

    padding = (BITS_PER_FRAME - (len(data) % BITS_PER_FRAME)) % BITS_PER_FRAME
    data.extend([0] * padding)
    frame_count = len(data) // BITS_PER_FRAME

    frames = [pr.make_ping_frame(plid, pp16, 400)]

    frame = pr.make_mcu_frame(plid, 0x05)
    pr.append_word(frame, len(data) // 8)
    frame.append(0x00)
    frame.append(compression_type)
    frame.append(page)
    pr.append_word(frame, width)
    pr.append_word(frame, height)
    pr.append_word(frame, pos_x)
    pr.append_word(frame, pos_y)
    pr.append_word(frame, 0x0000)
    frame.append(0x88)
    pr.append_word(frame, 0x0000)
    frame.extend([0x00, 0x00, 0x00, 0x00])
    pr.terminate_frame(frame, pp16, 1)
    frames.append(frame)

    bit_index = 0
    for frame_index in range(frame_count):
        frame = pr.make_mcu_frame(plid, 0x20)
        pr.append_word(frame, frame_index)
        for _ in range(BYTES_PER_FRAME):
            value = 0
            for _ in range(8):
                value = (value << 1) | data[bit_index]
                bit_index += 1
            frame.append(value)
        pr.terminate_frame(frame, pp16, 1)
        frames.append(frame)

    frames.append(pr.make_refresh_frame(plid, pp16))
    return frames, pp16


def build_segments_job(barcode, bitmap_hex):
    if len(bitmap_hex) != 46:
        raise ValueError("Segment bitmap must be exactly 46 hex digits.")

    plid = _barcode_to_plid(barcode)
    bitmap = bytearray.fromhex(bitmap_hex)
    payload = [0xBA, 0x00, 0x00, 0x00]
    payload.extend(bitmap)
    segcrc = pr.crc16(bitmap)
    payload.append(segcrc & 255)
    payload.append((segcrc >> 8) & 255)
    payload.extend([0x00, 0x00, 0x09, 0x00, 0x10, 0x00, 0x31])

    frame = pr.make_raw_frame(0x84, plid, payload[0])
    frame.extend(payload[1:])
    pr.terminate_frame(frame, 0, 100)
    return [frame], 0


def build_raw_job(barcode, esl_type, data_hex, count):
    plid = _barcode_to_plid(barcode)
    payload = bytearray.fromhex(data_hex)
    if not payload:
        raise ValueError("Raw payload must contain at least one byte.")

    frame = pr.make_raw_frame(0x85 if esl_type == "DM" else 0x84, plid, payload[0])
    frame.extend(payload[1:])
    pr.terminate_frame(frame, 0, count)
    return [frame], 0


def build_page_dm_job(page, duration):
    payload = [0x06, ((page & 7) << 3) | 1, 0x00, 0x00, 0x00, 0x00]
    if duration == "forever":
        payload[1] |= 0x80
    else:
        payload[4] = (duration >> 8) & 0xFF
        payload[5] = duration & 0xFF

    frame = pr.make_raw_frame(0x85, [0, 0, 0, 0], payload[0])
    frame.extend(payload[1:])
    pr.terminate_frame(frame, 0, 200)
    return [frame], 0


def build_page_seg_job(page, duration):
    durations = {"2s": 1, "15s": 3, "15m": 5, "forever": 0x80}
    payload = [0xAB, ((page & 7) << 3) | durations[duration], 0x00, 0x00]
    frame = pr.make_raw_frame(0x84, [0, 0, 0, 0], payload[0])
    frame.extend(payload[1:])
    pr.terminate_frame(frame, 0, 200)
    return [frame], 0


def default_output_path(prefix, name_hint):
    return os.path.join(
        os.path.dirname(__file__),
        "..",
        "flipperzero",
        "jobs",
        f"{prefix}_{_sanitize_name(name_hint)}.precir",
    )


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="Generate Flipper Zero PrecIR job files from existing PrecIR commands."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    image = subparsers.add_parser("image", help="Create a job file for a dot-matrix image update")
    image.add_argument("image")
    image.add_argument("barcode")
    image.add_argument("-p", "--page", type=int, default=1)
    image.add_argument("-c", "--color", type=int, choices=[0, 1], default=0)
    image.add_argument("--x", type=int, default=0)
    image.add_argument("--y", type=int, default=0)
    image.add_argument("--pp4", action="store_true")
    image.add_argument("-o", "--output")

    segments = subparsers.add_parser("segments", help="Create a job file for a segment ESL update")
    segments.add_argument("barcode")
    segments.add_argument("bitmap")
    segments.add_argument("-o", "--output")

    raw = subparsers.add_parser("raw", help="Create a job file for an arbitrary raw frame")
    raw.add_argument("barcode")
    raw.add_argument("type", choices=["DM", "SEG"])
    raw.add_argument("hex")
    raw.add_argument("count", type=int)
    raw.add_argument("-o", "--output")

    page_dm = subparsers.add_parser("page-dm", help="Create a job file for a dot-matrix page change")
    page_dm.add_argument("page", type=int)
    page_dm.add_argument(
        "--duration",
        choices=["2", "15", "900", "forever"],
        default="2",
        help="Display duration in seconds, or forever",
    )
    page_dm.add_argument("-o", "--output")

    page_seg = subparsers.add_parser("page-seg", help="Create a job file for a segment page change")
    page_seg.add_argument("page", type=int)
    page_seg.add_argument("--duration", choices=["2s", "15s", "15m", "forever"], default="2s")
    page_seg.add_argument("-o", "--output")

    return parser


def main():
    parser = build_arg_parser()
    args = parser.parse_args()

    if args.command == "image":
        frames, pp16 = build_image_job(
            args.image, args.barcode, args.page, args.color, args.x, args.y, args.pp4
        )
        output = args.output or default_output_path("image", Path(args.image).stem)
    elif args.command == "segments":
        frames, pp16 = build_segments_job(args.barcode, args.bitmap)
        output = args.output or default_output_path("segments", args.barcode)
    elif args.command == "raw":
        frames, pp16 = build_raw_job(args.barcode, args.type, args.hex, args.count)
        output = args.output or default_output_path("raw", args.barcode)
    elif args.command == "page-dm":
        duration = "forever" if args.duration == "forever" else int(args.duration)
        frames, pp16 = build_page_dm_job(args.page, duration)
        output = args.output or default_output_path("page_dm", f"page_{args.page}")
    elif args.command == "page-seg":
        frames, pp16 = build_page_seg_job(args.page, args.duration)
        output = args.output or default_output_path("page_seg", f"page_{args.page}")
    else:
        parser.error("Unknown command")

    output_path = serialize_job(frames, pp16, output)
    print(f"Wrote {len(frames)} frame(s) to {output_path}")


if __name__ == "__main__":
    main()
