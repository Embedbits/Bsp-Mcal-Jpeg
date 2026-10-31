/**
 * \author Mr.Nobody
 * \file Jpeg_Reg.c
 * \ingroup Jpeg
 * \brief Jpeg module Reg component functionality.
 *
 * Register access of the module: generic accessors (bounds checked register ID) and the register level
 * protocols of RM0517 (JPEG codec chapter): the enable of the core, the stop of the codec with the flush of the
 * FIFOs, the image configuration, the data registers and the table memories (quantization, Huffman encoder, DHT).
 * The registers are written only here, the masks come from the CMSIS device header.
 *
 * The JPEG codec exists on the STM32H5E and STM32H5F lines. On the other lines the register list is not defined
 * and the component is empty (the public functions of the module return an error there).
 *
 * The registers of the codec are accessible only while JPEG_CR.JCEN is set, the clear of JCEN resets them. The
 * flags JPEG_SR.EOCF and JPEG_SR.HPDF have to be cleared before the clear of JCEN and of JPEG_CONFR1.HDR.
 *
 */
/* ============================== INCLUDES ================================== */
#include "Jpeg_Reg.h"                       /* Self include                   */
#include "Jpeg_Types.h"                     /* Module types definitions       */
#include "Stm32.h"                          /* JPEG registers definition      */
/* ============================== TYPEDEFS ================================== */

#if defined(JPEG)

/** \brief Structure type used to store physical address of JPEG registers.
 * This array is used to reduce size of configuration array. With this style
 * the registers are referenced through enumeration.
 *
 */
typedef struct
{
    jpeg_RegId_t        RegId;   /**< Peripheral register ID      */
    volatile uint32_t * RegAddr; /**< Peripheral register address */
}   jpeg_RegList_t;

/* ======================== FORWARD DECLARATIONS ============================ */

/* ========================== SYMBOLIC CONSTANTS ============================ */

/** \brief Register value without any bit set (masked register value of an inactive bit field) */
#define JPEG_REG_BITS_CLEAR              ( 0u )

/** \brief Count of read-back iterations of a register waiting for a hardware reaction */
#define JPEG_REG_TIMEOUT_CNT             ( 0x1000u )

/** \brief Interrupt enable bits and DMA request enable bits of JPEG_CR */
#define JPEG_REG_CR_REQUEST_MASK         ( JPEG_CR_IFTIE   | JPEG_CR_IFNFIE | JPEG_CR_OFTIE | JPEG_CR_OFNEIE | \
                                           JPEG_CR_EOCIE   | JPEG_CR_HPDIE  | JPEG_CR_IDMAEN | JPEG_CR_ODMAEN   )

/** \brief Both clear bits of JPEG_CFR */
#define JPEG_REG_CFR_ALL_MASK            ( JPEG_REG_CFR_CEOCF | JPEG_REG_CFR_CHPDF )

/** \brief Count of the entries of the Huffman encoder memories */
#define JPEG_REG_HUFFENC_TABLE_CNT       ( 4u )

/* The registers JPEG_CONFR4 to JPEG_CONFR7 share the position and the width of the fields of a component */
_Static_assert( ( JPEG_CONFR4_HSF_Pos == JPEG_CONFR7_HSF_Pos ) &&
                ( JPEG_CONFR4_VSF_Pos == JPEG_CONFR7_VSF_Pos ) &&
                ( JPEG_CONFR4_NB_Pos  == JPEG_CONFR7_NB_Pos  ) &&
                ( JPEG_CONFR4_QT_Pos  == JPEG_CONFR7_QT_Pos  ) &&
                ( JPEG_CONFR4_HA_Pos  == JPEG_CONFR7_HA_Pos  ) &&
                ( JPEG_CONFR4_HD_Pos  == JPEG_CONFR7_HD_Pos  ) &&
                ( JPEG_CONFR4_HSF_Pos == JPEG_CONFR5_HSF_Pos ) &&
                ( JPEG_CONFR4_HSF_Pos == JPEG_CONFR6_HSF_Pos ),
                "Jpeg_Reg: component configuration registers have different field positions." );

/* =============================== MACROS =================================== */

/* ========================== EXPORTED VARIABLES ============================ */

/* =========================== LOCAL VARIABLES ============================== */

/** \brief JPEG registers configuration structure
 *
 * This structure is used to store addresses of JPEG registers.
 * It is used to reduce code size while referring to JPEG registers.
 */
static const jpeg_RegList_t             jpeg_RegList[] =
{
    { .RegId = JPEG_REG_CONFR0, .RegAddr = &(JPEG->CONFR0) }, /**< JPEG codec control register              */
    { .RegId = JPEG_REG_CONFR1, .RegAddr = &(JPEG->CONFR1) }, /**< JPEG codec configuration register 1      */
    { .RegId = JPEG_REG_CONFR2, .RegAddr = &(JPEG->CONFR2) }, /**< JPEG codec configuration register 2      */
    { .RegId = JPEG_REG_CONFR3, .RegAddr = &(JPEG->CONFR3) }, /**< JPEG codec configuration register 3      */
    { .RegId = JPEG_REG_CONFR4, .RegAddr = &(JPEG->CONFR4) }, /**< JPEG codec configuration register 4      */
    { .RegId = JPEG_REG_CONFR5, .RegAddr = &(JPEG->CONFR5) }, /**< JPEG codec configuration register 5      */
    { .RegId = JPEG_REG_CONFR6, .RegAddr = &(JPEG->CONFR6) }, /**< JPEG codec configuration register 6      */
    { .RegId = JPEG_REG_CONFR7, .RegAddr = &(JPEG->CONFR7) }, /**< JPEG codec configuration register 7      */
    { .RegId = JPEG_REG_CR    , .RegAddr = &(JPEG->CR    ) }, /**< JPEG control register                    */
    { .RegId = JPEG_REG_SR    , .RegAddr = &(JPEG->SR    ) }, /**< JPEG status register                     */
    { .RegId = JPEG_REG_CFR   , .RegAddr = &(JPEG->CFR   ) }, /**< JPEG clear flag register                 */
    { .RegId = JPEG_REG_DIR   , .RegAddr = &(JPEG->DIR   ) }, /**< JPEG data input register                 */
    { .RegId = JPEG_REG_DOR   , .RegAddr = &(JPEG->DOR   ) }, /**< JPEG data output register                */
};

_Static_assert( (sizeof(jpeg_RegList) / sizeof(jpeg_RegList_t)) == JPEG_REG_CNT,
                "Jpeg_Reg: jpeg_RegList has incorrect size." );

/** \brief Registers of the components of an image, indexed by the component */
static const jpeg_RegId_t jpeg_ComponentRegLut[ JPEG_REG_COMPONENT_CNT ] =
{
    JPEG_REG_CONFR4,
    JPEG_REG_CONFR5,
    JPEG_REG_CONFR6,
    JPEG_REG_CONFR7,
};

/** \brief Quantization memories, indexed by \ref jpeg_QuantId_t */
static volatile uint32_t * const jpeg_QuantMemLut[ JPEG_QUANT_TABLE_CNT ] =
{
    [JPEG_QUANT_TABLE_0] = JPEG->QMEM0,
    [JPEG_QUANT_TABLE_1] = JPEG->QMEM1,
    [JPEG_QUANT_TABLE_2] = JPEG->QMEM2,
    [JPEG_QUANT_TABLE_3] = JPEG->QMEM3,
};

/** \brief Huffman encoder memories, indexed by \ref jpeg_HuffmanId_t */
static volatile uint32_t * const jpeg_HuffEncMemLut[ JPEG_HUFFMAN_CNT ] =
{
    [JPEG_HUFFMAN_DC_0] = JPEG->HUFFENC_DC0,
    [JPEG_HUFFMAN_DC_1] = JPEG->HUFFENC_DC1,
    [JPEG_HUFFMAN_AC_0] = JPEG->HUFFENC_AC0,
    [JPEG_HUFFMAN_AC_1] = JPEG->HUFFENC_AC1,
};

/** \brief Count of the words of the Huffman encoder memories, indexed by \ref jpeg_HuffmanId_t */
static const uint32_t jpeg_HuffEncWordCntLut[ JPEG_HUFFMAN_CNT ] =
{
    [JPEG_HUFFMAN_DC_0] = JPEG_REG_HUFFENC_DC_WORD_CNT,
    [JPEG_HUFFMAN_DC_1] = JPEG_REG_HUFFENC_DC_WORD_CNT,
    [JPEG_HUFFMAN_AC_0] = JPEG_REG_HUFFENC_AC_WORD_CNT,
    [JPEG_HUFFMAN_AC_1] = JPEG_REG_HUFFENC_AC_WORD_CNT,
};

_Static_assert( ( sizeof( ( (JPEG_TypeDef *)0 )->DHTMEM ) / sizeof( uint32_t ) ) == JPEG_REG_DHT_WORD_CNT,
                "Jpeg_Reg: the DHT memory has an unexpected size." );

_Static_assert( ( sizeof( ( (JPEG_TypeDef *)0 )->QMEM0 ) / sizeof( uint32_t ) ) == JPEG_REG_QUANT_WORD_CNT,
                "Jpeg_Reg: the quantization memory has an unexpected size." );

_Static_assert( ( sizeof( ( (JPEG_TypeDef *)0 )->HUFFENC_AC0 ) / sizeof( uint32_t ) ) == JPEG_REG_HUFFENC_AC_WORD_CNT,
                "Jpeg_Reg: the AC Huffman encoder memory has an unexpected size." );

_Static_assert( ( sizeof( ( (JPEG_TypeDef *)0 )->HUFFENC_DC0 ) / sizeof( uint32_t ) ) == JPEG_REG_HUFFENC_DC_WORD_CNT,
                "Jpeg_Reg: the DC Huffman encoder memory has an unexpected size." );

/* ========================= EXPORTED FUNCTIONS ============================= */

/**
 * \brief Sets the register bit with given mask.
 *
 * \param regId   [in]: JPEG register ID
 * \param bitMask [in]: Mask to apply to the register
 */
void Jpeg_Set_RegBit( jpeg_RegId_t regId, uint32_t bitMask )
{
    if( JPEG_REG_CNT > regId )
    {
        *jpeg_RegList[regId].RegAddr |= bitMask;
    }
    else
    {
        /* Invalid register ID, no action required */
    }
}


/**
 * \brief Resets the register bit with given mask.
 *
 * \param regId   [in]: JPEG register ID
 * \param bitMask [in]: Mask to apply to the register
 */
void Jpeg_Reset_RegBit( jpeg_RegId_t regId, uint32_t bitMask )
{
    if( JPEG_REG_CNT > regId )
    {
        *jpeg_RegList[regId].RegAddr &= ~bitMask;
    }
    else
    {
        /* Invalid register ID, no action required */
    }
}


/**
 * \brief Returns the register bits selected by the mask.
 *
 * \param regId   [in]: JPEG register ID
 * \param bitMask [in]: Mask to apply to the register
 *
 * \return Masked register value, zero for an invalid register ID.
 */
uint32_t Jpeg_Get_RegBit( jpeg_RegId_t regId, uint32_t bitMask )
{
    uint32_t retValue = JPEG_REG_BITS_CLEAR;

    if( JPEG_REG_CNT > regId )
    {
        retValue = ( *jpeg_RegList[regId].RegAddr & bitMask );
    }
    else
    {
        /* Invalid register ID, no bit is set */
    }

    return ( retValue );
}


/**
 * \brief Replaces the register bits selected by the mask with the given value.
 *
 * \param regId    [in]: JPEG register ID
 * \param regMask  [in]: Mask of the bits to replace
 * \param regValue [in]: New value of the bits (already shifted to their position)
 */
void Jpeg_Set_RegVal( jpeg_RegId_t regId, uint32_t regMask, uint32_t regValue )
{
    if( JPEG_REG_CNT > regId )
    {
        const uint32_t oldValue = *jpeg_RegList[regId].RegAddr;

        *jpeg_RegList[regId].RegAddr = ( ( oldValue & ~regMask ) | ( regValue & regMask ) );
    }
    else
    {
        /* Invalid register ID, no action required */
    }
}


/**
 * \brief Returns the register bits selected by the mask (not shifted).
 *
 * \param regId   [in]: JPEG register ID
 * \param regMask [in]: Mask to apply to the register
 *
 * \return Masked register value, zero for an invalid register ID.
 */
uint32_t Jpeg_Get_RegVal( jpeg_RegId_t regId, uint32_t regMask )
{
    return ( Jpeg_Get_RegBit( regId, regMask ) );
}


/**
 * \brief Waits until the masked register bits read as the given value (bounded loop, no blind delay).
 *
 * \param regId    [in]: JPEG register ID
 * \param regMask  [in]: Mask of the bits to compare
 * \param regValue [in]: Expected value of the masked bits
 *
 * \return Returns \ref JPEG_REQUEST_OK if the bits had the value within the timeout.
 *         Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Reg_Wait_RegVal( jpeg_RegId_t regId, uint32_t regMask, uint32_t regValue )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

    for( uint32_t idx = 0u;
         ( JPEG_REG_TIMEOUT_CNT > idx ) &&
         ( JPEG_REQUEST_OK != retState );
         idx++ )
    {
        const uint32_t actualValue = Jpeg_Get_RegVal( regId, regMask );

        if( regValue == actualValue )
        {
            retState = JPEG_REQUEST_OK;
        }
        else
        {
            /* The bits did not change yet */
        }
    }

    return ( retState );
}


/**
 * \brief Enables the core of the codec (JPEG_CR.JCEN) and verifies the bit by a read-back.
 *
 * \return Returns \ref JPEG_REQUEST_OK if JCEN reads back as set. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Reg_Enable_Core( void )
{
    Jpeg_Set_RegBit( JPEG_REG_CR, JPEG_CR_JCEN );

    return ( Jpeg_Reg_Wait_RegVal( JPEG_REG_CR, JPEG_CR_JCEN, JPEG_CR_JCEN ) );
}


/**
 * \brief Disables the core of the codec: clears the flags first (JCEN may be cleared only while EOCF is clear),
 *        then clears JPEG_CR.JCEN, which resets the internal registers.
 */
void Jpeg_Reg_Disable_Core( void )
{
    Jpeg_Reg_Clear_Flags( JPEG_REG_CFR_ALL_MASK );
    Jpeg_Reset_RegBit( JPEG_REG_CR, JPEG_CR_JCEN );
}


/**
 * \brief Stops the codec and returns it to the idle state: clears START and waits for the end of the operation
 *        (JPEG_SR.COF), disables all interrupts and DMA requests, flushes both FIFOs and clears the flags.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the codec stopped within the timeout.
 *         Otherwise returns \ref JPEG_REQUEST_ERROR (the other steps are done anyway).
 */
jpeg_RequestState_t Jpeg_Reg_Reset_Codec( void )
{
    const jpeg_RequestState_t retState = Jpeg_Reg_Stop_Codec();

    Jpeg_Reset_RegBit( JPEG_REG_CR, JPEG_REG_CR_REQUEST_MASK );
    Jpeg_Set_RegBit( JPEG_REG_CR, ( JPEG_CR_IFF | JPEG_CR_OFF ) );
    Jpeg_Reg_Clear_Flags( JPEG_REG_CFR_ALL_MASK );

    return ( retState );
}


/**
 * \brief Writes the image configuration (JPEG_CONFR1 to JPEG_CONFR7). The codec has to be stopped.
 *
 * \param imageConfig [in]: Image configuration. Must not be NULL.
 */
void Jpeg_Reg_Set_ImageConfig( const jpeg_RegImageConfig_t * const imageConfig )
{
    if( JPEG_NULL_PTR != imageConfig )
    {
        uint32_t confr1 = ( ( imageConfig->Decode << JPEG_CONFR1_DE_Pos ) & JPEG_CONFR1_DE_Msk );
        const uint32_t confr2 = ( ( imageConfig->McuCnt << JPEG_CONFR2_NMCU_Pos ) & JPEG_CONFR2_NMCU_Msk );
        const uint32_t confr3 = ( ( imageConfig->Width << JPEG_CONFR3_XSIZE_Pos ) & JPEG_CONFR3_XSIZE_Msk );

        confr1 |= ( ( imageConfig->Header     << JPEG_CONFR1_HDR_Pos        ) & JPEG_CONFR1_HDR_Msk        );
        confr1 |= ( ( imageConfig->ColorSpace << JPEG_CONFR1_COLORSPACE_Pos ) & JPEG_CONFR1_COLORSPACE_Msk );
        confr1 |= ( ( imageConfig->Components << JPEG_CONFR1_NF_Pos         ) & JPEG_CONFR1_NF_Msk         );
        confr1 |= ( ( imageConfig->Scan       << JPEG_CONFR1_NS_Pos         ) & JPEG_CONFR1_NS_Msk         );
        confr1 |= ( ( imageConfig->Height     << JPEG_CONFR1_YSIZE_Pos      ) & JPEG_CONFR1_YSIZE_Msk      );

        Jpeg_Set_RegVal( JPEG_REG_CONFR1, JPEG_REG_FULL_MASK, confr1 );
        Jpeg_Set_RegVal( JPEG_REG_CONFR2, JPEG_REG_FULL_MASK, confr2 );
        Jpeg_Set_RegVal( JPEG_REG_CONFR3, JPEG_REG_FULL_MASK, confr3 );

        for( uint32_t idx = 0u; JPEG_REG_COMPONENT_CNT > idx; idx++ )
        {
            const jpeg_RegComponent_t * const component = &imageConfig->Component[ idx ];
            uint32_t compValue = ( ( component->HorizontalFactor << JPEG_CONFR4_HSF_Pos ) & JPEG_CONFR4_HSF_Msk );

            compValue |= ( ( component->VerticalFactor << JPEG_CONFR4_VSF_Pos ) & JPEG_CONFR4_VSF_Msk );
            compValue |= ( ( component->BlockCnt       << JPEG_CONFR4_NB_Pos  ) & JPEG_CONFR4_NB_Msk  );
            compValue |= ( ( component->QuantTable     << JPEG_CONFR4_QT_Pos  ) & JPEG_CONFR4_QT_Msk  );
            compValue |= ( ( component->HuffmanAc      << JPEG_CONFR4_HA_Pos  ) & JPEG_CONFR4_HA_Msk  );
            compValue |= ( ( component->HuffmanDc      << JPEG_CONFR4_HD_Pos  ) & JPEG_CONFR4_HD_Msk  );

            Jpeg_Set_RegVal( jpeg_ComponentRegLut[ idx ], JPEG_REG_FULL_MASK, compValue );
        }
    }
    else
    {
        /* Missing parameters, no action required */
    }
}


/**
 * \brief Reads the image configuration (JPEG_CONFR1 to JPEG_CONFR7), for example the one the header parser stored.
 *
 * \param imageConfig [out]: Image configuration to fill. Must not be NULL.
 */
void Jpeg_Reg_Get_ImageConfig( jpeg_RegImageConfig_t * const imageConfig )
{
    if( JPEG_NULL_PTR != imageConfig )
    {
        const uint32_t confr1 = Jpeg_Get_RegVal( JPEG_REG_CONFR1, JPEG_REG_FULL_MASK );
        const uint32_t confr2 = Jpeg_Get_RegVal( JPEG_REG_CONFR2, JPEG_REG_FULL_MASK );
        const uint32_t confr3 = Jpeg_Get_RegVal( JPEG_REG_CONFR3, JPEG_REG_FULL_MASK );

        imageConfig->Decode     = ( ( confr1 & JPEG_CONFR1_DE_Msk         ) >> JPEG_CONFR1_DE_Pos         );
        imageConfig->Header     = ( ( confr1 & JPEG_CONFR1_HDR_Msk        ) >> JPEG_CONFR1_HDR_Pos        );
        imageConfig->ColorSpace = ( ( confr1 & JPEG_CONFR1_COLORSPACE_Msk ) >> JPEG_CONFR1_COLORSPACE_Pos );
        imageConfig->Components = ( ( confr1 & JPEG_CONFR1_NF_Msk         ) >> JPEG_CONFR1_NF_Pos         );
        imageConfig->Scan       = ( ( confr1 & JPEG_CONFR1_NS_Msk         ) >> JPEG_CONFR1_NS_Pos         );
        imageConfig->Height     = ( ( confr1 & JPEG_CONFR1_YSIZE_Msk      ) >> JPEG_CONFR1_YSIZE_Pos      );
        imageConfig->Width      = ( ( confr3 & JPEG_CONFR3_XSIZE_Msk      ) >> JPEG_CONFR3_XSIZE_Pos      );
        imageConfig->McuCnt     = ( ( confr2 & JPEG_CONFR2_NMCU_Msk       ) >> JPEG_CONFR2_NMCU_Pos       );

        for( uint32_t idx = 0u; JPEG_REG_COMPONENT_CNT > idx; idx++ )
        {
            const uint32_t compValue = Jpeg_Get_RegVal( jpeg_ComponentRegLut[ idx ], JPEG_REG_FULL_MASK );
            jpeg_RegComponent_t * const component = &imageConfig->Component[ idx ];

            component->HorizontalFactor = ( ( compValue & JPEG_CONFR4_HSF_Msk ) >> JPEG_CONFR4_HSF_Pos );
            component->VerticalFactor   = ( ( compValue & JPEG_CONFR4_VSF_Msk ) >> JPEG_CONFR4_VSF_Pos );
            component->BlockCnt         = ( ( compValue & JPEG_CONFR4_NB_Msk  ) >> JPEG_CONFR4_NB_Pos  );
            component->QuantTable       = ( ( compValue & JPEG_CONFR4_QT_Msk  ) >> JPEG_CONFR4_QT_Pos  );
            component->HuffmanAc        = ( ( compValue & JPEG_CONFR4_HA_Msk  ) >> JPEG_CONFR4_HA_Pos  );
            component->HuffmanDc        = ( ( compValue & JPEG_CONFR4_HD_Msk  ) >> JPEG_CONFR4_HD_Pos  );
        }
    }
    else
    {
        /* Missing parameters, no action required */
    }
}


/**
 * \brief Starts the encoding or the decoding (JPEG_CONFR0.START).
 */
void Jpeg_Reg_Start_Codec( void )
{
    Jpeg_Set_RegBit( JPEG_REG_CONFR0, JPEG_CONFR0_START );
}


/**
 * \brief Stops the encoding or the decoding (clears JPEG_CONFR0.START) and waits until the codec reports that
 *        no operation is in progress (JPEG_SR.COF).
 *
 * \return Returns \ref JPEG_REQUEST_OK if the codec stopped within the timeout.
 *         Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Reg_Stop_Codec( void )
{
    Jpeg_Reset_RegBit( JPEG_REG_CONFR0, JPEG_CONFR0_START );

    return ( Jpeg_Reg_Wait_RegVal( JPEG_REG_SR, JPEG_SR_COF, JPEG_REG_BITS_CLEAR ) );
}


/**
 * \brief Clears the flags of the end of the conversion and of the header parsing (the other flags follow the FIFOs).
 *
 * \param clearMask [in]: Bits of JPEG_CFR to set (\ref JPEG_REG_CFR_CEOCF, \ref JPEG_REG_CFR_CHPDF).
 */
void Jpeg_Reg_Clear_Flags( uint32_t clearMask )
{
    Jpeg_Set_RegVal( JPEG_REG_CFR, JPEG_REG_CFR_ALL_MASK, clearMask );
}


/**
 * \brief Writes one 32-bit word to the input FIFO (JPEG_DIR).
 *
 * \param dataWord [in]: Data word (four bytes of the stream, the first one in the lowest byte).
 */
void Jpeg_Reg_Write_Word( uint32_t dataWord )
{
    *jpeg_RegList[ JPEG_REG_DIR ].RegAddr = dataWord;
}


/**
 * \brief Reads one 32-bit word from the output FIFO (JPEG_DOR).
 *
 * \return Data word (four bytes of the stream, the first one in the lowest byte).
 */
uint32_t Jpeg_Reg_Read_Word( void )
{
    return ( *jpeg_RegList[ JPEG_REG_DOR ].RegAddr );
}


/**
 * \brief Writes a quantization memory: 16 words of four coefficients each, in the zigzag order.
 *
 * \param tableId  [in]: Quantization table.
 * \param wordList [in]: The 16 words to write. Must not be NULL.
 */
void Jpeg_Reg_Set_QuantWords( jpeg_QuantId_t tableId, const uint32_t * const wordList )
{
    if( ( JPEG_QUANT_TABLE_CNT > tableId      ) &&
        ( JPEG_NULL_PTR       != wordList     )    )
    {
        for( uint32_t idx = 0u; JPEG_REG_QUANT_WORD_CNT > idx; idx++ )
        {
            jpeg_QuantMemLut[ tableId ][ idx ] = wordList[ idx ];
        }
    }
    else
    {
        /* Invalid table or missing words, no action required */
    }
}


/**
 * \brief Reads one word of a quantization memory.
 *
 * \param tableId [in]: Quantization table.
 * \param wordIdx [in]: Index of the word (0 to 15).
 *
 * \return The word, zero for an invalid table or index.
 */
uint32_t Jpeg_Reg_Get_QuantWord( jpeg_QuantId_t tableId, uint32_t wordIdx )
{
    uint32_t retValue = JPEG_REG_BITS_CLEAR;

    if( ( JPEG_QUANT_TABLE_CNT     > tableId ) &&
        ( JPEG_REG_QUANT_WORD_CNT  > wordIdx )    )
    {
        retValue = jpeg_QuantMemLut[ tableId ][ wordIdx ];
    }
    else
    {
        /* Invalid table or index, zero is returned */
    }

    return ( retValue );
}


/**
 * \brief Writes the first words of a Huffman encoder memory (two entries of the code length and the code per word).
 *
 * \param tableId  [in]: Huffman table.
 * \param wordList [in]: The words to write. Must not be NULL.
 * \param wordCnt  [in]: Count of the words (at most the size of the memory of the table).
 */
void Jpeg_Reg_Set_HuffEncWords( jpeg_HuffmanId_t tableId, const uint32_t * const wordList, uint32_t wordCnt )
{
    if( ( JPEG_HUFFMAN_CNT > tableId  ) &&
        ( JPEG_NULL_PTR   != wordList )    )
    {
        if( jpeg_HuffEncWordCntLut[ tableId ] >= wordCnt )
        {
            for( uint32_t idx = 0u; wordCnt > idx; idx++ )
            {
                jpeg_HuffEncMemLut[ tableId ][ idx ] = wordList[ idx ];
            }
        }
        else
        {
            /* More words than the memory has, no action required */
        }
    }
    else
    {
        /* Invalid table or missing words, no action required */
    }
}


/**
 * \brief Writes the DHT memory: the tables of the header the encoder generates.
 *
 * \param wordList [in]: The 103 words to write. Must not be NULL.
 */
void Jpeg_Reg_Set_DhtWords( const uint32_t * const wordList )
{
    if( JPEG_NULL_PTR != wordList )
    {
        for( uint32_t idx = 0u; JPEG_REG_DHT_WORD_CNT > idx; idx++ )
        {
            JPEG->DHTMEM[ idx ] = wordList[ idx ];
        }
    }
    else
    {
        /* Missing words, no action required */
    }
}


/**
 * \brief Returns the address of JPEG_DIR as a plain 32-bit value, for the Gpdma destination address.
 *
 * \return Address of JPEG_DIR.
 */
uint32_t Jpeg_Reg_Get_DirAddr( void )
{
    return ( (uint32_t)(uintptr_t)jpeg_RegList[ JPEG_REG_DIR ].RegAddr );
}


/**
 * \brief Returns the address of JPEG_DOR as a plain 32-bit value, for the Gpdma source address.
 *
 * \return Address of JPEG_DOR.
 */
uint32_t Jpeg_Reg_Get_DorAddr( void )
{
    return ( (uint32_t)(uintptr_t)jpeg_RegList[ JPEG_REG_DOR ].RegAddr );
}

#endif /* JPEG */
