`timescale 1ns/1ps
`default_nettype none

module hygcn_aggregation_buffer_ctrl #(
    parameter int BANKS = 2,
    parameter int BANK_BYTES = 8 * 1024 * 1024,
    parameter int BATCH_ID_WIDTH = 16,
    parameter int BYTE_COUNT_WIDTH = 32,
    parameter int BANK_ID_WIDTH = (BANKS <= 2) ? 1 : $clog2(BANKS)
) (
    input  logic                                  clk,
    input  logic                                  rst_n,

    input  logic                                  alloc_valid,
    output logic                                  alloc_ready,
    input  logic [BATCH_ID_WIDTH-1:0]             alloc_batch_id,
    input  logic [BYTE_COUNT_WIDTH-1:0]           alloc_bytes,
    output logic [BANK_ID_WIDTH-1:0]              alloc_bank_id,

    input  logic                                  ae_done_valid,
    input  logic [BANK_ID_WIDTH-1:0]              ae_done_bank_id,

    output logic                                  ce_valid,
    input  logic                                  ce_ready,
    output logic [BANK_ID_WIDTH-1:0]              ce_bank_id,
    output logic [BATCH_ID_WIDTH-1:0]             ce_batch_id,
    output logic [BYTE_COUNT_WIDTH-1:0]           ce_bytes,

    input  logic                                  ce_done_valid,
    input  logic [BANK_ID_WIDTH-1:0]              ce_done_bank_id,

    output logic [BANKS*3-1:0]                    bank_state,
    output logic                                  protocol_error
);
    localparam logic [2:0] STATE_FREE = 3'd0;
    localparam logic [2:0] STATE_AE_ACTIVE = 3'd1;
    localparam logic [2:0] STATE_READY = 3'd2;
    localparam logic [2:0] STATE_CE_ACTIVE = 3'd3;
    localparam logic [BYTE_COUNT_WIDTH-1:0] BANK_BYTES_VALUE =
        BYTE_COUNT_WIDTH'(BANK_BYTES);

    logic [2:0] state [0:BANKS-1];
    logic [BATCH_ID_WIDTH-1:0] batch_id [0:BANKS-1];
    logic [BYTE_COUNT_WIDTH-1:0] byte_count [0:BANKS-1];
    integer allocation_bank;
    integer ready_bank;
    integer state_bank;
    integer reset_bank;
    integer alloc_selection;
    integer ce_selection;
    logic alloc_found;
    logic ce_found;
    logic [BATCH_ID_WIDTH-1:0] oldest_ready_batch;

    always_comb begin
        alloc_found = 1'b0;
        alloc_selection = 0;
        for (allocation_bank = 0;
             allocation_bank < BANKS;
             allocation_bank = allocation_bank + 1) begin
            if (!alloc_found && state[allocation_bank] == STATE_FREE) begin
                alloc_found = 1'b1;
                alloc_selection = allocation_bank;
            end
        end
        alloc_ready = alloc_found && (alloc_bytes != 0) &&
                      (alloc_bytes <= BANK_BYTES_VALUE);
        alloc_bank_id = BANK_ID_WIDTH'(alloc_selection);

        ce_found = 1'b0;
        ce_selection = 0;
        oldest_ready_batch = '1;
        for (ready_bank = 0; ready_bank < BANKS; ready_bank = ready_bank + 1) begin
            if ((state[ready_bank] == STATE_READY) &&
                (!ce_found || batch_id[ready_bank] < oldest_ready_batch)) begin
                ce_found = 1'b1;
                ce_selection = ready_bank;
                oldest_ready_batch = batch_id[ready_bank];
            end
        end
        ce_valid = ce_found;
        ce_bank_id = BANK_ID_WIDTH'(ce_selection);
        ce_batch_id = batch_id[ce_selection];
        ce_bytes = byte_count[ce_selection];

        for (state_bank = 0; state_bank < BANKS; state_bank = state_bank + 1) begin
            bank_state[state_bank*3 +: 3] = state[state_bank];
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            protocol_error <= 1'b0;
            for (reset_bank = 0; reset_bank < BANKS; reset_bank = reset_bank + 1) begin
                state[reset_bank] <= STATE_FREE;
                batch_id[reset_bank] <= '0;
                byte_count[reset_bank] <= '0;
            end
        end else begin
            if (alloc_valid && alloc_ready) begin
                state[alloc_selection] <= STATE_AE_ACTIVE;
                batch_id[alloc_selection] <= alloc_batch_id;
                byte_count[alloc_selection] <= alloc_bytes;
            end else if (alloc_valid &&
                         (!alloc_found || alloc_bytes == 0 ||
                          alloc_bytes > BANK_BYTES_VALUE)) begin
                protocol_error <= protocol_error |
                                  (alloc_bytes == 0 || alloc_bytes > BANK_BYTES_VALUE);
            end

            if (ae_done_valid) begin
                if (int'(ae_done_bank_id) >= BANKS ||
                    state[ae_done_bank_id] != STATE_AE_ACTIVE) begin
                    protocol_error <= 1'b1;
                end else begin
                    state[ae_done_bank_id] <= STATE_READY;
                end
            end

            if (ce_valid && ce_ready) begin
                state[ce_selection] <= STATE_CE_ACTIVE;
            end

            if (ce_done_valid) begin
                if (int'(ce_done_bank_id) >= BANKS ||
                    state[ce_done_bank_id] != STATE_CE_ACTIVE) begin
                    protocol_error <= 1'b1;
                end else begin
                    state[ce_done_bank_id] <= STATE_FREE;
                    batch_id[ce_done_bank_id] <= '0;
                    byte_count[ce_done_bank_id] <= '0;
                end
            end
        end
    end
endmodule

`default_nettype wire
