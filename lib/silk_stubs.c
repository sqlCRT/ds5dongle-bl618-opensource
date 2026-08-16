/* Minimal SILK stubs — we run CELT-only (48kHz, RESTRICTED_LOWDELAY).
 * opus_encoder.c / opus_decoder.c reference these SILK pipeline functions
 * at link time, but they are never reached at runtime.
 * Utility functions (lin2log, biquad, etc.) are kept as real implementations. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "silk/API.h"

opus_int silk_Get_Encoder_Size(opus_int *encSizeBytes, opus_int channels) {
    (void)channels;
    *encSizeBytes = 4;
    return SILK_NO_ERROR;
}

opus_int silk_InitEncoder(void *encState, int channels, int arch,
                          silk_EncControlStruct *encStatus) {
    (void)encState; (void)channels; (void)arch; (void)encStatus;
    return SILK_NO_ERROR;
}

opus_int silk_Encode(void *encState, silk_EncControlStruct *encControl,
                     const opus_res *samplesIn, opus_int nSamplesIn,
                     ec_enc *psRangeEnc, opus_int32 *nBytesOut,
                     const opus_int prefillFlag, int activity) {
    (void)encState; (void)encControl; (void)samplesIn;
    (void)nSamplesIn; (void)psRangeEnc; (void)prefillFlag; (void)activity;
    *nBytesOut = 0;
    return SILK_NO_ERROR;
}

opus_int silk_Get_Decoder_Size(opus_int *decSizeBytes) {
    *decSizeBytes = 4;
    return SILK_NO_ERROR;
}

opus_int silk_InitDecoder(void *decState) {
    (void)decState;
    return SILK_NO_ERROR;
}

opus_int silk_ResetDecoder(void *decState) {
    (void)decState;
    return SILK_NO_ERROR;
}

opus_int silk_Decode(void *decState, silk_DecControlStruct *decControl,
                     opus_int lostFlag, opus_int newPacketFlag,
                     ec_dec *psRangeDec, opus_res *samplesOut,
                     opus_int32 *nSamplesOut,
#ifdef ENABLE_DEEP_PLC
                     LPCNetPLCState *lpcnet,
#endif
                     int arch) {
    (void)decState; (void)decControl; (void)lostFlag; (void)newPacketFlag;
    (void)psRangeDec; (void)samplesOut; (void)arch;
#ifdef ENABLE_DEEP_PLC
    (void)lpcnet;
#endif
    *nSamplesOut = 0;
    return SILK_NO_ERROR;
}

void silk_QueryEncoder(const void *encState, silk_EncControlStruct *encStatus) {
    (void)encState; (void)encStatus;
}

opus_int silk_LoadOSCEModels(void *decState, const unsigned char *data, int len) {
    (void)decState; (void)data; (void)len;
    return SILK_NO_ERROR;
}
