`timescale 1ns/1ps
`default_nettype none

module hygcn_request_arbiter #(
    parameter int REQUEST_CLASSES = 4,
    parameter int BATCH_ID_WIDTH = 16,
    parameter int SEQUENCE_WIDTH = 32,
    parameter int ADDRESS_WIDTH = 64,
    parameter int LENGTH_WIDTH = 16,
    parameter int CLASS_WIDTH = (REQUEST_CLASSES <= 2) ? 1 : $clog2(REQUEST_CLASSES)
) (
    input  logic                                      mode_batch_class,
    input  logic [REQUEST_CLASSES-1:0]                s_valid,
    output logic [REQUEST_CLASSES-1:0]                s_ready,
    input  logic [REQUEST_CLASSES*BATCH_ID_WIDTH-1:0] s_batch_id,
    input  logic [REQUEST_CLASSES*SEQUENCE_WIDTH-1:0] s_sequence,
    input  logic [REQUEST_CLASSES*ADDRESS_WIDTH-1:0]  s_address,
    input  logic [REQUEST_CLASSES*LENGTH_WIDTH-1:0]   s_length,
    input  logic [REQUEST_CLASSES-1:0]                s_write,

    output logic                                      m_valid,
    input  logic                                      m_ready,
    output logic [CLASS_WIDTH-1:0]                    m_class,
    output logic [BATCH_ID_WIDTH-1:0]                 m_batch_id,
    output logic [SEQUENCE_WIDTH-1:0]                 m_sequence,
    output logic [ADDRESS_WIDTH-1:0]                  m_address,
    output logic [LENGTH_WIDTH-1:0]                   m_length,
    output logic                                      m_write
);
    integer request_index;
    integer selected_index;
    logic selected_valid;
    logic [BATCH_ID_WIDTH-1:0] candidate_batch;
    logic [SEQUENCE_WIDTH-1:0] candidate_sequence;

    always_comb begin
        selected_valid = 1'b0;
        selected_index = 0;
        candidate_batch = '1;
        candidate_sequence = '1;

        for (request_index = 0; request_index < REQUEST_CLASSES; request_index = request_index + 1) begin
            if (s_valid[request_index]) begin
                if (!selected_valid ||
                    (mode_batch_class &&
                     ((s_batch_id[request_index*BATCH_ID_WIDTH +: BATCH_ID_WIDTH] < candidate_batch) ||
                      ((s_batch_id[request_index*BATCH_ID_WIDTH +: BATCH_ID_WIDTH] == candidate_batch) &&
                       (request_index < selected_index)))) ||
                    (!mode_batch_class &&
                     ((s_sequence[request_index*SEQUENCE_WIDTH +: SEQUENCE_WIDTH] < candidate_sequence) ||
                      ((s_sequence[request_index*SEQUENCE_WIDTH +: SEQUENCE_WIDTH] == candidate_sequence) &&
                       (request_index < selected_index))))) begin
                    selected_valid = 1'b1;
                    selected_index = request_index;
                    candidate_batch = s_batch_id[request_index*BATCH_ID_WIDTH +: BATCH_ID_WIDTH];
                    candidate_sequence = s_sequence[request_index*SEQUENCE_WIDTH +: SEQUENCE_WIDTH];
                end
            end
        end

        s_ready = '0;
        if (selected_valid && m_ready) begin
            s_ready[selected_index] = 1'b1;
        end

        m_valid = selected_valid;
        m_class = CLASS_WIDTH'(selected_index);
        m_batch_id = s_batch_id[selected_index*BATCH_ID_WIDTH +: BATCH_ID_WIDTH];
        m_sequence = s_sequence[selected_index*SEQUENCE_WIDTH +: SEQUENCE_WIDTH];
        m_address = s_address[selected_index*ADDRESS_WIDTH +: ADDRESS_WIDTH];
        m_length = s_length[selected_index*LENGTH_WIDTH +: LENGTH_WIDTH];
        m_write = s_write[selected_index];
    end
endmodule

`default_nettype wire
