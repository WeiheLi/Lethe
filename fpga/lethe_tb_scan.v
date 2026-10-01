// Testbench for the window-end-scan core: replays hwmodel_scan's stimulus and
// dumps every report the RTL emits.
`timescale 1ns/1ps
`default_nettype none

module lethe_tb_scan;
    localparam integer ADDR_W = 11;
    localparam integer ID_W   = 64;
    localparam integer TS_W   = 16;
    localparam integer P_W    = 16;
    localparam integer MAXP   = 4000000;

    reg clk = 1'b0, rst = 1'b1;
    always #1 clk = ~clk;

    reg              in_valid = 1'b0;
    reg  [ID_W-1:0]  in_id    = {ID_W{1'b0}};
    reg  [TS_W-1:0]  in_cw    = {TS_W{1'b0}};
    wire             out_ready, rep_valid;
    wire [ID_W-1:0]  rep_id;
    wire [P_W-1:0]   rep_p;

    lethe_core #(.ADDR_W(ADDR_W), .L(80), .GR(9),
                 .ID_W(ID_W), .TS_W(TS_W), .P_W(P_W))
    dut (.clk(clk), .rst(rst), .in_valid(in_valid), .out_ready(out_ready),
         .in_id(in_id), .in_cw(in_cw),
         .rep_valid(rep_valid), .rep_id(rep_id), .rep_p(rep_p));

    reg [ID_W-1:0] sid [0:MAXP-1];
    reg [31:0]     scw [0:MAXP-1];
    integer npkt, i, fd, code, fo, nrep, fs, w, a;
    reg [ID_W+P_W+TS_W:0] bv;
    reg [1023:0] stim_path, out_path;

    initial begin
        nrep = 0;
        if (!$value$plusargs("stim=%s", stim_path)) stim_path = "stim_scan.txt";
        if (!$value$plusargs("out=%s",  out_path))  out_path  = "rtl_scan.txt";

        fd = $fopen(stim_path, "r");
        if (fd == 0) begin $display("TB_ERROR cannot open stimulus"); $finish; end
        npkt = 0;
        code = $fscanf(fd, "%h %h\n", sid[0], scw[0]);
        while (code == 2 && npkt < MAXP-1) begin
            npkt = npkt + 1;
            code = $fscanf(fd, "%h %h\n", sid[npkt], scw[npkt]);
        end
        $fclose(fd);
        $display("TB_INFO packets=%0d", npkt);

        fo = $fopen(out_path, "w");
        repeat (8) @(posedge clk);
        rst <= 1'b0;

        in_id    <= sid[0];
        in_cw    <= scw[0][TS_W-1:0];
        in_valid <= 1'b1;
        i         = 1;
        wait (i >= npkt);
        repeat (8) @(posedge clk);
        in_valid <= 1'b0;
        repeat (2048) @(posedge clk);       // let the last sweep drain
        $fclose(fo);
        fs = $fopen("rtl_state.txt", "w");
        for (w = 0; w < 2; w = w + 1)
            for (a = 0; a < (1 << ADDR_W); a = a + 1) begin
                bv = w ? dut.mem1[a] : dut.mem0[a];
                $fwrite(fs, "%0d %0d %016h %0d %0d %0d\n", w, a,
                        bv[ID_W-1:0], bv[ID_W+P_W-1:ID_W],
                        bv[ID_W+P_W+TS_W-1:ID_W+P_W], bv[ID_W+P_W+TS_W]);
            end
        $fclose(fs);
        $display("TB_DONE reports=%0d", nrep);
        $finish;
    end

    always @(posedge clk) begin
        if (!rst && rep_valid) begin
            $fwrite(fo, "%016h %0d\n", rep_id, rep_p);
            nrep = nrep + 1;
        end
    end

    // one packet per read phase
    always @(posedge clk) begin
        if (!rst && in_valid && out_ready && i < npkt) begin
            in_id <= sid[i];
            in_cw <= scw[i][TS_W-1:0];
            i     <= i + 1;
        end
    end

endmodule

`default_nettype wire
