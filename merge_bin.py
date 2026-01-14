Import("env")
import os

def merge_binaries(source, target, env, **kwargs):
    build_dir = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    output_name = os.path.join(build_dir, "full_install_v1.0.1.bin")
    
    # Path to esptool executable (PlatformIO internal)
    # This is more reliable than 'python -m esptool'
    esptool_path = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy") or "", "esptool.py")

    print("\n🚀 Starting dynamic binary merging...")

    bootloader_bin = os.path.join(build_dir, "bootloader.bin")
    partitions_bin = os.path.join(build_dir, "partitions.bin")
    firmware_bin = os.path.join(build_dir, "firmware.bin")
    otadata_bin = os.path.join(build_dir, "ota_data_initial.bin")
    fs_bin = os.path.join(build_dir, "littlefs.bin")

    flash_images = [
        "0x0000", bootloader_bin,
        "0x8000", partitions_bin,
        "0x10000", firmware_bin
    ]

    if os.path.exists(otadata_bin):
        flash_images.extend(["0xe000", otadata_bin])

    fs_offset = None
    partitions_csv = os.path.join(project_dir, "partitions.csv")
    
    if os.path.exists(partitions_csv):
        with open(partitions_csv, "r") as f:
            for line in f:
                if "spiffs" in line or "littlefs" in line:
                    parts = line.split(",")
                    if len(parts) >= 4 and parts[3].strip():
                        fs_offset = parts[3].strip()
                    break
    
    if not fs_offset:
        fs_offset = "0x250000" 
        print(f"ℹ️ Offset calculated from partition sizes: {fs_offset}")

    if os.path.exists(fs_bin):
        flash_images.extend([fs_offset, fs_bin])
        print(f"📦 File System found at offset {fs_offset}")
    else:
        print("⚠️ Warning: littlefs.bin not found!")

    # Combine using the direct path to esptool.py
    cmd = [
        '"' + env.subst("$PYTHONEXE") + '"',
        '"' + esptool_path + '"',
        "--chip", "esp32c3", "merge_bin",
        "-o", f'"{output_name}"',
        "--flash_mode", "dio",
        "--flash_size", "4MB"
    ] + flash_images

    print(f"Executing: {' '.join(cmd)}")
    env.Execute(" ".join(cmd))
    print(f"✅ Success! Merged binary created: {output_name}\n")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_binaries)