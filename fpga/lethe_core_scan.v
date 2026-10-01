`timescale 1ns/1ps
// ---------------------------------------------------------------------------
// Lethe: vanished-flow detection.  Kintex UltraScale+ (KCU116 / XCKU5P).
//
// Faithful to lethe.h at the default configuration:
//   grouped=0, harvest_first=1, ripening_protection=1, age_decay=0, admit=1.0,
//   scan=1.
// Under that configuration the placement path is entirely deterministic; the
// only randomness is the decay coin, which lethe.h writes as
//   rng.unit() * P < decay_k * (g+1)
// i.e. a multiply and a compare, no divide.
//
// Each way is one true dual-port BRAM.  A BRAM port carries a single address,
// so the core runs a two-phase cycle: phase R reads, phase W writes back, i.e.
// one packet every two cycles.  Port A serves the packet throughout.  When a
// window closes, the scan walks the whole table on port B, one bucket per
// packet, and packets carry on undisturbed.  The sweep needs 2*M cycles and a
// window supplies 2*N of them, so it fits whenever M <= N. That holds at
// every size in the paper's Table 2 and across its small-memory budgets,
// NOT across its whole accuracy axis: at 1.72 MB, M/N reaches 5.5 on
// Campus, far outside what one pass a window can cover.
// ---------------------------------------------------------------------------
`default_nettype none

module lethe_core #(
    parameter integer ADDR_W = 13,   // buckets per way = 2**ADDR_W
    parameter integer L      = 80,   // establishment threshold
    parameter integer GR     = 9,    // the scan is triggered by the first
                                     // packet of the next window, so a silence
                                     // of g completed windows reads as g+1
    parameter integer ID_W   = 64,   // flow key, carried whole
    parameter integer TS_W   = 16,   // last-seen field: a window index
    parameter integer P_W    = 16,   // persistence counter width
    parameter integer KNUM   = 9,    // decay coin: rng*P*KDEN < KNUM<<32
    parameter integer KDEN   = 2     //   decay_k*(g+1) = 9/2 = 4.5
) (
    input  wire              clk,
    input  wire              rst,
    input  wire              in_valid,
    output wire              out_ready,
    input  wire [ID_W-1:0]   in_id,
    input  wire [TS_W-1:0]   in_cw,
    output reg               rep_valid,
    output reg  [ID_W-1:0]   rep_id,
    output reg  [P_W-1:0]    rep_p
);
    localparam integer BW     = ID_W + P_W + TS_W + 1;   // bucket width, 97 bits
    localparam integer DEPTH  = 1 << ADDR_W;
    localparam integer RANK_W = P_W + 1;

    // bucket layout: {rep, lw, P, id}
    `define F_ID(v)  v[ID_W-1:0]
    `define F_P(v)   v[ID_W-1+P_W:ID_W]
    `define F_LW(v)  v[ID_W-1+P_W+TS_W:ID_W+P_W]
    `define F_REP(v) v[BW-1]

    // ---------------- two-phase control ----------------------------------
    reg phase;
    wire ph_r = (phase == 1'b0);
    assign out_ready = ph_r;
    always @(posedge clk)
        if (rst) phase <= 1'b0;
        else     phase <= ~phase;

    // ---------------- hashes ---------------------------------------------
    // the key is folded to a word before hashing, which keeps the two
    // multipliers 32 bits wide whatever the key width
    wire [31:0] idf = in_id[63:32] ^ in_id[31:0];
    wire [31:0] hm0 = idf * 32'h2545F491;
    wire [31:0] hm1 = idf * 32'h9E3779B1;
    wire [ADDR_W-1:0] ha0 = hm0[31 -: ADDR_W];
    wire [ADDR_W-1:0] ha1 = hm1[31 -: ADDR_W];

    reg [ADDR_W-1:0] ra0, ra1;
    reg [ID_W-1:0]   rid;
    reg [TS_W-1:0]   rcw;
    reg              rvalid;

    always @(posedge clk) begin
        if (rst) rvalid <= 1'b0;
        else if (ph_r) begin
            rvalid <= in_valid;
            rid <= in_id; rcw <= in_cw; ra0 <= ha0; ra1 <= ha1;
        end
    end

    // ---------------- window-end scan pointer ----------------------------
    // hptr/haddr/hway are leftover names from the earlier continuous-hand
    // core, whose pointer had no gate and never stopped. This one is gated
    // by `sweeping`: a window edge sets it, and it clears as soon as the
    // pointer completes one revolution. One pass per window, then idle.
    // The first packet of a new window starts a sweep; the pointer then walks
    // both ways once and stops until the next window closes.
    reg [TS_W-1:0]  last_cw;
    reg             sweeping;
    reg [ADDR_W:0]  hptr;
    wire            win_edge = ph_r && in_valid && (in_cw != last_cw);

    always @(posedge clk) begin
        if (rst) begin
            last_cw <= {TS_W{1'b0}}; sweeping <= 1'b0; hptr <= {(ADDR_W+1){1'b0}};
        end else if (win_edge) begin
            last_cw <= in_cw; sweeping <= 1'b1; hptr <= {(ADDR_W+1){1'b0}};
        end else if (!ph_r && sweeping) begin
            if (hptr == {(ADDR_W+1){1'b1}}) sweeping <= 1'b0;
            hptr <= hptr + 1'b1;             // one bucket per packet
        end
    end

    wire              hway  = hptr[ADDR_W];
    wire [ADDR_W-1:0] haddr = hptr[ADDR_W-1:0];

    // ---------------- memory: one TDP BRAM per way ------------------------
    wire [ADDR_W-1:0] adr_a0 = ph_r ? ha0 : ra0;
    wire [ADDR_W-1:0] adr_a1 = ph_r ? ha1 : ra1;

    reg we_a0, we_a1, we_b0, we_b1;
    reg [BW-1:0] wd_a0, wd_a1, wd_b;

    (* ram_style = "block" *) reg [BW-1:0] mem0 [0:DEPTH-1];
    (* ram_style = "block" *) reg [BW-1:0] mem1 [0:DEPTH-1];
    reg [BW-1:0] qa0, qa1, qb0, qb1;

    // BRAM powers up zeroed on the device; state it so simulation matches and
    // so the tools emit an INIT rather than leaving the array undefined
    integer ii;
    initial begin
        for (ii = 0; ii < DEPTH; ii = ii + 1) begin
            mem0[ii] = {BW{1'b0}};
            mem1[ii] = {BW{1'b0}};
        end
        qa0 = {BW{1'b0}}; qa1 = {BW{1'b0}};
        qb0 = {BW{1'b0}}; qb1 = {BW{1'b0}};
    end

    always @(posedge clk) begin
        if (we_a0) mem0[adr_a0] <= wd_a0;
        qa0 <= mem0[adr_a0];
    end
    always @(posedge clk) begin
        if (we_b0) mem0[haddr] <= wd_b;
        qb0 <= mem0[haddr];
    end
    always @(posedge clk) begin
        if (we_a1) mem1[adr_a1] <= wd_a1;
        qa1 <= mem1[adr_a1];
    end
    always @(posedge clk) begin
        if (we_b1) mem1[haddr] <= wd_b;
        qb1 <= mem1[haddr];
    end

    // ---------------- update decode, in the write phase -------------------
    wire [BW-1:0] b0 = qa0;
    wire [BW-1:0] b1 = qa1;

    wire [TS_W-1:0] s0 = rcw - `F_LW(b0);
    wire [TS_W-1:0] s1 = rcw - `F_LW(b1);
    wire nz0 = |`F_P(b0);
    wire nz1 = |`F_P(b1);
    wire es0 = (`F_P(b0) >= L[P_W-1:0]);
    wire es1 = (`F_P(b1) >= L[P_W-1:0]);
    wire rp0 = nz0 && !`F_REP(b0) && es0 && (s0 >= GR[TS_W-1:0]);
    wire rp1 = nz1 && !`F_REP(b1) && es1 && (s1 >= GR[TS_W-1:0]);

    // rank replaces score(): 0 if takeable now, P while climbing, max if protected
    wire [RANK_W-1:0] r0 = (!nz0 || `F_REP(b0) || rp0) ? {RANK_W{1'b0}}
                         : (es0 ? {RANK_W{1'b1}} : {1'b0, `F_P(b0)});
    wire [RANK_W-1:0] r1 = (!nz1 || `F_REP(b1) || rp1) ? {RANK_W{1'b0}}
                         : (es1 ? {RANK_W{1'b1}} : {1'b0, `F_P(b1)});

    wire hit0 = rvalid && nz0 && (`F_ID(b0) == rid);
    wire hit1 = rvalid && nz1 && (`F_ID(b1) == rid) && !hit0;
    wire hit  = hit0 | hit1;
    wire pick1 = (r1 < r0) || ((r1 == r0) && (s1 > s0));

    wire [BW-1:0]   hb  = hit0 ? b0 : b1;
    wire [TS_W-1:0] hs  = hit0 ? s0 : s1;
    wire            hrp = hit0 ? rp0 : rp1;

    wire [BW-1:0]   vb  = pick1 ? b1 : b0;
    wire            vrp = pick1 ? rp1 : rp0;
    wire            vfr = pick1 ? !nz1 : !nz0;
    wire            vre = pick1 ? `F_REP(b1) : `F_REP(b0);
    wire            ves = pick1 ? es1 : es0;
    wire [TS_W-1:0] vs  = pick1 ? s1 : s0;

    // climbing flow: if (s != 0 && s > P) { if (--P == 0) install; }
    // P_W and TS_W are equal here, so the silence needs no widening
    wire [P_W-1:0] vs_ext = vs;
    wire vdec  = !vfr && !vre && !ves && (vs != 0) && (vs_ext > `F_P(vb));
    wire vlast = vdec && (`F_P(vb) == {{(P_W-1){1'b0}}, 1'b1});

    wire [P_W-1:0] hp_next = ((hs != 0) && (`F_P(hb) != {P_W{1'b1}}))
                             ? `F_P(hb) + 1'b1 : `F_P(hb);

    wire [BW-1:0] wd_hit  = {1'b0, rcw, hp_next, `F_ID(hb)};
    wire [BW-1:0] wd_inst = {1'b0, rcw, {{(P_W-1){1'b0}}, 1'b1}, rid};
    wire [BW-1:0] wd_dec  = {`F_REP(vb), `F_LW(vb), `F_P(vb) - 1'b1, `F_ID(vb)};

    wire do_inst = rvalid && !hit && (vfr || vre || vrp);
    wire do_dec  = rvalid && !hit && !do_inst && vdec && !vlast;
    wire do_di   = rvalid && !hit && !do_inst && vlast;

    // ---------------- scan decode ------------------------------------------
    wire [BW-1:0]   hqb  = hway ? qb1 : qb0;
    wire [TS_W-1:0] hsil = rcw - `F_LW(hqb);
    wire hnz  = |`F_P(hqb);
    wire hest = (`F_P(hqb) >= L[P_W-1:0]);
    wire hdue = sweeping && hnz && !`F_REP(hqb) && (hsil >= GR[TS_W-1:0]);

    reg [31:0] lfsr;
    always @(posedge clk)
        if (rst)        lfsr <= 32'hACE12345;
        else if (!ph_r) lfsr <= {lfsr[30:0], lfsr[31]^lfsr[21]^lfsr[1]^lfsr[0]};

    wire [P_W+33:0] coin_l = lfsr * `F_P(hqb) * KDEN;
    wire [P_W+33:0] coin_r = {{(P_W+2){1'b0}}, KNUM[31:0]} << 32;
    wire            coin   = (coin_l < coin_r);

    wire h_report = hdue && hest;
    wire h_decay  = hdue && !hest && coin;
    wire h_touch  = hdue && !hest;

    wire [BW-1:0] wd_hrep = {1'b1, `F_LW(hqb), `F_P(hqb), `F_ID(hqb)};
    wire [BW-1:0] wd_hdec = (`F_P(hqb) == {{(P_W-1){1'b0}}, 1'b1}) ? {BW{1'b0}}
                          : {`F_REP(hqb), rcw, `F_P(hqb) - 1'b1, `F_ID(hqb)};
    wire [BW-1:0] wd_href = {`F_REP(hqb), rcw, `F_P(hqb), `F_ID(hqb)};

    // ---------------- write-back -------------------------------------------
    always @(*) begin
        we_a0 = 1'b0; we_a1 = 1'b0; wd_a0 = {BW{1'b0}}; wd_a1 = {BW{1'b0}};
        we_b0 = 1'b0; we_b1 = 1'b0; wd_b  = {BW{1'b0}};
        if (!ph_r) begin
            if (hit0) begin
                we_a0 = 1'b1; wd_a0 = wd_hit;
            end else if (hit1) begin
                we_a1 = 1'b1; wd_a1 = wd_hit;
            end else if (do_inst || do_di) begin
                if (pick1) begin we_a1 = 1'b1; wd_a1 = wd_inst; end
                else       begin we_a0 = 1'b1; wd_a0 = wd_inst; end
            end else if (do_dec) begin
                if (pick1) begin we_a1 = 1'b1; wd_a1 = wd_dec; end
                else       begin we_a0 = 1'b1; wd_a0 = wd_dec; end
            end
            if (h_report || h_touch) begin
                wd_b = h_report ? wd_hrep : (h_decay ? wd_hdec : wd_href);
                if (hway) we_b1 = 1'b1;
                else      we_b0 = 1'b1;
            end
        end
    end

    // ---------------- report port ------------------------------------------
    // A report is emitted when a due bucket is released, by either engine, and
    // when a returning packet finds its own bucket already due.
    always @(posedge clk) begin
        if (rst) begin
            rep_valid <= 1'b0; rep_id <= {ID_W{1'b0}}; rep_p <= {P_W{1'b0}};
        end else if (!ph_r && hit && hrp) begin
            rep_valid <= 1'b1; rep_id <= `F_ID(hb);  rep_p <= `F_P(hb);
        end else if (!ph_r && rvalid && !hit && vrp) begin
            rep_valid <= 1'b1; rep_id <= `F_ID(vb);  rep_p <= `F_P(vb);
        end else if (!ph_r && h_report) begin
            rep_valid <= 1'b1; rep_id <= `F_ID(hqb); rep_p <= `F_P(hqb);
        end else begin
            rep_valid <= 1'b0;
        end
    end

endmodule

`default_nettype wire
