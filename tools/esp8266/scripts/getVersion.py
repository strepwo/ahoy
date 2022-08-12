import os
from datetime import date
from dulwich import porcelain
import pkg_resources

Import("env")

required_pkgs = {'dulwich'}
installed_pkgs = {pkg.key for pkg in pkg_resources.working_set}
missing_pkgs = required_pkgs - installed_pkgs

if missing_pkgs:
    env.Execute('"$PYTHONEXE" -m pip install dulwich --global-option="--pure"')

def get_firmware_specifier_build_flag():
    try:
        build_version = porcelain.describe('../../')  # refers to the repository root dir
    except:
        build_version = "g0000000"

    return (build_version)


def readVersion(path, infile):
    f = open(path + infile, "r")
    lines = f.readlines()
    f.close()

    today = date.today()
    search = ["_MAJOR", "_MINOR", "_PATCH"]
    version = today.strftime("%y%m%d") + "_ahoy_"
    for line in lines:
        if(line.find("VERSION_") != -1):
            for s in search:
                p = line.find(s)
                if(p != -1):
                    version += line[p+13:].rstrip() + "."
    
    os.mkdir(path + ".pio/build/out/")
    
    versionout = version[:-1] + "_esp8266_debug_" + get_firmware_specifier_build_flag() + ".bin"
    src = path + ".pio/build/esp8266-debug/firmware.bin"
    dst = path + ".pio/build/out/" + versionout
    os.rename(src, dst)
    
    versionout = version[:-1] + "_esp8266_release_" + get_firmware_specifier_build_flag() + ".bin"
    src = path + ".pio/build/esp8266-release/firmware.bin"
    dst = path + ".pio/build/out/" + versionout
    os.rename(src, dst)

readVersion("../", "defines.h")
