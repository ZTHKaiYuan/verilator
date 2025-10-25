#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2024 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

# We also test +librescan and +notimingchecks here, which are NOPs
test.compile(v_flags2=[
    "-f t/t_flag_define.vc -DCMD_DEF -DCMD_UNDEF -UCMD_UNDEF +define+CMD_DEF2", "+librescan",
    "+notimingchecks"
])

test.execute()

test.passes()
