// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Antmicro.
// SPDX-License-Identifier: CC0-1.0

class SubClass;
  rand bit [2:0] field;
  function new ();
    field = 0;
  endfunction
endclass
class MyClass;
  SubClass sc_inst2[int];
  function new ();
    sc_inst2[1] = new;
  endfunction
endclass;
class Deep;
  MyClass sc_inst1;
  function new ();
    sc_inst1 = new;
  endfunction
endclass;
class WeNeedToGoDeeper;
  Deep sc_inst;
  function new ();
    sc_inst = new;
  endfunction
endclass;

module t;
  initial begin
    WeNeedToGoDeeper cl_inst[string];
    MyClass cl_inst2[int];
    cl_inst["val1"] = new;
    cl_inst2[0] = new;
    repeat(10) begin
      if (cl_inst["val1"].sc_inst.sc_inst1.sc_inst2[1].randomize() with {field inside {1, 2, 3};} == 0) begin
        $stop;
      end
      if (cl_inst["val1"].sc_inst.sc_inst1.sc_inst2[1].field < 1 || cl_inst["val1"].sc_inst.sc_inst1.sc_inst2[1].field > 3) begin
        $stop;
      end
      if (cl_inst2[0].sc_inst2[1].randomize() with {field inside {1, 2, 3};} == 0) begin
        $stop;
      end
      if (cl_inst2[0].sc_inst2[1].field < 1 || cl_inst2[0].sc_inst2[1].field > 3) begin
        $stop;
      end
    end
   $write("*-* All Finished *-*\n");
   $finish;
  end
endmodule
