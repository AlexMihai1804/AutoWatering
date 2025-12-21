import sys
import subprocess
import os
import serial.tools.list_ports

# Real bossac path
BOSSAC = r"C:\Users\tapir\AppData\Local\Arduino15\packages\arduino\tools\bossac\1.9.1-arduino2\bossac.exe"

def find_port():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        return None

    def score_port(p):
        desc = (p.description or "").lower()
        manu = (getattr(p, "manufacturer", "") or "").lower()
        hwid = (p.hwid or "").lower()

        # Filter obvious non-targets.
        if "bluetooth" in desc or "bluetooth" in manu:
            return -10

        score = 0
        # Prefer likely bootloader/programming ports.
        for token, weight in [
            ("bossa", 50),
            ("boot", 30),
            ("arduino", 25),
            ("nano", 20),
            ("samd", 15),
            ("mbed", 10),
            ("cdc", 5),
        ]:
            if token in desc or token in manu or token in hwid:
                score += weight

        # Prefer USB VID/PID based devices over generic COM (best effort).
        if "vid" in hwid and "pid" in hwid:
            score += 3
        return score

    # Pick the highest scoring port; fall back to first non-bluetooth.
    ranked = sorted(ports, key=score_port, reverse=True)
    for p in ranked:
        if score_port(p) >= 0:
            return p.device
    return None


def print_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports detected.")
        return
    print("Available serial ports:")
    for p in ports:
        desc = p.description or ""
        manu = getattr(p, "manufacturer", "") or ""
        hwid = p.hwid or ""
        print(f"- {p.device}: {desc} | {manu} | {hwid}")

def main():
    args = sys.argv[1:]
    new_args = []
    skip = False
    
    for i, arg in enumerate(args):
        if skip:
            skip = False
            continue
            
        if arg == '-p' or arg == '--port':
            # Skip the flag and the next argument (the port value)
            skip = True
            continue
            
        if arg.startswith('-p') or arg.startswith('--port='):
            # Skip combined flag+value
            continue
            
        new_args.append(arg)

    # Prefer explicit port override when debugging.
    port = os.environ.get("BOSSAC_PORT")
    if port:
        print(f"Using BOSSAC_PORT override: {port}")
    else:
        port = find_port()

    if port:
        print(f"Auto-detected port: {port}")
        new_args.extend(["-p", port])
    else:
        print("No suitable port found, letting bossac try auto-detection...")

    # Run the real bossac with filtered arguments
    try:
        subprocess.check_call([BOSSAC] + new_args)
    except subprocess.CalledProcessError as e:
        print("bossac failed.")
        print_ports()
        print("Hint: If you see 'Device unsupported', put the board in bootloader mode and retry.")
        sys.exit(e.returncode)
    except Exception as e:
        print(f"Error running bossac wrapper: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
