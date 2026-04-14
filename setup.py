import os
import shutil
import setuptools
import setuptools.command.build_ext
from wheel.bdist_wheel import bdist_wheel

ROOT_DIR = os.path.dirname(__file__)


class BinaryDistribution(setuptools.Distribution):
    """Forces setuptools to produce a platform-specific wheel."""

    def has_ext_modules(self):
        return True


class HAProxyBdistWheel(bdist_wheel):
    """Produces a py3-none-{platform} tag — no Python ABI dependency."""

    def finalize_options(self):
        super().finalize_options()
        self.root_is_pure = False  # prevents py3-none-any

    def get_tag(self):
        _, _, platform_tag = super().get_tag()
        return "py3", "none", platform_tag  # e.g. py3-none-manylinux_2_17_x86_64


class build_ext(setuptools.command.build_ext.build_ext):
    """Copies the pre-built HAProxy binary + vendored libs into build_lib."""

    def run(self):
        bin_src = os.path.join(ROOT_DIR, "ray_haproxy", "bin", "haproxy")
        lib_src = os.path.join(ROOT_DIR, "ray_haproxy", "bin", "lib")

        files_to_copy = []
        if os.path.isfile(bin_src):
            files_to_copy.append(bin_src)
        if os.path.isdir(lib_src):
            for name in os.listdir(lib_src):
                path = os.path.join(lib_src, name)
                if os.path.isfile(path):
                    files_to_copy.append(path)

        for src in files_to_copy:
            # Compute path relative to ROOT_DIR so we can mirror it in build_lib.
            rel = os.path.relpath(src, ROOT_DIR)
            dest = os.path.join(self.build_lib, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copy2(src, dest)
            # Preserve executable bit.
            os.chmod(dest, os.stat(dest).st_mode | 0o111)


setuptools.setup(
    distclass=BinaryDistribution,
    cmdclass={
        "build_ext": build_ext,
        "bdist_wheel": HAProxyBdistWheel,
    },
    zip_safe=False,
    setup_requires=["wheel"],
    packages=["ray_haproxy"],
    package_data={"ray_haproxy": ["bin/haproxy", "bin/lib/*.so*"]},
    data_files=[
        ("", ["LICENSE", "THIRD_PARTY_LICENSES"]),
    ],
)
