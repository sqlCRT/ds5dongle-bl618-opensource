set(OPUS_DIR ${CMAKE_CURRENT_LIST_DIR}/opus)

set(OPUS_SOURCES
    # Core API
    ${OPUS_DIR}/src/opus.c
    ${OPUS_DIR}/src/opus_encoder.c
    ${OPUS_DIR}/src/opus_decoder.c
    ${OPUS_DIR}/src/extensions.c
    ${OPUS_DIR}/src/repacketizer.c

    # CELT (both encode and decode)
    ${OPUS_DIR}/celt/bands.c
    ${OPUS_DIR}/celt/celt.c
    ${OPUS_DIR}/celt/celt_encoder.c
    ${OPUS_DIR}/celt/celt_decoder.c
    ${OPUS_DIR}/celt/cwrs.c
    ${OPUS_DIR}/celt/entcode.c
    ${OPUS_DIR}/celt/entdec.c
    ${OPUS_DIR}/celt/entenc.c
    ${OPUS_DIR}/celt/kiss_fft.c
    ${OPUS_DIR}/celt/laplace.c
    ${OPUS_DIR}/celt/mathops.c
    ${OPUS_DIR}/celt/mdct.c
    ${OPUS_DIR}/celt/modes.c
    ${OPUS_DIR}/celt/pitch.c
    ${OPUS_DIR}/celt/celt_lpc.c
    ${OPUS_DIR}/celt/quant_bands.c
    ${OPUS_DIR}/celt/rate.c
    ${OPUS_DIR}/celt/vq.c

    # SILK utility functions used by opus_encoder/decoder even in CELT-only mode
    ${OPUS_DIR}/silk/lin2log.c
    ${OPUS_DIR}/silk/log2lin.c
    ${OPUS_DIR}/silk/biquad_alt.c
    ${OPUS_DIR}/silk/LPC_inv_pred_gain.c
    ${OPUS_DIR}/silk/sum_sqr_shift.c
    ${OPUS_DIR}/silk/inner_prod_aligned.c
    ${OPUS_DIR}/silk/bwexpander.c
    ${OPUS_DIR}/silk/bwexpander_32.c
    ${OPUS_DIR}/silk/debug.c
    ${OPUS_DIR}/silk/sort.c
    ${OPUS_DIR}/silk/sigm_Q15.c
    ${OPUS_DIR}/silk/table_LSF_cos.c
    ${OPUS_DIR}/silk/tables_other.c
    ${OPUS_DIR}/silk/stereo_decode_pred.c
    ${OPUS_DIR}/silk/stereo_encode_pred.c
    ${OPUS_DIR}/silk/stereo_find_predictor.c
    ${OPUS_DIR}/silk/stereo_quant_pred.c
    ${OPUS_DIR}/silk/stereo_LR_to_MS.c
    ${OPUS_DIR}/silk/stereo_MS_to_LR.c

    # SILK API stubs (heavy encoder/decoder pipeline removed)
    ${CMAKE_CURRENT_LIST_DIR}/silk_stubs.c
)
