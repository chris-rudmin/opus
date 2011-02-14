/***********************************************************************
Copyright (c) 2006-2011, Skype Limited. All rights reserved. 
Redistribution and use in source and binary forms, with or without 
modification, (subject to the limitations in the disclaimer below) 
are permitted provided that the following conditions are met:
- Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright 
notice, this list of conditions and the following disclaimer in the 
documentation and/or other materials provided with the distribution.
- Neither the name of Skype Limited, nor the names of specific 
contributors, may be used to endorse or promote products derived from 
this software without specific prior written permission.
NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED 
BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
CONTRIBUTORS ''AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND 
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE 
COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, 
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF 
USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON 
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
***********************************************************************/

#include "SKP_Silk_main_FLP.h"


void SKP_Silk_find_pred_coefs_FLP(
    SKP_Silk_encoder_state_FLP      *psEnc,             /* I/O  Encoder state FLP                       */
    SKP_Silk_encoder_control_FLP    *psEncCtrl,         /* I/O  Encoder control FLP                     */
    const SKP_float                 res_pitch[],        /* I    Residual from pitch analysis            */
    const SKP_float                 x[]                 /* I    Speech signal                           */
)
{
    SKP_int         i;
    SKP_float       WLTP[ MAX_NB_SUBFR * LTP_ORDER * LTP_ORDER ];
    SKP_float       invGains[ MAX_NB_SUBFR ], Wght[ MAX_NB_SUBFR ];
    SKP_float       NLSF[ MAX_LPC_ORDER ];
    const SKP_float *x_ptr;
    SKP_float       *x_pre_ptr, LPC_in_pre[ MAX_NB_SUBFR * MAX_LPC_ORDER + MAX_FRAME_LENGTH ];

    /* Weighting for weighted least squares */
    for( i = 0; i < psEnc->sCmn.nb_subfr; i++ ) {
        SKP_assert( psEncCtrl->Gains[ i ] > 0.0f );
        invGains[ i ] = 1.0f / psEncCtrl->Gains[ i ];
        Wght[ i ]     = invGains[ i ] * invGains[ i ];
    }

    if( psEnc->sCmn.indices.signalType == TYPE_VOICED ) {
        /**********/
        /* VOICED */
        /**********/
        SKP_assert( psEnc->sCmn.ltp_mem_length - psEnc->sCmn.predictLPCOrder >= psEncCtrl->pitchL[ 0 ] + LTP_ORDER / 2 );

        /* LTP analysis */
        SKP_Silk_find_LTP_FLP( psEncCtrl->LTPCoef, WLTP, &psEncCtrl->LTPredCodGain, res_pitch, 
            psEncCtrl->pitchL, Wght, psEnc->sCmn.subfr_length, psEnc->sCmn.nb_subfr, psEnc->sCmn.ltp_mem_length );

#ifdef SAVE_ALL_INTERNAL_DATA
		DEBUG_STORE_DATA( ltp_gains.dat, psEncCtrl->LTPCoef, sizeof( psEncCtrl->LTPCoef ) );
		DEBUG_STORE_DATA( ltp_weights.dat, WLTP, sizeof( WLTP ) );
#endif

        /* Quantize LTP gain parameters */
        SKP_Silk_quant_LTP_gains_FLP( psEncCtrl->LTPCoef, psEnc->sCmn.indices.LTPIndex, &psEnc->sCmn.indices.PERIndex, 
            WLTP, psEnc->sCmn.mu_LTP_Q9, psEnc->sCmn.LTPQuantLowComplexity , psEnc->sCmn.nb_subfr );

        /* Control LTP scaling */
        SKP_Silk_LTP_scale_ctrl_FLP( psEnc, psEncCtrl );

        /* Create LTP residual */
        SKP_Silk_LTP_analysis_filter_FLP( LPC_in_pre, psEnc->x_buf + psEnc->sCmn.ltp_mem_length - psEnc->sCmn.predictLPCOrder, 
            psEncCtrl->LTPCoef, psEncCtrl->pitchL, invGains, psEnc->sCmn.subfr_length, psEnc->sCmn.nb_subfr, psEnc->sCmn.predictLPCOrder );

    } else {
        /************/
        /* UNVOICED */
        /************/
        /* Create signal with prepended subframes, scaled by inverse gains */
        x_ptr     = x - psEnc->sCmn.predictLPCOrder;
        x_pre_ptr = LPC_in_pre;
        for( i = 0; i < psEnc->sCmn.nb_subfr; i++ ) {
            SKP_Silk_scale_copy_vector_FLP( x_pre_ptr, x_ptr, invGains[ i ], 
                psEnc->sCmn.subfr_length + psEnc->sCmn.predictLPCOrder );
            x_pre_ptr += psEnc->sCmn.subfr_length + psEnc->sCmn.predictLPCOrder;
            x_ptr     += psEnc->sCmn.subfr_length;
        }

        SKP_memset( psEncCtrl->LTPCoef, 0, psEnc->sCmn.nb_subfr * LTP_ORDER * sizeof( SKP_float ) );
        psEncCtrl->LTPredCodGain = 0.0f;
    }

    /* LPC_in_pre contains the LTP-filtered input for voiced, and the unfiltered input for unvoiced */
    SKP_Silk_find_LPC_FLP( NLSF, &psEnc->sCmn.indices.NLSFInterpCoef_Q2, psEnc->sPred.prev_NLSFq, 
        psEnc->sCmn.useInterpolatedNLSFs * ( 1 - psEnc->sCmn.first_frame_after_reset ), psEnc->sCmn.predictLPCOrder, 
        LPC_in_pre, psEnc->sCmn.subfr_length + psEnc->sCmn.predictLPCOrder, psEnc->sCmn.nb_subfr );


    /* Quantize LSFs */
TIC(LSF_quant);
    SKP_Silk_process_NLSFs_FLP( psEnc, psEncCtrl, NLSF );
TOC(LSF_quant);

    /* Calculate residual energy using quantized LPC coefficients */
    SKP_Silk_residual_energy_FLP( psEncCtrl->ResNrg, LPC_in_pre, psEncCtrl->PredCoef, psEncCtrl->Gains,
        psEnc->sCmn.subfr_length, psEnc->sCmn.nb_subfr, psEnc->sCmn.predictLPCOrder );

    /* Copy to prediction struct for use in next frame for fluctuation reduction */
    SKP_memcpy( psEnc->sPred.prev_NLSFq, NLSF, psEnc->sCmn.predictLPCOrder * sizeof( SKP_float ) );
}

