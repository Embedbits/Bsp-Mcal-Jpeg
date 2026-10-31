/**
 * \author Mr.Nobody
 * \file Jpeg_Port.h
 * \ingroup Jpeg
 * \brief Jpeg module public functionality
 *
 * This file contains all available public functionality, any other files shall
 * not used outside of the module.
 *
 */

#ifndef JPEG_JPEG_PORT_H
#define JPEG_JPEG_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== INCLUDES ================================== */
#include "Jpeg_Types.h"                     /* Module types definition        */
/* ============================== TYPEDEFS ================================== */

/* ========================== SYMBOLIC CONSTANTS ============================ */

/* ========================== EXPORTED MACROS =============================== */

/* ========================== EXPORTED VARIABLES ============================ */

/* ========================= EXPORTED FUNCTIONS ============================= */

jpeg_ModuleVersion_t        Jpeg_Get_ModuleVersion          ( void );

jpeg_RequestState_t         Jpeg_Init                       ( const jpeg_Config_t * const jpegConfig );
jpeg_RequestState_t         Jpeg_Deinit                     ( void );
void                        Jpeg_Task                       ( void );

jpeg_RequestState_t         Jpeg_Get_DefaultConfig          ( jpeg_Config_t * const jpegConfig );

/* --------------------------------- Jobs ----------------------------------- */

jpeg_RequestState_t         Jpeg_Set_EncodeStart            ( const jpeg_ImageConfig_t * const imageConfig );
jpeg_RequestState_t         Jpeg_Set_DecodeStart            ( void );
jpeg_RequestState_t         Jpeg_Set_Abort                  ( void );
jpeg_RequestState_t         Jpeg_Set_Pause                  ( jpeg_Stream_t stream );
jpeg_RequestState_t         Jpeg_Set_Resume                 ( jpeg_Stream_t stream );
jpeg_RequestState_t         Jpeg_Get_State                  ( jpeg_State_t * const jpegState );

/* -------------------------------- Streams --------------------------------- */

jpeg_RequestState_t         Jpeg_Set_InputBuffer            ( const jpeg_Data_t * const buffer, jpeg_Size_t size );
jpeg_RequestState_t         Jpeg_Set_OutputBuffer           ( jpeg_Data_t * const buffer, jpeg_Size_t size );
jpeg_RequestState_t         Jpeg_Get_OutputLength           ( jpeg_Size_t * const length );

/* --------------------------- Image and tables ----------------------------- */

jpeg_RequestState_t         Jpeg_Get_ImageInfo              ( jpeg_ImageInfo_t * const imageInfo );
jpeg_RequestState_t         Jpeg_Get_RawSize                ( const jpeg_ImageConfig_t * const imageConfig,
                                                              jpeg_Size_t * const rawSize );
jpeg_RequestState_t         Jpeg_Set_Quality                ( jpeg_Quality_t quality );
jpeg_RequestState_t         Jpeg_Get_Quality                ( jpeg_Quality_t * const quality );
jpeg_RequestState_t         Jpeg_Set_QuantizationTable      ( jpeg_QuantId_t tableId,
                                                              const jpeg_QuantTable_t * const table );
jpeg_RequestState_t         Jpeg_Set_HuffmanTable           ( jpeg_HuffmanId_t tableId,
                                                              const jpeg_HuffmanTable_t * const table );
jpeg_RequestState_t         Jpeg_Set_HeaderMode             ( jpeg_HeaderMode_t headerMode );

/* ------------------------------- Interrupts ------------------------------- */

jpeg_RequestState_t         Jpeg_Set_IrqPriority            ( jpeg_IrqPrio_t irqPrio );
jpeg_RequestState_t         Jpeg_Get_IrqPriority            ( jpeg_IrqPrio_t * const irqPrio );

#ifdef __cplusplus
}
#endif

#endif /* JPEG_JPEG_PORT_H */
