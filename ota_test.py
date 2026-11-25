import argparse
import subprocess
import time
import zlib
from pathlib import Path


def uds(data, expected=None):
    command = [
        "./ecu_boot", "can0", "req",
        *("{:02X}".format(value) for value in data)
    ]

    result = subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        universal_newlines=True,
        timeout=310
    )

    response = bytes.fromhex(result.stdout.strip())

    if expected is not None:
        if response != bytes(expected):
            raise RuntimeError(
                "Request {}: expected {}, got {}".format(
                    bytes(data).hex(),
                    bytes(expected).hex(),
                    response.hex()
                )
            )
    elif response[:1] == b"\x7f":
        raise RuntimeError("UDS rejected: " + response.hex())

    return response

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("--bad-crc", action="store_true")
    parser.add_argument("--repeat", action="store_true")
    parser.add_argument("--stop-after", type=int, default=-1)
    args = parser.parse_args()

    image = Path(args.image).read_bytes()

    if not 0x20C <= len(image) <= 448 * 1024:
        raise RuntimeError("Unexpected App image size")

    new_version = image[0x208:0x20B]
    old_version = uds([0x22, 0xF1, 0x80])[3:]
    block_count = (len(image) + 253) // 254
    crc = zlib.crc32(image) & 0xFFFFFFFF

    print("Old version:", ".".join(map(str, old_version)), flush=True)
    print("New version:", ".".join(map(str, new_version)), flush=True)
    print(
        "Size={}, blocks={}, CRC32={:08X}".format(
            len(image), block_count, crc
        ),
        flush=True
    )

    uds([0x10, 0x02], [0x50, 0x02])
    uds([0x22, 0xF1, 0xA2], [0x62, 0xF1, 0xA2, 0x01])

    download = (
        b"\x34\x00\x44"
        + (0x08010000).to_bytes(4, "big")
        + len(image).to_bytes(4, "big")
    )

    uds(download, [0x74, 0x20, 0x01, 0x00])
    print("A area ready", flush=True)

    if args.stop_after == 0:
        print("Stopped before first data block", flush=True)
        return

    for number, offset in enumerate(range(0, len(image), 254), 1):
        block = number & 0xFF
        request = bytes([0x36, block]) + image[offset:offset + 254]

        uds(request, [0x76, block])

        if args.repeat and number == 1:
            uds(request, [0x7F, 0x36, 0x73])
            print("Repeated block rejected correctly", flush=True)

        if number % 32 == 0 or number == block_count:
            print(
                "Downloaded {}/{} blocks".format(number, block_count),
                flush=True
            )

        if number == args.stop_after:
            print("Stopped after block {}".format(number), flush=True)
            return

    uds([0x37], [0x77])
    print("Transfer complete; starting verification/install", flush=True)

    sent_crc = crc ^ 1 if args.bad_crc else crc
    verify = b"\x31\x01\xFF\x00" + sent_crc.to_bytes(4, "big")

    if args.bad_crc:
        uds(verify, [0x7F, 0x31, 0x72])
        time.sleep(2)
        uds([0x22, 0xF1, 0xA2], [0x62, 0xF1, 0xA2, 0x00])
        uds([0x22, 0xF1, 0x80], b"\x62\xF1\x80" + old_version)
        print("Bad CRC rejected; old App running", flush=True)
        return

    uds(verify, [0x71, 0x01, 0xFF, 0x00])
    uds([0x11, 0x01], [0x51, 0x01])

    time.sleep(2)

    uds([0x22, 0xF1, 0xA2], [0x62, 0xF1, 0xA2, 0x00])
    uds([0x22, 0xF1, 0x80], b"\x62\xF1\x80" + new_version)

    print("Upgrade complete; new App running", flush=True)


if __name__ == "__main__":
    main()