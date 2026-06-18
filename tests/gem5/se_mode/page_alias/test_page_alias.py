# Copyright (c) 2026
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import re
import subprocess

from testlib import *


test_program_dir = joinpath(
    config.base_dir,
    "tests",
    "test-progs",
    "se",
    "page-alias",
)

class PageAliasProgram(Fixture):
    def __init__(self):
        super().__init__(name="build page_alias SE test program")
        self.built = False

    def setup(self, testitem):
        if self.built:
            return
        subprocess.run(
            ["make", "-C", test_program_dir, "page_alias"],
            check=True,
        )
        self.built = True


page_alias_binary = PageAliasProgram()

gem5_verify_config(
    name="se-page-alias-preservation",
    fixtures=(page_alias_binary,),
    verifiers=(
        verifier.MatchRegex(
            re.compile(r"SE page alias preservation passed")
        ),
    ),
    config=joinpath(test_program_dir, "run.py"),
    config_args=[],
    valid_isas=(constants.vega_x86_tag,),
    valid_variants=(constants.opt_tag,),
    length=constants.quick_tag,
)
