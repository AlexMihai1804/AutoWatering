import sys
import subprocess
import os
import serial.tools.list_ports

# Real bossac path
BOSSAC = r"C:\Users\tapir\AppData\Local\Arduino15\packages\arduino\tools\bossac\1.9.1-arduino2\bossac.exe"

def find_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        # Ignore Bluetooth ports
        if "Bluetooth" in p.description:
            continue
        # Return the first non-Bluetooth port found
        return p.device
    return None

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

    # Auto-detect port
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
        sys.exit(e.returncode)
    except Exception as e:
        print(f"Error running bossac wrapper: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
