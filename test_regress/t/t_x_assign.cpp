// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
//
// Copyright 2020 by Geza Lore. This program is free software; you can
// redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "verilated.h"

#include <iostream>
#include VM_PREFIX_INCLUDE

double sc_time_stamp() { return 0; }

// clang-format off
#if defined(T_X_ASSIGN_0)
# define EXPECTED 0
#elif defined(T_X_ASSIGN_1)
# define EXPECTED 1
#endif
// clang-format on

int main(int argc, const char** argv) {
#if defined(T_X_ASSIGN_UNIQUE_0)
    Verilated::randReset(0);
#elif defined(T_X_ASSIGN_UNIQUE_1)
    Verilated::randReset(1);
#endif

    VM_PREFIX* top = new VM_PREFIX{};

    // Evaluate one clock posedge
    top->clk = 0;
    top->eval();
    top->clk = 1;
    top->eval();

#if defined(T_X_ASSIGN_UNIQUE_0) || defined(T_X_ASSIGN_UNIQUE_1)
    if (top->o_int == 0 || top->o_int == -1) {
        vl_fatal(__FILE__, __LINE__, "TOP.t", "x assign was not unique");
        exit(1);
    }
#else
    if (top->o != EXPECTED) {
        vl_fatal(__FILE__, __LINE__, "TOP.t", "incorrect module output");
        exit(1);
    }

    uint32_t o_int_expected = EXPECTED ? 0xffffffff : 0;

    if (top->o_int != o_int_expected) {
        vl_fatal(__FILE__, __LINE__, "TOP.t", "incorrect module output");
        exit(1);
    }
#endif

    VL_DO_DANGLING(delete top, top);
    std::cout << "*-* All Finished *-*" << std::endl;
    return 0;
}
