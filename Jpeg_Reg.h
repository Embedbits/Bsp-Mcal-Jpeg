/**
 * \author Mr.Nobody
 * \file Jpeg_Reg.h
 * \ingroup Jpeg
 * \brief Jpeg module Reg component header file.
 *
 * This component contains functionality for register operations. It is private
 * to the module (the register access is not shared with other modules).
 *
 */

#ifndef JPEG_REG_JPEG_REG_H
#define JPEG_REG_JPEG_REG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================= INCLUDES =================================== */
#include "Jpeg.h"                           /* Module internal types          */
#include "Jpeg_Types.h"                     /* Module types definition        */
/* ============================= TYPEDEFS =================================== */

/** \brief List of JPEG registers accessed as a single 32-bit value */
typedef enum
{
    JPEG_REG_CONFR0 = 0u, /**< JPEG codec control register (START)                    */
    JPEG_REG_CONFR1     , /**< JPEG codec configuration register 1 (mode, color space) */
    JPEG_REG_CONFR2     , /**< JPEG codec configuration register 2 (count of the MCUs) */
    JPEG_REG_CONFR3     , /**< JPEG codec configuration register 3 (width)             */
    JPEG_REG_CONFR4     , /**< JPEG codec configuration register 4 (component 0)       */
    JPEG_REG_CONFR5     , /**< JPEG codec configuration register 5 (component 1)       */
    JPEG_REG_CONFR6     , /**< JPEG codec configuration register 6 (component 2)       */
    JPEG_REG_CONFR7     , /**< JPEG codec configuration register 7 (component 3)       */
    JPEG_REG_CR         , /**< JPEG control register                                   */
    JPEG_REG_SR         , /**< JPEG status register                                    */
    JPEG_REG_CFR        , /**< JPEG clear flag register                                */
    JPEG_REG_DIR        , /**< JPEG data input register                                */
    JPEG_REG_DOR        , /**< JPEG data output register                               */
    JPEG_REG_CNT          /**< Count of available JPEG registers                       */
}   jpeg_RegId_t;


/**
 * \brief Clear bit of the end of the conversion flag in JPEG_CFR (bit 5, the same position as JPEG_SR.EOCF).
 *
 * The CMSIS headers of ST (stm32h5e4xx.h, stm32h5e5xx.h, stm32h5f4xx.h, stm32h5f5xx.h) define JPEG_CFR_CEOCF_Pos as 4,
 * which is wrong (RM0517 41.5.8: CEOCF = bit 5). The CMSIS macros JPEG_CFR_* must not be used.
 */
#define JPEG_REG_CFR_CEOCF                  ( 0x20u )

/**
 * \brief Clear bit of the header parsing done flag in JPEG_CFR (bit 6, the same position as JPEG_SR.HPDF).
 *
 * The CMSIS headers of ST define JPEG_CFR_CHPDF_Pos as 5, which is wrong (RM0517 41.5.8: CHPDF = bit 6).
 */
#define JPEG_REG_CFR_CHPDF                  ( 0x40u )


/** \brief Components of an image (the registers JPEG_CONFR4 to JPEG_CONFR7) */
#define JPEG_REG_COMPONENT_CNT              ( 4u )


/** \brief Parameters of one color component (JPEG_CONFR4 to JPEG_CONFR7), raw register field values */
typedef struct
{
    uint32_t HorizontalFactor; /**< JPEG_CONFRx.HSF: horizontal sampling factor                             */
    uint32_t VerticalFactor;   /**< JPEG_CONFRx.VSF: vertical sampling factor                               */
    uint32_t BlockCnt;         /**< JPEG_CONFRx.NB: count of the 8 x 8 blocks of the component in the MCU - 1 */
    uint32_t QuantTable;       /**< JPEG_CONFRx.QT: quantization table                                      */
    uint32_t HuffmanAc;        /**< JPEG_CONFRx.HA: AC Huffman table                                        */
    uint32_t HuffmanDc;        /**< JPEG_CONFRx.HD: DC Huffman table                                        */
}   jpeg_RegComponent_t;


/** \brief Parameters of the image (JPEG_CONFR1 to JPEG_CONFR7), raw register field values */
typedef struct
{
    uint32_t            Decode;     /**< JPEG_CONFR1.DE: 0 encoder, 1 decoder                                   */
    uint32_t            Header;     /**< JPEG_CONFR1.HDR: header processing                                     */
    uint32_t            ColorSpace; /**< JPEG_CONFR1.COLORSPACE: count of the quantization tables - 1            */
    uint32_t            Components; /**< JPEG_CONFR1.NF: count of the color components - 1                      */
    uint32_t            Scan;       /**< JPEG_CONFR1.NS: count of the components of the scan - 1                */
    uint32_t            Height;     /**< JPEG_CONFR1.YSIZE: count of the lines                                  */
    uint32_t            Width;      /**< JPEG_CONFR3.XSIZE: count of the pixels per line                        */
    uint32_t            McuCnt;     /**< JPEG_CONFR2.NMCU: count of the MCUs - 1                                */
    jpeg_RegComponent_t Component[ JPEG_REG_COMPONENT_CNT ]; /**< Color components                               */
}   jpeg_RegImageConfig_t;

/* ========================= SYMBOLIC CONSTANTS ============================= */

/** \brief Full register mask, used to write / read back a whole 32-bit register at once */
#define JPEG_REG_FULL_MASK                  ( 0xFFFFFFFFu )

/** \brief Count of the 32-bit words of the DHT memory (the Huffman tables of the generated header) */
#define JPEG_REG_DHT_WORD_CNT               ( 103u )

/** \brief Count of the 32-bit words of a quantization memory (64 coefficients of 8 bit) */
#define JPEG_REG_QUANT_WORD_CNT             ( 16u )

/** \brief Count of the 32-bit words of the DC Huffman encoder memory of one table */
#define JPEG_REG_HUFFENC_DC_WORD_CNT        ( 8u )

/** \brief Count of the 32-bit words of the AC Huffman encoder memory of one table */
#define JPEG_REG_HUFFENC_AC_WORD_CNT        ( 88u )

/* ========================= EXPORTED MACROS ================================ */

/* ========================= EXPORTED VARIABLES ============================= */

/* ======================== EXPORTED FUNCTIONS ============================== */

/* ---------------------------- Generic accessors --------------------------- */

void                        Jpeg_Set_RegBit             ( jpeg_RegId_t regId, uint32_t bitMask );
void                        Jpeg_Reset_RegBit           ( jpeg_RegId_t regId, uint32_t bitMask );
uint32_t                    Jpeg_Get_RegBit             ( jpeg_RegId_t regId, uint32_t bitMask );

void                        Jpeg_Set_RegVal             ( jpeg_RegId_t regId, uint32_t regMask, uint32_t regValue );
uint32_t                    Jpeg_Get_RegVal             ( jpeg_RegId_t regId, uint32_t regMask );

jpeg_RequestState_t         Jpeg_Reg_Wait_RegVal        ( jpeg_RegId_t regId, uint32_t regMask, uint32_t regValue );

/* -------------------------- Register level protocols ---------------------- */

jpeg_RequestState_t         Jpeg_Reg_Enable_Core        ( void );
void                        Jpeg_Reg_Disable_Core       ( void );
jpeg_RequestState_t         Jpeg_Reg_Reset_Codec        ( void );
void                        Jpeg_Reg_Set_ImageConfig    ( const jpeg_RegImageConfig_t * const imageConfig );
void                        Jpeg_Reg_Get_ImageConfig    ( jpeg_RegImageConfig_t * const imageConfig );
void                        Jpeg_Reg_Start_Codec        ( void );
jpeg_RequestState_t         Jpeg_Reg_Stop_Codec         ( void );
void                        Jpeg_Reg_Clear_Flags        ( uint32_t clearMask );
void                        Jpeg_Reg_Write_Word         ( uint32_t dataWord );
uint32_t                    Jpeg_Reg_Read_Word          ( void );

/* ------------------------------ Table memories ---------------------------- */

void                        Jpeg_Reg_Set_QuantWords     ( jpeg_QuantId_t tableId,
                                                          const uint32_t * const wordList );
uint32_t                    Jpeg_Reg_Get_QuantWord      ( jpeg_QuantId_t tableId, uint32_t wordIdx );
void                        Jpeg_Reg_Set_HuffEncWords   ( jpeg_HuffmanId_t tableId,
                                                          const uint32_t * const wordList,
                                                          uint32_t wordCnt );
void                        Jpeg_Reg_Set_DhtWords       ( const uint32_t * const wordList );

/* ------------------------ Addresses for the DMA --------------------------- */

uint32_t                    Jpeg_Reg_Get_DirAddr        ( void );
uint32_t                    Jpeg_Reg_Get_DorAddr        ( void );

#ifdef __cplusplus
}
#endif

#endif /* JPEG_REG_JPEG_REG_H */
