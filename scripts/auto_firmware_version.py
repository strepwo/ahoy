# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2022 Thomas Basler and others
#

Import("env")

try:
    from dulwich import porcelain
except ModuleNotFoundError:
    env.Execute('"$PYTHONEXE" -m pip install dulwich')
    from dulwich import porcelain

from dulwich import porcelain

def get_firmware_specifier_build_flag():
    try:
        build_version = porcelain.describe('../')  # refers to the repository root dir
    except:
        build_version = "g0000000"

    build_flag = "-D AUTO_GIT_HASH=\\\"" + build_version[1:] + "\\\" "
    build_flag += "-DENV_NAME=\\\"" + env["PIOENV"] + "\\\" ";
    print ("Firmware Revision: " + build_version)
    return (build_flag)

env.Append(
    BUILD_FLAGS=[get_firmware_specifier_build_flag()]
)
