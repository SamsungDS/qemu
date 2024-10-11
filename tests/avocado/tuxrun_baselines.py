# Functional test that boots known good tuxboot images the same way
# that tuxrun (www.tuxrun.org) does. This tool is used by things like
# the LKFT project to run regression tests on kernels.
#
# Copyright (c) 2023 Linaro Ltd.
#
# Author:
#  Alex Bennée <alex.bennee@linaro.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import time
import tempfile

from avocado import skip, skipUnless
from avocado_qemu import QemuSystemTest
from avocado_qemu import exec_command, exec_command_and_wait_for_pattern
from avocado_qemu import wait_for_console_pattern
from avocado.utils import process
from avocado.utils.path import find_command

class TuxRunBaselineTest(QemuSystemTest):
    """
    :avocado: tags=accel:tcg
    """

    KERNEL_COMMON_COMMAND_LINE = 'printk.time=0'
    # Tests are ~10-40s, allow for --debug/--enable-gcov overhead
    timeout = 100

    def get_tag(self, tagname, default=None):
        """
        Get the metadata tag or return the default.
        """
        utag = self._get_unique_tag_val(tagname)
        print(f"{tagname}/{default} -> {utag}")
        if utag:
            return utag

        return default

    def setUp(self):
        super().setUp()

        # We need zstd for all the tuxrun tests
        # See https://github.com/avocado-framework/avocado/issues/5609
        zstd = find_command('zstd', False)
        if zstd is False:
            self.cancel('Could not find "zstd", which is required to '
                        'decompress rootfs')
        self.zstd = zstd

        # Process the TuxRun specific tags, most machines work with
        # reasonable defaults but we sometimes need to tweak the
        # config. To avoid open coding everything we store all these
        # details in the metadata for each test.

        # The tuxboot tag matches the root directory
        self.tuxboot = self.get_tag('tuxboot')

        # Most Linux's use ttyS0 for their serial port
        self.console = self.get_tag('console', "ttyS0")

        # Does the machine shutdown QEMU nicely on "halt"
        self.shutdown = self.get_tag('shutdown')

        # The name of the kernel Image file
        self.image = self.get_tag('image', "Image")

        self.root = self.get_tag('root', "vda")

        # Occasionally we need extra devices to hook things up
        self.extradev = self.get_tag('extradev')

        self.qemu_img = super().get_qemu_img()

    def wait_for_console_pattern(self, success_message, vm=None):
        wait_for_console_pattern(self, success_message,
                                 failure_message='Kernel panic - not syncing',
                                 vm=vm)

    def fetch_tuxrun_assets(self, csums=None, dt=None):
        """
        Fetch the TuxBoot assets. They are stored in a standard way so we
        use the per-test tags to fetch details.
        """
        base_url = f"https://storage.tuxboot.com/20230331/{self.tuxboot}/"

        # empty hash if we weren't passed one
        csums = {} if csums is None else csums
        ksum = csums.get(self.image, None)
        isum = csums.get("rootfs.ext4.zst", None)

        kernel_image =  self.fetch_asset(base_url + self.image,
                                         asset_hash = ksum,
                                         algorithm = "sha256")
        disk_image_zst = self.fetch_asset(base_url + "rootfs.ext4.zst",
                                         asset_hash = isum,
                                         algorithm = "sha256")

        cmd = f"{self.zstd} -d {disk_image_zst} -o {self.workdir}/rootfs.ext4"
        process.run(cmd)

        if dt:
            dsum = csums.get(dt, None)
            dtb = self.fetch_asset(base_url + dt,
                                   asset_hash = dsum,
                                   algorithm = "sha256")
        else:
            dtb = None

        return (kernel_image, self.workdir + "/rootfs.ext4", dtb)

    def prepare_run(self, kernel, disk, drive, dtb=None, console_index=0):
        """
        Setup to run and add the common parameters to the system
        """
        self.vm.set_console(console_index=console_index)

        # all block devices are raw ext4's
        blockdev = "driver=raw,file.driver=file," \
            + f"file.filename={disk},node-name=hd0"

        kcmd_line = self.KERNEL_COMMON_COMMAND_LINE
        kcmd_line += f" root=/dev/{self.root}"
        kcmd_line += f" console={self.console}"

        self.vm.add_args('-kernel', kernel,
                         '-append', kcmd_line,
                         '-blockdev', blockdev)

        # Sometimes we need extra devices attached
        if self.extradev:
            self.vm.add_args('-device', self.extradev)

        self.vm.add_args('-device',
                         f"{drive},drive=hd0")

        # Some machines need an explicit DTB
        if dtb:
            self.vm.add_args('-dtb', dtb)

    def run_tuxtest_tests(self, haltmsg):
        """
        Wait for the system to boot up, wait for the login prompt and
        then do a few things on the console. Trigger a shutdown and
        wait to exit cleanly.
        """
        self.wait_for_console_pattern("Welcome to TuxTest")
        time.sleep(0.2)
        exec_command(self, 'root')
        time.sleep(0.2)
        exec_command(self, 'cat /proc/interrupts')
        time.sleep(0.1)
        exec_command(self, 'cat /proc/self/maps')
        time.sleep(0.1)
        exec_command(self, 'uname -a')
        time.sleep(0.1)
        exec_command_and_wait_for_pattern(self, 'halt', haltmsg)

        # Wait for VM to shut down gracefully if it can
        if self.shutdown == "nowait":
            self.vm.shutdown()
        else:
            self.vm.wait()

    def common_tuxrun(self,
                      csums=None,
                      dt=None,
                      drive="virtio-blk-device",
                      haltmsg="reboot: System halted",
                      console_index=0):
        """
        Common path for LKFT tests. Unless we need to do something
        special with the command line we can process most things using
        the tag metadata.
        """
        (kernel, disk, dtb) = self.fetch_tuxrun_assets(csums, dt)

        self.prepare_run(kernel, disk, drive, dtb, console_index)
        self.vm.launch()
        self.run_tuxtest_tests(haltmsg)


    #
    # The tests themselves. The configuration is derived from how
    # tuxrun invokes qemu (with minor tweaks like using -blockdev
    # consistently). The tuxrun equivalent is something like:
    #
    # tuxrun --device qemu-{ARCH} \
    #        --kernel https://storage.tuxboot.com/{TUXBOOT}/{IMAGE}
    #

    def test_arm64(self):
        """
        :avocado: tags=arch:aarch64
        :avocado: tags=cpu:cortex-a57
        :avocado: tags=machine:virt
        :avocado: tags=tuxboot:arm64
        :avocado: tags=console:ttyAMA0
        :avocado: tags=shutdown:nowait
        """
        sums = {"Image" :
                "ce95a7101a5fecebe0fe630deee6bd97b32ba41bc8754090e9ad8961ea8674c7",
                "rootfs.ext4.zst" :
                "bbd5ed4b9c7d3f4ca19ba71a323a843c6b585e880115df3b7765769dbd9dd061"}
        self.common_tuxrun(csums=sums)

    def test_arm64be(self):
        """
        :avocado: tags=arch:aarch64
        :avocado: tags=cpu:cortex-a57
        :avocado: tags=endian:big
        :avocado: tags=machine:virt
        :avocado: tags=tuxboot:arm64be
        :avocado: tags=console:ttyAMA0
        :avocado: tags=shutdown:nowait
        """
        sums = { "Image" :
                 "e0df4425eb2cd9ea9a283e808037f805641c65d8fcecc8f6407d8f4f339561b4",
                 "rootfs.ext4.zst" :
                 "e6ffd8813c8a335bc15728f2835f90539c84be7f8f5f691a8b01451b47fb4bd7"}
        self.common_tuxrun(csums=sums)

    def test_i386(self):
        """
        :avocado: tags=arch:i386
        :avocado: tags=cpu:coreduo
        :avocado: tags=machine:q35
        :avocado: tags=tuxboot:i386
        :avocado: tags=image:bzImage
        :avocado: tags=shutdown:nowait
        """
        sums = {"bzImage" :
                "a3e5b32a354729e65910f5a1ffcda7c14a6c12a55e8213fb86e277f1b76ed956",
                "rootfs.ext4.zst" :
                "f15e66b2bf673a210ec2a4b2e744a80530b36289e04f5388aab812b97f69754a" }

        self.common_tuxrun(csums=sums, drive="virtio-blk-pci")

    def test_mips32(self):
        """
        :avocado: tags=arch:mips
        :avocado: tags=machine:malta
        :avocado: tags=cpu:mips32r6-generic
        :avocado: tags=endian:big
        :avocado: tags=tuxboot:mips32
        :avocado: tags=image:vmlinux
        :avocado: tags=root:sda
        :avocado: tags=shutdown:nowait
        """
        sums = { "rootfs.ext4.zst" :
                 "fc3da0b4c2f38d74c6d705123bb0f633c76ed953128f9d0859378c328a6d11a0",
                 "vmlinux" :
                 "bfd2172f8b17fb32970ca0c8c58f59c5a4ca38aa5855d920be3a69b5d16e52f0" }

        self.common_tuxrun(csums=sums, drive="driver=ide-hd,bus=ide.0,unit=0")

    def test_mips32el(self):
        """
        :avocado: tags=arch:mipsel
        :avocado: tags=machine:malta
        :avocado: tags=cpu:mips32r6-generic
        :avocado: tags=tuxboot:mips32el
        :avocado: tags=image:vmlinux
        :avocado: tags=root:sda
        :avocado: tags=shutdown:nowait
        """
        sums = { "rootfs.ext4.zst" :
                 "e799768e289fd69209c21f4dacffa11baea7543d5db101e8ce27e3bc2c41d90e",
                 "vmlinux" :
                 "8573867c68a8443db8de6d08bb33fb291c189ca2ca671471d3973a3e712096a3" }

        self.common_tuxrun(csums=sums, drive="driver=ide-hd,bus=ide.0,unit=0")

    def test_mips64(self):
        """
        :avocado: tags=arch:mips64
        :avocado: tags=machine:malta
        :avocado: tags=tuxboot:mips64
        :avocado: tags=endian:big
        :avocado: tags=image:vmlinux
        :avocado: tags=root:sda
        :avocado: tags=shutdown:nowait
        """
        sums = { "rootfs.ext4.zst" :
                 "69d91eeb04df3d8d172922c6993bb37d4deeb6496def75d8580f6f9de3e431da",
                 "vmlinux" :
                 "09010e51e4b8bcbbd2494786ffb48eca78f228e96e5c5438344b0eac4029dc61" }

        self.common_tuxrun(csums=sums, drive="driver=ide-hd,bus=ide.0,unit=0")

    def test_mips64el(self):
        """
        :avocado: tags=arch:mips64el
        :avocado: tags=machine:malta
        :avocado: tags=tuxboot:mips64el
        :avocado: tags=image:vmlinux
        :avocado: tags=root:sda
        :avocado: tags=shutdown:nowait
        """
        sums = { "rootfs.ext4.zst" :
                 "fba585368f5915b1498ed081863474b2d7ec4e97cdd46d21bdcb2f9698f83de4",
                 "vmlinux" :
                 "d4e08965e2155c4cccce7c5f34d18fe34c636cda2f2c9844387d614950155266" }

        self.common_tuxrun(csums=sums, drive="driver=ide-hd,bus=ide.0,unit=0")

    def test_ppc32(self):
        """
        :avocado: tags=arch:ppc
        :avocado: tags=machine:ppce500
        :avocado: tags=cpu:e500mc
        :avocado: tags=tuxboot:ppc32
        :avocado: tags=image:uImage
        :avocado: tags=shutdown:nowait
        """
        sums = { "rootfs.ext4.zst" :
                 "8885b9d999cc24d679542a02e9b6aaf48f718f2050ece6b8347074b6ee41dd09",
                 "uImage" :
                 "1a68f74b860fda022fb12e03c5efece8c2b8b590d96cca37a8481a3ae0b3f81f" }

        self.common_tuxrun(csums=sums, drive="virtio-blk-pci")

    def test_riscv64(self):
        """
        :avocado: tags=arch:riscv64
        :avocado: tags=machine:virt
        :avocado: tags=tuxboot:riscv64
        """
        sums = { "Image" :
                 "cd634badc65e52fb63465ec99e309c0de0369f0841b7d9486f9729e119bac25e",
                 "fw_jump.elf" :
                 "6e3373abcab4305fe151b564a4c71110d833c21f2c0a1753b7935459e36aedcf",
                 "rootfs.ext4.zst" :
                 "b18e3a3bdf27be03da0b285e84cb71bf09eca071c3a087b42884b6982ed679eb" }

        self.common_tuxrun(csums=sums)

    def test_riscv64_maxcpu(self):
        """
        :avocado: tags=arch:riscv64
        :avocado: tags=machine:virt
        :avocado: tags=cpu:max
        :avocado: tags=tuxboot:riscv64
        """
        sums = { "Image" :
                 "cd634badc65e52fb63465ec99e309c0de0369f0841b7d9486f9729e119bac25e",
                 "fw_jump.elf" :
                 "6e3373abcab4305fe151b564a4c71110d833c21f2c0a1753b7935459e36aedcf",
                 "rootfs.ext4.zst" :
                 "b18e3a3bdf27be03da0b285e84cb71bf09eca071c3a087b42884b6982ed679eb" }

        self.common_tuxrun(csums=sums)

    # Note: some segfaults caused by unaligned userspace access
    @skipUnless(os.getenv('QEMU_TEST_FLAKY_TESTS'), 'Test is unstable on GitLab')
    def test_sh4(self):
        """
        :avocado: tags=arch:sh4
        :avocado: tags=machine:r2d
        :avocado: tags=cpu:sh7785
        :avocado: tags=tuxboot:sh4
        :avocado: tags=image:zImage
        :avocado: tags=root:sda
        :avocado: tags=console:ttySC1
        :avocado: tags=flaky
        """
        sums = { "rootfs.ext4.zst" :
                 "3592a7a3d5a641e8b9821449e77bc43c9904a56c30d45da0694349cfd86743fd",
                 "zImage" :
                 "29d9b2aba604a0f53a5dc3b5d0f2b8e35d497de1129f8ee5139eb6fdf0db692f" }

        # The test is currently too unstable to do much in userspace
        # so we skip common_tuxrun and do a minimal boot and shutdown.
        (kernel, disk, dtb) = self.fetch_tuxrun_assets(csums=sums)

        # the console comes on the second serial port
        self.prepare_run(kernel, disk,
                         "driver=ide-hd,bus=ide.0,unit=0",
                         console_index=1)
        self.vm.launch()

        self.wait_for_console_pattern("Welcome to TuxTest")
        time.sleep(0.1)
        exec_command(self, 'root')
        time.sleep(0.1)
        exec_command_and_wait_for_pattern(self, 'halt',
                                          "reboot: System halted")

    def test_x86_64(self):
        """
        :avocado: tags=arch:x86_64
        :avocado: tags=machine:q35
        :avocado: tags=cpu:Nehalem
        :avocado: tags=tuxboot:x86_64
        :avocado: tags=image:bzImage
        :avocado: tags=root:sda
        :avocado: tags=shutdown:nowait
        """
        sums = { "bzImage" :
                 "2bc7480a669ee9b6b82500a236aba0c54233debe98cb968268fa230f52f03461",
                 "rootfs.ext4.zst" :
                 "b72ac729769b8f51c6dffb221113c9a063c774dbe1d66af30eb593c4e9999b4b" }

        self.common_tuxrun(csums=sums,
                           drive="driver=ide-hd,bus=ide.0,unit=0")
