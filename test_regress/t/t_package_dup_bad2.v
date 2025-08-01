// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

package Pkg;
endpackage

module t;
  IOBUF iocell (
      .O (in),
      .IO(pad),
      .I ('0),
      .T (~oe)
  );
endmodule

package Pkg;
endpackage
