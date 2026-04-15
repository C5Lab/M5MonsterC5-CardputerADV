import time
import sys
import subprocess
import os
import argparse

def ensure_packages():
    missing = []
    try:
        import serial.tools.list_ports  # noqa
    except ImportError:
        missing.append("pyserial")
    try:
        import esptool  # noqa
    except ImportError:
        missing.append("esptool")
    if missing:
        print("\033[93mInstalling missing packages: " + ", ".join(missing) + "\033[0m")
        subprocess.check_call([sys.executable, "-m", "pip", "install"] + missing)
        os.execv(sys.executable, [sys.executable] + sys.argv)

ensure_packages()
import serial
import serial.tools.list_ports

# Colors
RED = "\033[91m"; GREEN = "\033[92m"; YELLOW = "\033[93m"; CYAN = "\033[96m"; RESET = "\033[0m"

VERSION = "v03"
DEFAULT_BAUD = 460800  # Original default; can be overridden via CLI
DEFAULT_BOARD = "adv"

# >>> DO NOT CHANGE: keep your original offsets <<<
OFFSETS = {
    "bootloader": "0x0",       # as requested
    "partition-table": "0x8000",
    "app": "0x10000",
}

def detect_board_by_files():
    if (os.path.exists("bootloader-k132.bin") and os.path.exists("partition-table-k132.bin")) or \
       os.path.exists("M5MonsterC5-CardputerADV-k132.bin"):
        return "k132"
    if (os.path.exists("bootloader-adv.bin") and os.path.exists("partition-table-adv.bin")) or \
       os.path.exists("M5MonsterC5-CardputerADV-adv.bin"):
        return "adv"
    return None

def detect_board_by_path():
    cwd = os.getcwd().lower()
    if cwd.endswith(os.sep + "k132") or (os.sep + "k132" + os.sep) in cwd:
        return "k132"
    if cwd.endswith(os.sep + "adv") or (os.sep + "adv" + os.sep) in cwd:
        return "adv"
    return None

def choose_board_interactive():
    print(f"{YELLOW}Select target board:{RESET}")
    print("  1) ADV")
    print("  2) K132")
    choice = input("> ").strip()
    if choice == "2":
        return "k132"
    return "adv"

def board_files(board):
    suffix = board.lower()
    return {
        "bootloader": f"bootloader-{suffix}.bin",
        "partition-table": f"partition-table-{suffix}.bin",
        "app": f"M5MonsterC5-CardputerADV-{suffix}.bin",
    }

def check_files(files):
    missing = [f for f in files.values() if not os.path.exists(f)]
    if missing:
        print(f"{RED}Missing files: {', '.join(missing)}{RESET}")
        sys.exit(1)
    print(f"{GREEN}All required files found.{RESET}")

def list_ports():
    return set(p.device for p in serial.tools.list_ports.comports())

def wait_for_new_port(before, timeout=20.0):
    print(f"{CYAN}Hold BOOT and connect the board to enter ROM mode.{RESET}")
    spinner = ['|','/','-','\\']
    print(f"{YELLOW}Waiting for new serial port...{RESET}")
    t0 = time.time()
    i = 0
    while time.time() - t0 < timeout:
        after = list_ports()
        new_ports = after - before
        sys.stdout.write(f"\r{spinner[i % len(spinner)]} "); sys.stdout.flush()
        i += 1
        if new_ports:
            sys.stdout.write("\r"); sys.stdout.flush()
            return new_ports.pop()
        time.sleep(0.15)
    print(f"\n{RED}No new serial port detected.{RESET}")
    sys.exit(1)

def erase_all(port, baud=DEFAULT_BAUD):
    cmd = [sys.executable, "-m", "esptool", "-p", port, "-b", str(baud),
           "--before", "default-reset", "--after", "no_reset", "--chip", "esp32c5",
           "erase_flash"]
    print(f"{CYAN}Erasing full flash:{RESET} {' '.join(cmd)}")
    res = subprocess.run(cmd)
    if res.returncode != 0:
        print(f"{RED}Erase failed with code {res.returncode}.{RESET}")
        sys.exit(res.returncode)

def do_flash(port, files, baud=DEFAULT_BAUD, flash_mode="dio", flash_freq="80m"):
    cmd = [
        sys.executable, "-m", "esptool",
        "-p", port,
        "-b", str(baud),
        "--before", "default-reset",
        "--after", "watchdog-reset",            # we'll do a precise reset pattern ourselves
        "--chip", "esp32s3",
        "write-flash",
        "--flash-mode", flash_mode,       # default "dio"
        "--flash-freq", flash_freq,       # default "80m"
        "--flash-size", "detect",
        OFFSETS["bootloader"], files["bootloader"],
        OFFSETS["partition-table"], files["partition-table"],
        OFFSETS["app"], files["app"],
    ]
    print(f"{CYAN}Flashing command:{RESET} {' '.join(cmd)}")
    res = subprocess.run(cmd)
    if res.returncode != 0:
        print(f"{RED}Flash failed with code {res.returncode}.{RESET}")
        sys.exit(res.returncode)

def pulse(ser, dtr=None, rts=None, delay=0.06):
    if dtr is not None:
        ser.dtr = dtr
    if rts is not None:
        ser.rts = rts
    time.sleep(delay)

def reset_to_app(port):
    """
    Typical ESP auto-reset wiring:
      RTS -> EN (inverted)
      DTR -> GPIO0 (inverted)

    To boot the *application*:
      - DTR=False  (GPIO0 HIGH, i.e., not in ROM)
      - pulse EN low via RTS=True then RTS=False
    """
    print(f"{YELLOW}Issuing post-flash reset (RTS/DTR) to run app...{RESET}")
    try:
        with serial.Serial(port, 115200, timeout=0.1) as ser:
            # Make sure BOOT is released
            pulse(ser, dtr=False, rts=None)
            # Short EN reset
            pulse(ser, rts=True)
            pulse(ser, rts=False)
        print(f"{GREEN}Reset sent. If not Press the board's RESET button manually.{RESET}")
        
    except Exception as e:
        print(f"{RED}RTS/DTR reset failed: {e}{RESET}")
        print(f"{YELLOW}Press the board's RESET button manually.{RESET}")

def monitor(port, baud=DEFAULT_BAUD):
    print(f"{CYAN}Opening serial monitor on {port} @ {baud} (Ctrl+C to exit)...{RESET}")
    try:
        # A brief delay to let the port re-enumerate after reset
        time.sleep(0.3)
        with serial.Serial(port, baud, timeout=0.2) as ser:
            while True:
                try:
                    data = ser.read(1024)
                    if data:
                        sys.stdout.write(data.decode(errors="replace"))
                        sys.stdout.flush()
                except KeyboardInterrupt:
                    break
    except Exception as e:
        print(f"{RED}Monitor failed: {e}{RESET}")

def main():
    parser = argparse.ArgumentParser(description="ESP32-C5 flasher with robust reboot handling")
    parser.add_argument("--version", action="version", version=f"%(prog)s {VERSION}")
    parser.add_argument("--port", help="Known serial port (e.g., COM10 or /dev/ttyACM0)")
    parser.add_argument("baud", nargs="?", type=int, default=DEFAULT_BAUD,
                        help=f"Optional baud rate (default: {DEFAULT_BAUD})")
    parser.add_argument("--board", default=None, choices=["adv", "k132"],
                        help="Target board (adv or k132). Default: auto-detect by folder/files.")
    parser.add_argument("--monitor", action="store_true", help="Open serial monitor after flashing")
    parser.add_argument("--erase", action="store_true", help="Full erase before flashing (fixes stale NVS/partitions)")
    parser.add_argument("--flash-mode", default="dio", choices=["dio", "qio", "dout", "qout"],
                        help="Flash mode (default: dio)")
    parser.add_argument("--flash-freq", default="80m", choices=["80m", "60m", "40m", "26m", "20m"],
                        help="Flash frequency (default: 80m). If you see boot loops, try 40m.")
    args = parser.parse_args()

    board = args.board or detect_board_by_files() or detect_board_by_path()
    if not board:
        board = choose_board_interactive()
    files = board_files(board)
    check_files(files)

    print(f"{CYAN}ESP32-S3 flasher version: {VERSION}{RESET}")
    print(f"{CYAN}Using baud rate: {args.baud}{RESET}")

    if args.port:
        port = args.port
    else:
        before = list_ports()
        port = wait_for_new_port(before)

    print(f"{GREEN}Detected serial port: {port}{RESET}")
    print(f"{YELLOW}Tip: release the BOOT button before programming finishes.{RESET}")

    if args.erase:
        erase_all(port, args.baud)

    do_flash(port, files, baud=args.baud, flash_mode=args.flash_mode, flash_freq=args.flash_freq)

    reset_to_app(port)

    if args.monitor:
        monitor(port, args.baud)

if __name__ == "__main__":
    main()
