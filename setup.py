# This file is part of "echion" which is released under MIT.
#
# Copyright (c) 2023 Gabriele N. Tornetta <phoenix1987@gmail.com>.

from pathlib import Path
import sys
from setuptools import find_packages, setup, Extension

echionmodule = Extension(
    "echion.core",
    sources=["echion/coremodule.cc"],
    include_dirs=["."],
    define_macros=[(f"PL_{sys.platform.upper()}", None)],
)

setup(
    name="echion",
    author="Gabriele N. Tornetta",
    version="0.1.0",
    description="In-process Python sampling profiler",
    long_description=Path("README.md").read_text(),
    ext_modules=[echionmodule],
    entry_points={
        "console_scripts": ["echion=echion.__main__:main"],
    },
    packages=find_packages(exclude=["tests"]),
)
