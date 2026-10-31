/**
 * \author Mr.Nobody
 * \file Test_Jpeg.c
 * \ingroup Jpeg
 * \brief Unit tests of the JPEG codec (JPEG) module.
 *
 * Jpeg.c, Jpeg_Reg.c and Jpeg_Tables.c are compiled unchanged, the JPEG registers are emulated by RegMem, the RCC,
 *   NVIC,
 * Gpdma and Cache modules are mocked by CMock.
 *
 * \note Limitations of the register emulation (RegMem registers are plain memory). The tests do not activate a HW
 *       model. The FIFOs are not emulated: JPEG_SR (the flags of the FIFOs, the end of the conversion, the header
 *       parsing) is preset by the tests, a write to JPEG_DIR keeps the last word and a read of JPEG_DOR returns the
 *       word the test preset (every read returns the same word). The tests prove the register programming, the
 *       memory images the encoder writes (calculated by an independent implementation, see Test_Jpeg_Data.h), the
 *       flow control by the flags, the packing of the words, the callbacks, the states and the transfers the module
 *       arms in the Gpdma; the codec itself (the compression) is proved by the integration tests on hardware.
 *       Every test case is executed in its own process (CTest), the runtime data of the module start from the
 *       reset state in every test.
 *
 * \note The Gpdma functions the module calls are replaced by stubs which record every call (function, controller,
 *       channel and the value the test checks) in a log, keep a copy of the configuration of both channels (so that
 *       the tests can call the callbacks registered by the module) and let the remaining block size of the output
 *       channel be preset. The Cache functions are replaced by stubs as well. The data buffers of the DMA tests are
 *       placed in the emulated SRAM (32-bit addresses).
 *
 * \note The JPEG codec does not exist on STM32H503, H523, H533, H543, H553, H562, H563 and H573. The test runner is
 *       generated from this file without running the preprocessor, so every test case function below is defined
 *       unconditionally (the same list on all 12 device lines); on the lines without the peripheral the test cases
 *       use TEST_IGNORE_MESSAGE and the test case of the missing peripheral checks that every function returns an
 *       error.
 */

/* ============================= INCLUDES =================================== */
#include "unity.h"                          /* Unity testing framework        */
#include "RegMem.h"                         /* Register memory emulation      */
#include "Jpeg_Port.h"                      /* Module under test              */
#include "Jpeg.h"                           /* Private interface (test only)  */
#include "Jpeg_Reg.h"                       /* Register access (test only)    */
#include "Jpeg_Tables.h"                    /* Standard tables (test only)    */
#include "Test_Jpeg_Data.h"                 /* Expected register images       */
#include "MockRcc_Port.h"                   /* RCC module mock                */
#include "MockNvic_Port.h"                  /* NVIC module mock               */
#include "MockGpdma_Port.h"                 /* Gpdma module mock              */
#include "MockCache_Port.h"                 /* Cache module mock              */
#include "Stm32.h"                          /* JPEG registers definition      */
#include "stdint.h"                         /* Standard integer types         */
/* ============================= TYPEDEFS =================================== */

#if defined(JPEG)

/** Gpdma functions recorded by the stubs */
typedef enum
{
    UT_JPEG_GPDMA_INIT = 0u,    /**< Gpdma_Init                    */
    UT_JPEG_GPDMA_DEINIT,       /**< Gpdma_Deinit                  */
    UT_JPEG_GPDMA_IRQ_ACTIVE,   /**< Gpdma_Set_InterruptActive     */
    UT_JPEG_GPDMA_LIST_CONFIG,  /**< Gpdma_Get_XferListConfig      */
    UT_JPEG_GPDMA_LINK_REG,     /**< Gpdma_Get_XferList_LinkReg    */
    UT_JPEG_GPDMA_LIST_LINK,    /**< Gpdma_Set_XferListLink        */
    UT_JPEG_GPDMA_BLOCK_SIZE,   /**< Gpdma_Set_BlockSize           */
    UT_JPEG_GPDMA_SRC_ADDR,     /**< Gpdma_Set_SourceAddr          */
    UT_JPEG_GPDMA_DST_ADDR,     /**< Gpdma_Set_DestinationAddr     */
    UT_JPEG_GPDMA_CH_ACTIVE,    /**< Gpdma_Set_ChannelActive       */
    UT_JPEG_GPDMA_CH_INACTIVE,  /**< Gpdma_Set_ChannelInactive     */
    UT_JPEG_GPDMA_PAUSE_ACTIVE, /**< Gpdma_Set_PauseActive         */
    UT_JPEG_GPDMA_PAUSE_INACTIVE, /**< Gpdma_Set_PauseInactive     */
    UT_JPEG_GPDMA_GET_BLOCK,    /**< Gpdma_Get_BlockSize           */
    UT_JPEG_GPDMA_NONE,         /**< No call (no failure injected) */
    UT_JPEG_GPDMA_CNT           /**< Count of the items            */
}   utJpeg_GpdmaCall_t;

/** Record of one Gpdma call */
typedef struct
{
    utJpeg_GpdmaCall_t Id;      /**< Gpdma function                    */
    uint32_t           Periph;  /**< Controller of the call            */
    uint32_t           Channel; /**< Channel of the call               */
    uint32_t           Value;   /**< Value of the call the test checks */
}   utJpeg_GpdmaRecord_t;

/** Cache operations recorded by the stubs */
typedef enum
{
    UT_JPEG_CACHE_CLEAN = 0u,    /**< Cache_Set_CleanRange           */
    UT_JPEG_CACHE_FLUSH,         /**< Cache_Set_CleanInvalidateRange */
    UT_JPEG_CACHE_INVALIDATE,    /**< Cache_Set_InvalidateRange      */
    UT_JPEG_CACHE_OP_CNT         /**< Count of the items             */
}   utJpeg_CacheOp_t;

/** Record of one Cache call */
typedef struct
{
    utJpeg_CacheOp_t Operation; /**< Cache function     */
    uint32_t         Address;   /**< Address of the call */
    uint32_t         Size;      /**< Size of the call    */
}   utJpeg_CacheRecord_t;

/** One row of the table of the image registers of the encoder */
typedef struct
{
    jpeg_ColorSpace_t  ColorSpace;  /**< Color space of the image                   */
    jpeg_Subsampling_t Subsampling; /**< Chroma subsampling of the image            */
    uint32_t           Confr1;      /**< Expected JPEG_CONFR1 without the YSIZE     */
    uint32_t           Confr4;      /**< Expected JPEG_CONFR4                       */
    uint32_t           Confr5;      /**< Expected JPEG_CONFR5                       */
    uint32_t           Confr6;      /**< Expected JPEG_CONFR6                       */
    uint32_t           Confr7;      /**< Expected JPEG_CONFR7                       */
    uint32_t           Quant;       /**< Count of the quantization tables written   */
}   utJpeg_ColorCase_t;

/** One row of the table of the raw sizes */
typedef struct
{
    jpeg_ColorSpace_t  ColorSpace;  /**< Color space of the image   */
    jpeg_Subsampling_t Subsampling; /**< Chroma subsampling         */
    jpeg_Dimension_t   Width;       /**< Width in pixels            */
    jpeg_Dimension_t   Height;      /**< Height in pixels           */
    jpeg_Count_t       McuCount;    /**< Expected count of the MCUs */
    jpeg_Size_t        RawSize;     /**< Expected raw size          */
}   utJpeg_RawCase_t;

#endif /* JPEG */

/* ======================= FORWARD DECLARATIONS ============================= */

#if defined(JPEG)

static nvic_RequestState_t  UtJpeg_NvicSetHandlerStub( nvic_PeriphIrqList_t irqId,
                                                       const nvic_IsrCallback_t irqHandler,
                                                       int callCnt );
static void                 UtJpeg_HeaderParsedCallback( const jpeg_ImageInfo_t * const imageInfo );
static void                 UtJpeg_InputNeededCallback( void );
static void                 UtJpeg_OutputReadyCallback( jpeg_Size_t length );
static void                 UtJpeg_EndOfConversionCallback( jpeg_Size_t totalLength );
static void                 UtJpeg_ErrorCallback   ( jpeg_Error_t errorMask );
static jpeg_Config_t        UtJpeg_Get_Config      ( jpeg_XferMode_t xferMode );
static void                 UtJpeg_Expect_Init     ( jpeg_XferMode_t xferMode );
static void                 UtJpeg_Expect_Release  ( jpeg_XferMode_t xferMode );
static void                 UtJpeg_Init            ( jpeg_XferMode_t xferMode );
static jpeg_ImageConfig_t   UtJpeg_Get_Image       ( jpeg_ColorSpace_t colorSpace,
                                                     jpeg_Subsampling_t subsampling,
                                                     jpeg_Quality_t quality );
static uint32_t             UtJpeg_CountEvents     ( void );
static void                 UtJpeg_StartJob        ( void );
static void                 UtJpeg_StartEncode     ( jpeg_XferMode_t xferMode );
static void                 UtJpeg_StartDecode     ( jpeg_XferMode_t xferMode );
static void                 UtJpeg_Preset_Header   ( uint32_t components, uint32_t horizontal, uint32_t vertical );
static uint32_t             UtJpeg_Build_Header    ( jpeg_Data_t * const data, uint32_t sofMarker, uint32_t precision );
static uint32_t             UtJpeg_Put_Segment     ( jpeg_Data_t * const data,
                                                     uint32_t pos,
                                                     uint32_t marker,
                                                     uint32_t length );
static void                 UtJpeg_FillBuffer      ( jpeg_Data_t * const data, jpeg_Size_t size, jpeg_Data_t pattern );
static uint32_t             UtJpeg_Get_Word        ( const jpeg_Data_t * const data, uint32_t wordIdx );
static cache_RequestState_t UtJpeg_CacheRecord     ( utJpeg_CacheOp_t operation, uint32_t address, uint32_t size );
static void                 UtJpeg_AssertQuantMemory( jpeg_QuantId_t tableId, const uint32_t * const expected );
static void                 UtJpeg_AssertWords     ( const volatile uint32_t * const actual,
                                                     const uint32_t * const expected,
                                                     uint32_t wordCnt );
static gpdma_RequestState_t UtJpeg_GpdmaRecord     ( utJpeg_GpdmaCall_t callId,
                                                     uint32_t periphId,
                                                     uint32_t channelId,
                                                     uint32_t value );
static void                 UtJpeg_GpdmaStubs      ( void );
static void                 UtJpeg_CacheStubs      ( void );
static void                 UtJpeg_ResetGpdmaLog   ( void );
static uint32_t             UtJpeg_CountGpdma      ( utJpeg_GpdmaCall_t callId, uint32_t channelId );
static uint32_t             UtJpeg_GetGpdmaValue   ( utJpeg_GpdmaCall_t callId, uint32_t channelId );
static void                 UtJpeg_StartDmaEncode  ( void );
static jpeg_Data_t *        UtJpeg_Get_DmaInput    ( void );
static jpeg_Data_t *        UtJpeg_Get_DmaOutput   ( void );
static gpdma_RequestState_t UtJpeg_GpdmaInitStub   ( gpdma_ConfigStruct_t * const configStruct, int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaDeinitStub ( gpdma_PeriphId_t periphId,
                                                     gpdma_ChannelId_t channelId,
                                                     int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaIrqActiveStub( gpdma_PeriphId_t periphId,
                                                       gpdma_ChannelId_t channelId,
                                                       int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaListConfigStub( gpdma_TransferConfig_t * const transferConfig,
                                                        gpdma_TransfersCount_t transferCount,
                                                        gpdma_ChannelType_t channelType,
                                                        volatile gpdma_XferList_t * transferList,
                                                        int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaLinkRegStub( volatile gpdma_XferList_t * const transferList,
                                                     gpdma_ChannelType_t channelType,
                                                     gpdma_XferLinkReg_t * const linkReg,
                                                     int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaListLinkStub( gpdma_PeriphId_t periphId,
                                                      gpdma_ChannelId_t channelId,
                                                      gpdma_XferLinkReg_t linkReg,
                                                      int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaBlockSizeStub( gpdma_PeriphId_t periphId,
                                                       gpdma_ChannelId_t channelId,
                                                       gpdma_BlockSize_t blockSize,
                                                       int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaSrcAddrStub( gpdma_PeriphId_t periphId,
                                                     gpdma_ChannelId_t channelId,
                                                     gpdma_SrcAddr_t sourceAddr,
                                                     int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaDstAddrStub( gpdma_PeriphId_t periphId,
                                                     gpdma_ChannelId_t channelId,
                                                     gpdma_DstAddr_t destAddr,
                                                     int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaChActiveStub( gpdma_PeriphId_t periphId,
                                                      gpdma_ChannelId_t channelId,
                                                      int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaChInactiveStub( gpdma_PeriphId_t periphId,
                                                        gpdma_ChannelId_t channelId,
                                                        int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaPauseActiveStub( gpdma_PeriphId_t periphId,
                                                         gpdma_ChannelId_t channelId,
                                                         int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaPauseInactiveStub( gpdma_PeriphId_t periphId,
                                                           gpdma_ChannelId_t channelId,
                                                           int callCnt );
static gpdma_RequestState_t UtJpeg_GpdmaGetBlockStub( gpdma_PeriphId_t periphId,
                                                      gpdma_ChannelId_t channelId,
                                                      gpdma_BlockSize_t * const blockSize,
                                                      int callCnt );
static cache_RequestState_t UtJpeg_CacheGetStateStub( cache_CacheId_t cacheId,
                                                      cache_State_t * const cacheState,
                                                      int callCnt );
static cache_RequestState_t UtJpeg_CacheCleanStub  ( cache_CacheId_t cacheId,
                                                     uint32_t address,
                                                     uint32_t size,
                                                     int callCnt );
static cache_RequestState_t UtJpeg_CacheFlushStub  ( cache_CacheId_t cacheId,
                                                     uint32_t address,
                                                     uint32_t size,
                                                     int callCnt );
static cache_RequestState_t UtJpeg_CacheInvalidateStub( cache_CacheId_t cacheId,
                                                        uint32_t address,
                                                        uint32_t size,
                                                        int callCnt );

#endif /* JPEG */

/* ========================== SYMBOLIC CONSTANTS ============================= */

/** Message of the test cases on a device line without the peripheral */
#define UT_JPEG_NO_PERIPH_MSG               "No JPEG codec on this device line"

/** Message of the test case of the missing peripheral on a device line with the peripheral */
#define UT_JPEG_HAS_PERIPH_MSG              "The JPEG codec exists on this device line"

/** Expected version of the module */
#define UT_JPEG_VERSION_MAJOR               ( 0u )
#define UT_JPEG_VERSION_MINOR               ( 1u )
#define UT_JPEG_VERSION_PATCH               ( 0u )

/** Interrupt priority used by the tests */
#define UT_JPEG_IRQ_PRIO                    ( 5u )

/** Quality factor of the module after the initialization and quality factors used by the tests */
#define UT_JPEG_QUALITY_DEFAULT             ( 75u )
#define UT_JPEG_QUALITY_LOW                 ( 1u )
#define UT_JPEG_QUALITY_MID                 ( 50u )
#define UT_JPEG_QUALITY_HIGH                ( 100u )
#define UT_JPEG_QUALITY_OVER                ( 101u )

/** Size of the image used by most tests: 16 x 16 pixels (4 MCUs of grayscale), 4:2:0 YCbCr 32 x 16 */
#define UT_JPEG_WIDTH                       ( 16u )
#define UT_JPEG_HEIGHT                      ( 16u )
#define UT_JPEG_WIDTH_WIDE                  ( 32u )
#define UT_JPEG_MCU_GRAY                    ( 4u )
#define UT_JPEG_MCU_WIDE                    ( 2u )

/** Bytes of a word of the FIFOs and the size of the FIFO in words */
#define UT_JPEG_WORD                        ( 4u )
#define UT_JPEG_FIFO_WORDS                  ( 8u )

/** Sizes of the test buffers in bytes */
#define UT_JPEG_BUF_SIZE                    ( 256u )
#define UT_JPEG_BUF_SMALL                   ( 16u )
#define UT_JPEG_BUF_TAIL                    ( 3u )
#define UT_JPEG_BUF_LARGE                   ( 40u )
#define UT_JPEG_BUF_MID                     ( 64u )
#define UT_JPEG_BUF_FIFO                    ( 32u )
#define UT_JPEG_BUF_UNALIGNED               ( 6u )
#define UT_JPEG_BUF_TOO_BIG                 ( 65536u )
#define UT_JPEG_BUF_DMA_MAX                 ( 65532u )

/** Test words of the FIFOs and their bytes */
#define UT_JPEG_IN_BYTE0                    ( 0x11u )
#define UT_JPEG_IN_BYTE1                    ( 0x22u )
#define UT_JPEG_IN_BYTE2                    ( 0x33u )
#define UT_JPEG_IN_BYTE3                    ( 0x44u )
#define UT_JPEG_IN_WORD                     ( 0x44332211u )
#define UT_JPEG_IN_WORD_TAIL                ( 0x00332211u )
#define UT_JPEG_OUT_WORD                    ( 0x04030201u )
#define UT_JPEG_OUT_BYTE0                   ( 0x01u )
#define UT_JPEG_OUT_BYTE1                   ( 0x02u )
#define UT_JPEG_OUT_BYTE2                   ( 0x03u )
#define UT_JPEG_OUT_BYTE3                   ( 0x04u )
#define UT_JPEG_DIR_EMPTY                   ( 0xA5A5A5A5u )
#define UT_JPEG_FILL_PATTERN                ( 0x5Au )

/** Count of the calls of a callback */
#define UT_JPEG_TWICE                       ( 2u )

/** Words of the sample tables: the memories of the encoder */
#define UT_JPEG_HUFFENC_DC_WORDS            ( 8u )
#define UT_JPEG_HUFFENC_AC_WORDS            ( 88u )

/** Marker values of the test streams */
#define UT_JPEG_SOF0                        ( 0xC0u )
#define UT_JPEG_SOF1                        ( 0xC1u )
#define UT_JPEG_SOF2                        ( 0xC2u )
#define UT_JPEG_SOF3                        ( 0xC3u )
#define UT_JPEG_SOF9                        ( 0xC9u )
#define UT_JPEG_PRECISION_8                 ( 8u )
#define UT_JPEG_PRECISION_12                ( 12u )
#define UT_JPEG_HEADER_SIZE                 ( 128u )
#define UT_JPEG_APP0_LENGTH                 ( 16u )
#define UT_JPEG_DQT_LENGTH                  ( 67u )
#define UT_JPEG_SOF_LENGTH                  ( 17u )
#define UT_JPEG_MARKER_PREFIX               ( 0xFFu )
#define UT_JPEG_MARKER_SOI                  ( 0xD8u )
#define UT_JPEG_MARKER_APP0                 ( 0xE0u )
#define UT_JPEG_MARKER_DQT                  ( 0xDBu )
#define UT_JPEG_MARKER_TEM                  ( 0x01u )
#define UT_JPEG_MARKER_RST0                 ( 0xD0u )
#define UT_JPEG_BYTE_MASK                   ( 0xFFu )
#define UT_JPEG_BYTE_SHIFT                  ( 8u )

/** Image of the parsed header and its sampling factors */
#define UT_JPEG_NF_GRAY                     ( 0u )
#define UT_JPEG_NF_TWO                      ( 1u )
#define UT_JPEG_NF_YCBCR                    ( 2u )
#define UT_JPEG_NF_CMYK                     ( 3u )
#define UT_JPEG_FACTOR_ONE                  ( 1u )
#define UT_JPEG_FACTOR_TWO                  ( 2u )
#define UT_JPEG_FACTOR_FOUR                 ( 4u )

/** Largest dimension and sizes which do not fit 32 bits */
#define UT_JPEG_DIMENSION_MAX               ( 65535u )

/** Argument of a Gpdma call which has no such argument */
#define UT_JPEG_NA                          ( 0xFFFFu )
#define UT_JPEG_LINKREG_MARK                ( 0xA5A5A5A5u )

/** Size of the log of the Gpdma calls and of the Cache calls */
#define UT_JPEG_DMA_LOG_SIZE                ( 96u )
#define UT_JPEG_CACHE_LOG_SIZE              ( 16u )

/** Count of the Gpdma channels of the module */
#define UT_JPEG_DMA_STREAMS                 ( 2u )

/** Second output buffer of the tests: the half of the output memory */
#define UT_JPEG_OUT_SECOND                  ( &utJpeg_OutBuf[ UT_JPEG_BUF_SMALL ] )

/** Indexes of the third and the fourth byte of a word */
#define UT_JPEG_BYTE_IDX2                   ( 2u )
#define UT_JPEG_BYTE_IDX3                   ( 3u )

/** Offsets of the buffers of the DMA tests in the emulated SRAM */
#define UT_JPEG_CFR_CLEAR_ALL               ( 0x60u )   /* CEOCF (bit 5) | CHPDF (bit 6), RM0517 41.5.8 */
#define UT_JPEG_CFR_CLEAR_HEADER            ( 0x40u )   /* CHPDF (bit 6), RM0517 41.5.8                 */
#define UT_JPEG_SRAM_INPUT                  ( 0x1000u )
#define UT_JPEG_SRAM_OUTPUT                 ( 0x2000u )

/** Remaining block size of the output channel preset by the test and the bytes moved before the end */
#define UT_JPEG_DMA_REMAINING               ( 16u )
#define UT_JPEG_DMA_MOVED                   ( 48u )

/** Address of a buffer as the Gpdma takes it */
#define UT_JPEG_ADDR( ptr )                 ( (uint32_t)(uintptr_t)( ptr ) )

/** Image registers of a component: the sampling factors, the blocks - 1 and the tables */
#define UT_JPEG_COMP( hsf, vsf, nb, qt, ha, hd )                                                  \
    ( ( ( (uint32_t)( hsf ) << JPEG_CONFR4_HSF_Pos ) | ( (uint32_t)( vsf ) << JPEG_CONFR4_VSF_Pos ) | \
        ( (uint32_t)( nb  ) << JPEG_CONFR4_NB_Pos  ) | ( (uint32_t)( qt  ) << JPEG_CONFR4_QT_Pos  ) | \
        ( (uint32_t)( ha  ) << JPEG_CONFR4_HA_Pos  ) | ( (uint32_t)( hd  ) << JPEG_CONFR4_HD_Pos  ) ) )

/** JPEG_CONFR1 without the YSIZE for the encoder: the count of the components - 1, the color space, the header */
#define UT_JPEG_CONFR1( nf, colorSpace, header )                                                       \
    ( ( (uint32_t)( nf ) << JPEG_CONFR1_NF_Pos ) | ( (uint32_t)( nf ) << JPEG_CONFR1_NS_Pos ) |        \
      ( (uint32_t)( colorSpace ) << JPEG_CONFR1_COLORSPACE_Pos ) | ( (uint32_t)( header ) << JPEG_CONFR1_HDR_Pos ) )

/** Interrupt enable bits and DMA enable bits of JPEG_CR the module controls */
#define UT_JPEG_CR_IRQ_MASK                 ( JPEG_CR_IFTIE | JPEG_CR_IFNFIE | JPEG_CR_OFTIE | JPEG_CR_OFNEIE | \
                                              JPEG_CR_EOCIE | JPEG_CR_HPDIE )
#define UT_JPEG_CR_DMA_MASK                 ( JPEG_CR_IDMAEN | JPEG_CR_ODMAEN )

/** Rows of the table of the image registers: color space, subsampling, JPEG_CONFR1 (without YSIZE), JPEG_CONFR4 to
 *  JPEG_CONFR7 and the count of the quantization tables */
#define UT_JPEG_COLOR_CASES                                                                              \
    { JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, UT_JPEG_CONFR1( 0u, 0u, 1u ),                         \
      UT_JPEG_COMP( 1u, 1u, 0u, 0u, 0u, 0u ), 0u, 0u, 0u, 1u },                                         \
    { JPEG_COLOR_YCBCR, JPEG_SUBSAMPLING_444, UT_JPEG_CONFR1( 2u, 1u, 1u ),                              \
      UT_JPEG_COMP( 1u, 1u, 0u, 0u, 0u, 0u ), UT_JPEG_COMP( 1u, 1u, 0u, 1u, 1u, 1u ),                   \
      UT_JPEG_COMP( 1u, 1u, 0u, 1u, 1u, 1u ), 0u, 2u },                                                  \
    { JPEG_COLOR_YCBCR, JPEG_SUBSAMPLING_422, UT_JPEG_CONFR1( 2u, 1u, 1u ),                              \
      UT_JPEG_COMP( 2u, 1u, 1u, 0u, 0u, 0u ), UT_JPEG_COMP( 1u, 1u, 0u, 1u, 1u, 1u ),                   \
      UT_JPEG_COMP( 1u, 1u, 0u, 1u, 1u, 1u ), 0u, 2u },                                                  \
    { JPEG_COLOR_YCBCR, JPEG_SUBSAMPLING_420, UT_JPEG_CONFR1( 2u, 1u, 1u ),                              \
      UT_JPEG_COMP( 2u, 2u, 3u, 0u, 0u, 0u ), UT_JPEG_COMP( 1u, 1u, 0u, 1u, 1u, 1u ),                   \
      UT_JPEG_COMP( 1u, 1u, 0u, 1u, 1u, 1u ), 0u, 2u },                                                  \
    { JPEG_COLOR_RGB, JPEG_SUBSAMPLING_444, UT_JPEG_CONFR1( 2u, 2u, 1u ),                                \
      UT_JPEG_COMP( 1u, 1u, 0u, 0u, 0u, 0u ), UT_JPEG_COMP( 1u, 1u, 0u, 1u, 0u, 0u ),                   \
      UT_JPEG_COMP( 1u, 1u, 0u, 2u, 0u, 0u ), 0u, 3u },                                                  \
    { JPEG_COLOR_CMYK, JPEG_SUBSAMPLING_444, UT_JPEG_CONFR1( 3u, 3u, 1u ),                               \
      UT_JPEG_COMP( 1u, 1u, 0u, 0u, 0u, 0u ), UT_JPEG_COMP( 1u, 1u, 0u, 1u, 0u, 0u ),                   \
      UT_JPEG_COMP( 1u, 1u, 0u, 2u, 0u, 0u ), UT_JPEG_COMP( 1u, 1u, 0u, 3u, 0u, 0u ), 4u }

/** Rows of the table of the raw sizes: color space, subsampling, width, height, count of the MCUs, raw size */
#define UT_JPEG_RAW_CASES                                                                                \
    { JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444,  8u,  8u, 1u,    64u },                                 \
    { JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444,  9u,  9u, 4u,   256u },                                 \
    { JPEG_COLOR_YCBCR,     JPEG_SUBSAMPLING_444, 16u, 16u, 4u,   768u },                                 \
    { JPEG_COLOR_YCBCR,     JPEG_SUBSAMPLING_422, 16u,  8u, 1u,   256u },                                 \
    { JPEG_COLOR_YCBCR,     JPEG_SUBSAMPLING_420, 16u, 16u, 1u,   384u },                                 \
    { JPEG_COLOR_YCBCR,     JPEG_SUBSAMPLING_420, 17u, 17u, 4u,  1536u },                                 \
    { JPEG_COLOR_RGB,       JPEG_SUBSAMPLING_444,  8u,  8u, 1u,   192u },                                 \
    { JPEG_COLOR_CMYK,      JPEG_SUBSAMPLING_444,  8u,  8u, 1u,   256u }

/** Gpdma requests of the FIFOs of the codec (jpeg_rx_dma and jpeg_tx_dma) */
#define UT_JPEG_DMA_REQUEST_INPUT           ( 140u )
#define UT_JPEG_DMA_REQUEST_OUTPUT          ( 141u )

/** First word of the quantization memory 0 for the sample luminance table at quality 50 (zigzag 16, 11, 12, 14) */
#define UT_JPEG_QUANT_LUM_Q50_WORD0         ( 0x0E0C0B10u )

/** Raw size of the image of 32 x 16 pixels YCbCr 4:2:0 (two MCUs of 6 blocks) and the first word of the DHT memory of
 *  the custom DC table 0 (the second code count is 1) */
#define UT_JPEG_RAW_WIDE                    ( 768u )
#define UT_JPEG_CUSTOM_DHT_WORD             ( 0x00000100u )

/** Count of the events of three and four callbacks and the index of the third record of a log */
#define UT_JPEG_THREE_TIMES                 ( 3u )
#define UT_JPEG_FOUR_TIMES                  ( 4u )
#define UT_JPEG_THIRD_IDX                   ( 2u )

/** Size of an input buffer of the DMA mode which is a multiple of 4 but not of the cache line (16 bytes) */
#define UT_JPEG_BUF_UNALIGNED_CACHE         ( 20u )

/** Count of the bytes of a marker, offset of the first marker segment of the test header, markers SOS and EOI */
#define UT_JPEG_MARKER_SIZE                 ( 2u )
#define UT_JPEG_APP0_OFFSET                 ( 2u )
#define UT_JPEG_MARKER_SOS                  ( 0xDAu )
#define UT_JPEG_MARKER_EOI                  ( 0xD9u )

/** Offset of the precision in the start of frame segment */
#define UT_JPEG_SOF_PRECISION_OFFSET        ( 4u )

/** Frames the decoder rejects: markers SOF2, SOF3, SOF9 and SOF0 with 12-bit samples */
#define UT_JPEG_UNSUPPORTED_MARKERS         UT_JPEG_SOF2, UT_JPEG_SOF3, UT_JPEG_SOF9, UT_JPEG_SOF0
#define UT_JPEG_UNSUPPORTED_PRECISIONS      UT_JPEG_PRECISION_8, UT_JPEG_PRECISION_8, UT_JPEG_PRECISION_8, \
                                            UT_JPEG_PRECISION_12

/** Sizes of the first input buffer the decoder accepts: the SOI marker only, inside the first segment, at the
 *  length of a segment, inside the start of frame, with the precision and the whole header */
#define UT_JPEG_ACCEPTED_SIZES              2u, 10u, 21u, 96u, 98u, 112u

/** Parsed headers of 32 x 16 pixels: components - 1, sampling factors, color space, subsampling, MCUs and raw size */
#define UT_JPEG_HEADER_COMPONENTS           UT_JPEG_NF_GRAY, UT_JPEG_NF_YCBCR, UT_JPEG_NF_YCBCR, UT_JPEG_NF_YCBCR, \
                                            UT_JPEG_NF_CMYK
#define UT_JPEG_HEADER_HORIZONTAL           1u, 1u, 2u, 2u, 1u
#define UT_JPEG_HEADER_VERTICAL             1u, 1u, 1u, 2u, 1u
#define UT_JPEG_HEADER_COLORS               JPEG_COLOR_GRAYSCALE, JPEG_COLOR_YCBCR, JPEG_COLOR_YCBCR, \
                                            JPEG_COLOR_YCBCR, JPEG_COLOR_CMYK
#define UT_JPEG_HEADER_SUBSAMPLINGS         JPEG_SUBSAMPLING_444, JPEG_SUBSAMPLING_444, JPEG_SUBSAMPLING_422, \
                                            JPEG_SUBSAMPLING_420, JPEG_SUBSAMPLING_444
#define UT_JPEG_HEADER_MCUS                 8u, 8u, 4u, 2u, 8u
#define UT_JPEG_HEADER_RAWS                 512u, 1536u, 1024u, 768u, 2048u

/** Quantization coefficient of the custom table and the scaled word at quality 50 */
#define UT_JPEG_CUSTOM_COEF                 ( 2u )
#define UT_JPEG_CUSTOM_WORD                 ( 0x02020202u )
#define UT_JPEG_ONES_WORD                   ( 0x01010101u )
#define UT_JPEG_MAX_WORD                    ( 0xFFFFFFFFu )

/** Words of the unused part of a Huffman encoder memory and the first symbol of the custom table */
#define UT_JPEG_CUSTOM_DC_WORD              ( 0x0FFF0100u )
#define UT_JPEG_CUSTOM_AC_WORD              ( 0x0FFF0100u )
#define UT_JPEG_EMPTY_WORD                  ( 0x0FFF0FFFu )

/** Huffman code counts used by the invalid tables */
#define UT_JPEG_BITS_TOO_MANY               ( 3u )
#define UT_JPEG_SYMBOL_BAD_SIZE             ( 0x0Bu )
#define UT_JPEG_SYMBOL_DC_BAD               ( 12u )
#define UT_JPEG_SYMBOL_DC_MAX               ( 11u )
#define UT_JPEG_AC_SYMBOL                   ( 0x01u )
#define UT_JPEG_AC_SYMBOL_EOB               ( 0x00u )
#define UT_JPEG_CODES_DC_FULL               ( 13u )

/* =============================== MACROS =================================== */

/* ========================== EXPORTED VARIABLES ============================ */

/* =========================== LOCAL VARIABLES ============================== */

#if defined(JPEG)

/** Expected images of the sample tables */
static const uint32_t utJpeg_HuffEncDc0[ UT_JPEG_HUFFENC_DC_WORDS ] = { UT_JPEG_DATA_HUFFENC_DC0 };
static const uint32_t utJpeg_HuffEncDc1[ UT_JPEG_HUFFENC_DC_WORDS ] = { UT_JPEG_DATA_HUFFENC_DC1 };
static const uint32_t utJpeg_HuffEncAc0[ UT_JPEG_HUFFENC_AC_WORDS ] = { UT_JPEG_DATA_HUFFENC_AC0 };
static const uint32_t utJpeg_HuffEncAc1[ UT_JPEG_HUFFENC_AC_WORDS ] = { UT_JPEG_DATA_HUFFENC_AC1 };
static const uint32_t utJpeg_Dht[ JPEG_REG_DHT_WORD_CNT ] = { UT_JPEG_DATA_DHT };
static const uint32_t utJpeg_QuantLumQ1[ JPEG_REG_QUANT_WORD_CNT ] = { UT_JPEG_DATA_QUANT_LUM_Q1 };
static const uint32_t utJpeg_QuantLumQ50[ JPEG_REG_QUANT_WORD_CNT ] = { UT_JPEG_DATA_QUANT_LUM_Q50 };
static const uint32_t utJpeg_QuantLumQ75[ JPEG_REG_QUANT_WORD_CNT ] = { UT_JPEG_DATA_QUANT_LUM_Q75 };
static const uint32_t utJpeg_QuantLumQ100[ JPEG_REG_QUANT_WORD_CNT ] = { UT_JPEG_DATA_QUANT_LUM_Q100 };
static const uint32_t utJpeg_QuantChromQ50[ JPEG_REG_QUANT_WORD_CNT ] = { UT_JPEG_DATA_QUANT_CHROM_Q50 };
static const uint32_t utJpeg_QuantChromQ100[ JPEG_REG_QUANT_WORD_CNT ] = { UT_JPEG_DATA_QUANT_CHROM_Q100 };

/** Image registers the encoder writes for the color spaces */
static const utJpeg_ColorCase_t utJpeg_ColorCases[] = { UT_JPEG_COLOR_CASES };

/** Raw sizes of images */
static const utJpeg_RawCase_t utJpeg_RawCases[] = { UT_JPEG_RAW_CASES };

/** The ISR the module registered in Nvic and the result the Nvic stub returns */
static nvic_IsrCallback_t utJpeg_Isr;
static nvic_RequestState_t utJpeg_NvicHandlerResult;

/** Counters and last values of the callbacks */
static uint32_t           utJpeg_InputCnt;
static uint32_t           utJpeg_OutputCnt;
static jpeg_Size_t        utJpeg_OutputLast;
static uint32_t           utJpeg_EndCnt;
static jpeg_Size_t        utJpeg_EndLast;
static uint32_t           utJpeg_ErrorCnt;
static jpeg_Error_t       utJpeg_ErrorLast;
static uint32_t           utJpeg_HeaderCnt;
static jpeg_ImageInfo_t   utJpeg_HeaderInfo;

/** Data buffers of the tests (the buffers of the DMA tests are in the emulated SRAM) */
static jpeg_Data_t        utJpeg_InBuf[ UT_JPEG_BUF_SIZE ];
static jpeg_Data_t        utJpeg_OutBuf[ UT_JPEG_BUF_SIZE ];

/** Log of the Gpdma calls, copies of the Init configurations and of the transfer configurations */
static utJpeg_GpdmaRecord_t utJpeg_GpdmaLog[ UT_JPEG_DMA_LOG_SIZE ];
static uint32_t           utJpeg_GpdmaCnt;
static gpdma_ConfigStruct_t utJpeg_DmaInitConfig[ UT_JPEG_DMA_STREAMS ];
static gpdma_TransferConfig_t utJpeg_DmaInitXfer[ UT_JPEG_DMA_STREAMS ];
static gpdma_TransferConfig_t utJpeg_DmaXfer;
static gpdma_BlockSize_t  utJpeg_DmaRemaining;

/** Log of the Cache calls and the state of the data cache */
static utJpeg_CacheRecord_t utJpeg_CacheLog[ UT_JPEG_CACHE_LOG_SIZE ];
static uint32_t           utJpeg_CacheCnt;
static cache_FlagState_t  utJpeg_CacheEnabled;

#endif /* JPEG */

/* ============================ TEST FIXTURE ================================ */

void setUp( void )
{
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Reset() );

#if defined(JPEG)
    utJpeg_Isr               = NULL;
    utJpeg_NvicHandlerResult = NVIC_REQUEST_OK;
    utJpeg_InputCnt          = 0u;
    utJpeg_OutputCnt         = 0u;
    utJpeg_OutputLast        = 0u;
    utJpeg_EndCnt            = 0u;
    utJpeg_EndLast           = 0u;
    utJpeg_ErrorCnt          = 0u;
    utJpeg_ErrorLast         = JPEG_ERROR_NONE;
    utJpeg_HeaderCnt         = 0u;
    utJpeg_DmaRemaining      = 0u;
    utJpeg_CacheCnt          = 0u;
    utJpeg_CacheEnabled      = CACHE_FLAG_INACTIVE;

    UtJpeg_FillBuffer( utJpeg_InBuf, UT_JPEG_BUF_SIZE, 0u );
    UtJpeg_FillBuffer( utJpeg_OutBuf, UT_JPEG_BUF_SIZE, UT_JPEG_FILL_PATTERN );
    UtJpeg_ResetGpdmaLog();
    UtJpeg_GpdmaStubs();
    UtJpeg_CacheStubs();
    Nvic_Set_PeriphIrq_Handler_Stub( UtJpeg_NvicSetHandlerStub );
#endif /* JPEG */
}


void tearDown( void )
{
    ( void )RegMem_Set_ModelInactive();
}

/* ======================= MODULE VERSION AND DEFAULTS ====================== */

/**
 * \brief   Jpeg_Get_ModuleVersion() returns version of the module.
 *
 * \par Expected results
 * - Major, minor and patch version match the version of the module.
 */
void Ut_Jpeg_Get_ModuleVersion_Always_ReturnsVersion( void )
{
    const jpeg_ModuleVersion_t version = Jpeg_Get_ModuleVersion();

    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_VERSION_MAJOR, version.Major );
    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_VERSION_MINOR, version.Minor );
    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_VERSION_PATCH, version.Patch );
}


/**
 * \brief   Jpeg_Get_DefaultConfig() fills the configuration with the defaults.
 *
 * \par Expected results
 * - JPEG_REQUEST_OK, poll mode, Gpdma1 channels 0 and 1 at low priority, the sample tables, the header generation on,
 *   interrupt priority 0, no callback.
 */
void Ut_Jpeg_Get_DefaultConfig_ValidPointer_FillsDefaults( void )
{
#if defined(JPEG)
    jpeg_Config_t config;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( JPEG_XFER_MODE_POLL, config.XferMode );
    TEST_ASSERT_EQUAL( GPDMA_PERIPH_1, config.DmaPeriphId );
    TEST_ASSERT_EQUAL( GPDMA_CHANNEL_0, config.DmaInputChannelId );
    TEST_ASSERT_EQUAL( GPDMA_CHANNEL_1, config.DmaOutputChannelId );
    TEST_ASSERT_EQUAL( GPDMA_PRIORITY_LOW, config.DmaPriority );
    TEST_ASSERT_EQUAL( JPEG_FUNCTION_ACTIVE, config.StandardTables );
    TEST_ASSERT_EQUAL( JPEG_HEADER_ON, config.HeaderMode );
    TEST_ASSERT_EQUAL_UINT32( 0u, config.IrqPriority );
    TEST_ASSERT_NULL( config.HeaderParsedCallback );
    TEST_ASSERT_NULL( config.InputNeededCallback );
    TEST_ASSERT_NULL( config.OutputReadyCallback );
    TEST_ASSERT_NULL( config.EndOfConversionCallback );
    TEST_ASSERT_NULL( config.ErrorCallback );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Get_DefaultConfig() rejects a NULL pointer.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR.
 */
void Ut_Jpeg_Get_DefaultConfig_NullPointer_ReturnsError( void )
{
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_DefaultConfig( NULL ) );
}


/**
 * \brief   On a device line without the JPEG codec every function returns an error.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR from every function, no module (Rcc, Nvic, Gpdma, Cache) is called, the registers are not
 *   written. Ignored on the lines with the codec.
 */
void Ut_Jpeg_AllFunctions_NoPeripheral_ReturnError( void )
{
#if defined(JPEG)
    TEST_IGNORE_MESSAGE( UT_JPEG_HAS_PERIPH_MSG );
#else
    jpeg_Config_t      config;
    jpeg_ImageConfig_t image = { UT_JPEG_WIDTH, UT_JPEG_HEIGHT, JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444,
                                 UT_JPEG_QUALITY_MID };
    jpeg_QuantTable_t  quant;
    jpeg_HuffmanTable_t huffman;
    jpeg_State_t       state;
    jpeg_ImageInfo_t   info;
    jpeg_Size_t        size;
    jpeg_Quality_t     quality;
    jpeg_IrqPrio_t     irqPrio;
    jpeg_Data_t        data[ UT_JPEG_WORD ] = { 0u };

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Deinit() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_EncodeStart( &image ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_DecodeStart() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Abort() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Pause( JPEG_STREAM_INPUT ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Resume( JPEG_STREAM_INPUT ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( data, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_OutputBuffer( data, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_OutputLength( &size ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_ImageInfo( &info ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, &size ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Quality( UT_JPEG_QUALITY_MID ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_Quality( &quality ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_QuantizationTable( JPEG_QUANT_TABLE_0, &quant ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_DC_0, &huffman ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HeaderMode( JPEG_HEADER_ON ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_IrqPriority( UT_JPEG_IRQ_PRIO ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_IrqPriority( &irqPrio ) );

    Jpeg_Task();
#endif /* JPEG */
}

/* ============================ INITIALIZATION ============================== */

/**
 * \brief   Jpeg_Init() in the poll mode enables the core, leaves the codec idle and loads the sample tables.
 *
 * \par Expected results
 * - JPEG_REQUEST_OK, the clock is enabled through Rcc, JPEG_CFR is exactly 0x60 (CEOCF bit 5 and CHPDF bit 6 written,
 *   RM0517 41.5.8, not the CMSIS positions), JPEG_CR.JCEN is set, no interrupt or DMA request is enabled,
 *   JPEG_CONFR0.START is clear, the state is idle, the quality factor is the default one, Nvic and Gpdma are not used.
 */
void Ut_Jpeg_Init_Poll_EnablesCoreAndLoadsTables( void )
{
#if defined(JPEG)
    jpeg_Config_t  config  = UtJpeg_Get_Config( JPEG_XFER_MODE_POLL );
    jpeg_State_t   state   = JPEG_STATE_CNT;
    jpeg_Quality_t quality = 0u;

    UtJpeg_Expect_Init( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Init( &config ) );
    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_CFR_CLEAR_ALL, JPEG->CFR );
    TEST_ASSERT_EQUAL_UINT32( JPEG_CR_JCEN, JPEG->CR & ( JPEG_CR_JCEN | UT_JPEG_CR_IRQ_MASK | UT_JPEG_CR_DMA_MASK ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CONFR0 & JPEG_CONFR0_START );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_Quality( &quality ) );
    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_QUALITY_DEFAULT, quality );
    TEST_ASSERT_NULL( utJpeg_Isr );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_GpdmaCnt );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Init() in the interrupt mode configures the JPEG interrupt in Nvic.
 *
 * \par Expected results
 * - JPEG_REQUEST_OK, the handler is registered, the priority is set and the interrupt is enabled (in this order),
 *   the priority is read through Jpeg_Get_IrqPriority() and changed through Jpeg_Set_IrqPriority() (Nvic errors are
 *   returned).
 */
void Ut_Jpeg_Init_Isr_ConfiguresNvic( void )
{
#if defined(JPEG)
    jpeg_Config_t  config  = UtJpeg_Get_Config( JPEG_XFER_MODE_ISR );
    jpeg_IrqPrio_t irqPrio = 0u;

    UtJpeg_Expect_Init( JPEG_XFER_MODE_ISR );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Init( &config ) );
    TEST_ASSERT_NOT_NULL( utJpeg_Isr );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_GpdmaCnt );

    Nvic_Get_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_JPEG, &irqPrio, NVIC_REQUEST_OK );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_IrqPriority( &irqPrio ) );
    Nvic_Get_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_JPEG, &irqPrio, NVIC_REQUEST_ERROR );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_IrqPriority( &irqPrio ) );

    Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_JPEG, UT_JPEG_IRQ_PRIO, NVIC_REQUEST_OK );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_IrqPriority( UT_JPEG_IRQ_PRIO ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_IrqPriority( NULL ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Init() in the DMA mode initializes the two Gpdma channels.
 *
 * \par Expected results
 * - JPEG_REQUEST_OK, two Gpdma_Init() calls (channel 0 for the input, channel 1 for the output, the configured
 *   controller, one transfer list node, the transfer complete and the error callbacks, the requests 140 and 141 and
 *   the directions of the streams) and two interrupt enables of the channels.
 */
void Ut_Jpeg_Init_Dma_InitializesGpdmaChannels( void )
{
#if defined(JPEG)
    jpeg_Config_t config = UtJpeg_Get_Config( JPEG_XFER_MODE_DMA );

    config.DmaPeriphId        = GPDMA_PERIPH_1;
    config.DmaInputChannelId  = GPDMA_CHANNEL_2;
    config.DmaOutputChannelId = GPDMA_CHANNEL_3;
    config.DmaPriority        = GPDMA_PRIORITY_HIGH;

    UtJpeg_Expect_Init( JPEG_XFER_MODE_DMA );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Init( &config ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_DMA_STREAMS, UtJpeg_CountGpdma( UT_JPEG_GPDMA_INIT, UT_JPEG_NA ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_DMA_STREAMS, UtJpeg_CountGpdma( UT_JPEG_GPDMA_IRQ_ACTIVE, UT_JPEG_NA ) );

    TEST_ASSERT_EQUAL( GPDMA_CHANNEL_2, utJpeg_DmaInitConfig[ 0u ].ChannelId );
    TEST_ASSERT_EQUAL( GPDMA_CHANNEL_3, utJpeg_DmaInitConfig[ 1u ].ChannelId );

    for( uint32_t streamIdx = 0u; UT_JPEG_DMA_STREAMS > streamIdx; streamIdx++ )
    {
        TEST_ASSERT_EQUAL( GPDMA_PERIPH_1, utJpeg_DmaInitConfig[ streamIdx ].PeriphId );
        TEST_ASSERT_EQUAL( GPDMA_PRIORITY_HIGH, utJpeg_DmaInitConfig[ streamIdx ].ChannelPrio );
        TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_DmaInitConfig[ streamIdx ].TransfersCount );
        TEST_ASSERT_NOT_NULL( utJpeg_DmaInitConfig[ streamIdx ].TransferCompleteIsr );
        TEST_ASSERT_NOT_NULL( utJpeg_DmaInitConfig[ streamIdx ].ErrorIsr );
        TEST_ASSERT_NULL( utJpeg_DmaInitConfig[ streamIdx ].HalfTransferIsr );
    }

    TEST_ASSERT_EQUAL( GPDMA_DIR_MEMORY_TO_PERIPH, utJpeg_DmaInitXfer[ 0u ].Direction );
    TEST_ASSERT_EQUAL( GPDMA_DIR_PERIPH_TO_MEMORY, utJpeg_DmaInitXfer[ 1u ].Direction );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_DMA_REQUEST_INPUT, utJpeg_DmaInitXfer[ 0u ].RequestSource );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_DMA_REQUEST_OUTPUT, utJpeg_DmaInitXfer[ 1u ].RequestSource );
    TEST_ASSERT_EQUAL( GPDMA_DATA_SIZE_32BITS, utJpeg_DmaInitXfer[ 0u ].SourceDataSize );
    TEST_ASSERT_EQUAL( GPDMA_DATA_SIZE_32BITS, utJpeg_DmaInitXfer[ 1u ].DestinationDataSize );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Init() rejects an invalid configuration without any call or register access.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR for a NULL pointer, an invalid transfer mode, header mode, flag of the tables and, in the DMA
 *   mode, an invalid controller, channel or priority and two equal channels. No Rcc call, JPEG_CR is not written.
 */
void Ut_Jpeg_Init_InvalidParams_ReturnsError( void )
{
#if defined(JPEG)
    jpeg_Config_t config = UtJpeg_Get_Config( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( NULL ) );

    config.XferMode = JPEG_XFER_MODE_CNT;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );

    config = UtJpeg_Get_Config( JPEG_XFER_MODE_POLL );
    config.HeaderMode = JPEG_HEADER_CNT;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );

    config = UtJpeg_Get_Config( JPEG_XFER_MODE_POLL );
    config.StandardTables = (jpeg_FunctionState_t)( JPEG_FUNCTION_ACTIVE + 1u );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );

    config = UtJpeg_Get_Config( JPEG_XFER_MODE_DMA );
    config.DmaPeriphId = GPDMA_PERIPH_CNT;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );

    config = UtJpeg_Get_Config( JPEG_XFER_MODE_DMA );
    config.DmaInputChannelId = GPDMA_CHANNEL_CNT;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );

    config = UtJpeg_Get_Config( JPEG_XFER_MODE_DMA );
    config.DmaOutputChannelId = GPDMA_CHANNEL_CNT;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );

    config = UtJpeg_Get_Config( JPEG_XFER_MODE_DMA );
    config.DmaPriority = GPDMA_PRIORITY_CNT;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );

    config = UtJpeg_Get_Config( JPEG_XFER_MODE_DMA );
    config.DmaOutputChannelId = config.DmaInputChannelId;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );

    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CR );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Init() returns an error when Rcc can not enable the clock.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR, no register is written, the module stays not initialized.
 */
void Ut_Jpeg_Init_RccFails_ReturnsError( void )
{
#if defined(JPEG)
    jpeg_Config_t config = UtJpeg_Get_Config( JPEG_XFER_MODE_POLL );
    jpeg_State_t  state  = JPEG_STATE_CNT;

    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_JPEG, RCC_REQUEST_ERROR );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CR );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_State( &state ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Init() deinitializes the module when the codec does not report that it stopped.
 *
 * \par Expected results
 * - JPEG_SR.COF stays set, the wait times out: JPEG_REQUEST_ERROR, the peripheral is reset again through Rcc, the
 *   clock is disabled, JPEG_CR.JCEN is cleared, the module is not initialized.
 */
void Ut_Jpeg_Init_CodecNeverStops_DeinitializesAndReturnsError( void )
{
#if defined(JPEG)
    jpeg_Config_t config = UtJpeg_Get_Config( JPEG_XFER_MODE_POLL );
    jpeg_State_t  state  = JPEG_STATE_CNT;

    JPEG->SR = JPEG_SR_COF;

    UtJpeg_Expect_Init( JPEG_XFER_MODE_POLL );
    UtJpeg_Expect_Release( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CR & JPEG_CR_JCEN );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_State( &state ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Init() deinitializes the module when Nvic does not accept the handler.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR, the interrupt is disabled again, the peripheral is reset, the clock is disabled and the module
 *   is not initialized.
 */
void Ut_Jpeg_Init_NvicFails_DeinitializesAndReturnsError( void )
{
#if defined(JPEG)
    jpeg_Config_t config = UtJpeg_Get_Config( JPEG_XFER_MODE_ISR );
    jpeg_State_t  state  = JPEG_STATE_CNT;

    utJpeg_NvicHandlerResult = NVIC_REQUEST_ERROR;

    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_JPEG, RCC_REQUEST_OK );
    UtJpeg_Expect_Release( JPEG_XFER_MODE_ISR );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Init( &config ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_State( &state ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Deinit() releases the peripheral.
 *
 * \par Expected results
 * - JPEG_REQUEST_OK, the peripheral is reset and its clock disabled, JPEG_CFR is exactly 0x60 (both flags cleared),
 *   JPEG_CR.JCEN is cleared, every other function
 *   returns an error afterwards, a second Jpeg_Deinit() returns an error and a new Jpeg_Init() succeeds.
 */
void Ut_Jpeg_Deinit_Initialized_ReleasesPeripheral( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    UtJpeg_Init( JPEG_XFER_MODE_POLL );
    UtJpeg_Expect_Release( JPEG_XFER_MODE_POLL );
    JPEG->CFR = 0u;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Deinit() );
    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_CFR_CLEAR_ALL, JPEG->CFR );
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CR & JPEG_CR_JCEN );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Deinit() );

    UtJpeg_Init( JPEG_XFER_MODE_POLL );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Deinit() in the DMA mode stops and releases the Gpdma channels and ends a running job.
 *
 * \par Expected results
 * - The job is stopped, both channels are stopped (inactive) and released (deinitialized), the interrupt of the codec
 *   is disabled in Nvic, JPEG_REQUEST_OK.
 */
void Ut_Jpeg_Deinit_Dma_ReleasesGpdmaChannels( void )
{
#if defined(JPEG)
    UtJpeg_StartDmaEncode();
    UtJpeg_ResetGpdmaLog();
    UtJpeg_Expect_Release( JPEG_XFER_MODE_DMA );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Deinit() );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_INACTIVE, 0u ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_INACTIVE, 1u ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_DEINIT, 0u ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_DEINIT, 1u ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CONFR0 & JPEG_CONFR0_START );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Every function except the default configuration needs an initialized module.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR from the functions, Jpeg_Task() does nothing.
 */
void Ut_Jpeg_Functions_NotInitialized_ReturnError( void )
{
#if defined(JPEG)
    jpeg_ImageConfig_t  image   = UtJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, UT_JPEG_QUALITY_MID );
    jpeg_QuantTable_t   quant   = jpeg_StdQuantTable[ JPEG_STD_QUANT_LUM ];
    jpeg_HuffmanTable_t huffman = jpeg_StdHuffmanTable[ JPEG_HUFFMAN_DC_0 ];
    jpeg_State_t        state;
    jpeg_ImageInfo_t    info;
    jpeg_Size_t         size;
    jpeg_Quality_t      quality;
    jpeg_IrqPrio_t      irqPrio;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Deinit() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_EncodeStart( &image ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_DecodeStart() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Abort() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Pause( JPEG_STREAM_INPUT ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Resume( JPEG_STREAM_OUTPUT ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_OutputLength( &size ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_ImageInfo( &info ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Quality( UT_JPEG_QUALITY_MID ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_Quality( &quality ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_QuantizationTable( JPEG_QUANT_TABLE_0, &quant ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_DC_0, &huffman ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HeaderMode( JPEG_HEADER_ON ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_IrqPriority( UT_JPEG_IRQ_PRIO ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_IrqPriority( &irqPrio ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, &size ) );

    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CR );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}

/* ========================= IMAGE, QUALITY, TABLES ========================= */

/**
 * \brief   Jpeg_Get_RawSize() calculates the size of the raw data of an image.
 *
 * \par Expected results
 * - The image is padded to whole MCUs: 8 x 8 pixels with 1, 3 or 4 blocks, 16 x 8 pixels with 4 blocks and 16 x 16
 *   pixels with 6 blocks, 64 bytes per block. The count of the MCUs of the image is reported by Jpeg_Get_ImageInfo().
 */
void Ut_Jpeg_Get_RawSize_AllColorSpaces_ReturnsMcuBytes( void )
{
#if defined(JPEG)
    const uint32_t caseCnt = sizeof( utJpeg_RawCases ) / sizeof( utJpeg_RawCases[ 0u ] );

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    for( uint32_t caseIdx = 0u; caseCnt > caseIdx; caseIdx++ )
    {
        const utJpeg_RawCase_t * const rawCase = &utJpeg_RawCases[ caseIdx ];
        jpeg_ImageConfig_t image = UtJpeg_Get_Image( rawCase->ColorSpace, rawCase->Subsampling, UT_JPEG_QUALITY_MID );
        jpeg_Size_t        rawSize = 0u;

        image.Width  = rawCase->Width;
        image.Height = rawCase->Height;

        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_RawSize( &image, &rawSize ) );
        TEST_ASSERT_EQUAL_UINT32( rawCase->RawSize, rawSize );
    }
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Get_RawSize() rejects an invalid image.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR for NULL pointers, a zero width or height, an invalid color space, subsampling or quality,
 *   a subsampling other than 4:4:4 for the other color spaces than YCbCr and a size that does not fit 32 bits.
 */
void Ut_Jpeg_Get_RawSize_InvalidImage_ReturnsError( void )
{
#if defined(JPEG)
    jpeg_ImageConfig_t image   = UtJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, UT_JPEG_QUALITY_MID );
    jpeg_Size_t        rawSize = 0u;

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( NULL, &rawSize ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, NULL ) );

    image.Width = 0u;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, &rawSize ) );

    image = UtJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, UT_JPEG_QUALITY_MID );
    image.Height = 0u;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, &rawSize ) );

    image = UtJpeg_Get_Image( JPEG_COLOR_CNT, JPEG_SUBSAMPLING_444, UT_JPEG_QUALITY_MID );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, &rawSize ) );

    image = UtJpeg_Get_Image( JPEG_COLOR_YCBCR, JPEG_SUBSAMPLING_CNT, UT_JPEG_QUALITY_MID );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, &rawSize ) );

    image = UtJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, UT_JPEG_QUALITY_OVER );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, &rawSize ) );

    image = UtJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_422, UT_JPEG_QUALITY_MID );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, &rawSize ) );

    image = UtJpeg_Get_Image( JPEG_COLOR_RGB, JPEG_SUBSAMPLING_420, UT_JPEG_QUALITY_MID );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, &rawSize ) );

    image = UtJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, UT_JPEG_QUALITY_MID );
    image.Width  = UT_JPEG_DIMENSION_MAX;
    image.Height = UT_JPEG_DIMENSION_MAX;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, &rawSize ) );

    image = UtJpeg_Get_Image( JPEG_COLOR_YCBCR, JPEG_SUBSAMPLING_420, UT_JPEG_QUALITY_MID );
    image.Width  = UT_JPEG_DIMENSION_MAX;
    image.Height = UT_JPEG_DIMENSION_MAX;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_RawSize( &image, &rawSize ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Set_Quality() and Jpeg_Get_Quality() keep the quality factor of the module.
 *
 * \par Expected results
 * - 1 and 100 are accepted and read back, 0 and 101 are rejected and the value is kept, a NULL pointer is rejected.
 */
void Ut_Jpeg_Set_Quality_ValidAndInvalid_StoresQuality( void )
{
#if defined(JPEG)
    jpeg_Quality_t quality = 0u;

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Quality( UT_JPEG_QUALITY_LOW ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_Quality( &quality ) );
    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_QUALITY_LOW, quality );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Quality( UT_JPEG_QUALITY_HIGH ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_Quality( &quality ) );
    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_QUALITY_HIGH, quality );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Quality( 0u ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Quality( UT_JPEG_QUALITY_OVER ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_Quality( &quality ) );
    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_QUALITY_HIGH, quality );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_Quality( NULL ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Set_QuantizationTable() rejects invalid requests.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR for an invalid table, a NULL pointer, a zero coefficient and while a job runs.
 */
void Ut_Jpeg_Set_QuantizationTable_Invalid_ReturnsError( void )
{
#if defined(JPEG)
    jpeg_QuantTable_t table = jpeg_StdQuantTable[ JPEG_STD_QUANT_LUM ];

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_QuantizationTable( JPEG_QUANT_TABLE_CNT, &table ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_QuantizationTable( JPEG_QUANT_TABLE_0, NULL ) );

    table.Values[ JPEG_QUANT_TABLE_SIZE - 1u ] = 0u;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_QuantizationTable( JPEG_QUANT_TABLE_0, &table ) );

    table = jpeg_StdQuantTable[ JPEG_STD_QUANT_LUM ];
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_QuantizationTable( JPEG_QUANT_TABLE_3, &table ) );

    UtJpeg_StartJob();
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_QuantizationTable( JPEG_QUANT_TABLE_0, &table ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   A custom quantization table is scaled by the quality factor and written to the codec.
 *
 * \par Expected results
 * - A table of the coefficients 2: quality 50 (scale 100 %) writes the words 0x02020202, quality 100 (scale 0 %)
 *   writes the lowest coefficient 1 (0x01010101).
 */
void Ut_Jpeg_Set_QuantizationTable_Custom_IsScaledAndWritten( void )
{
#if defined(JPEG)
    jpeg_QuantTable_t table;
    jpeg_ImageConfig_t image = UtJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, UT_JPEG_QUALITY_MID );

    for( uint32_t idx = 0u; JPEG_QUANT_TABLE_SIZE > idx; idx++ )
    {
        table.Values[ idx ] = UT_JPEG_CUSTOM_COEF;
    }

    UtJpeg_Init( JPEG_XFER_MODE_POLL );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_QuantizationTable( JPEG_QUANT_TABLE_0, &table ) );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    for( uint32_t wordIdx = 0u; JPEG_REG_QUANT_WORD_CNT > wordIdx; wordIdx++ )
    {
        TEST_ASSERT_EQUAL_HEX32( UT_JPEG_CUSTOM_WORD, JPEG->QMEM0[ wordIdx ] );
    }

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );
    image.Quality = UT_JPEG_QUALITY_HIGH;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    for( uint32_t wordIdx = 0u; JPEG_REG_QUANT_WORD_CNT > wordIdx; wordIdx++ )
    {
        TEST_ASSERT_EQUAL_HEX32( UT_JPEG_ONES_WORD, JPEG->QMEM0[ wordIdx ] );
    }
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Set_HuffmanTable() rejects invalid tables.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR for an invalid table, a NULL pointer, a table without symbols, a DC table with a symbol above
 *   11, an AC table with the size 11, a repeated symbol, more codes than the length allows and while a job runs.
 */
void Ut_Jpeg_Set_HuffmanTable_Invalid_ReturnsError( void )
{
#if defined(JPEG)
    jpeg_HuffmanTable_t table = jpeg_StdHuffmanTable[ JPEG_HUFFMAN_AC_0 ];

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_CNT, &table ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_AC_0, NULL ) );

    for( uint32_t idx = 0u; JPEG_HUFFMAN_LENGTH_CNT > idx; idx++ )
    {
        table.Bits[ idx ] = 0u;
    }
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_AC_0, &table ) );

    table = jpeg_StdHuffmanTable[ JPEG_HUFFMAN_DC_0 ];
    table.Values[ 1u ] = UT_JPEG_SYMBOL_DC_BAD;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_DC_0, &table ) );
    table.Values[ 1u ] = UT_JPEG_SYMBOL_DC_MAX;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_DC_0, &table ) );

    table = jpeg_StdHuffmanTable[ JPEG_HUFFMAN_AC_0 ];
    table.Values[ 0u ] = UT_JPEG_SYMBOL_BAD_SIZE;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_AC_0, &table ) );

    table = jpeg_StdHuffmanTable[ JPEG_HUFFMAN_AC_0 ];
    table.Values[ 1u ] = table.Values[ 0u ];
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_AC_0, &table ) );

    table = jpeg_StdHuffmanTable[ JPEG_HUFFMAN_DC_0 ];
    table.Bits[ 0u ] = UT_JPEG_BITS_TOO_MANY;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_DC_0, &table ) );

    table = jpeg_StdHuffmanTable[ JPEG_HUFFMAN_DC_0 ];
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_DC_0, &table ) );

    UtJpeg_StartJob();
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_DC_0, &table ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   A custom Huffman table is converted and written to the encoder memory and to the DHT memory.
 *
 * \par Expected results
 * - A DC table with one code of 2 bits for the category 0 and an AC table with one code of 2 bits for the symbol 0x01:
 *   the first word of the memories holds the entry (length - 1 = 1, code 0) and the empty entry; the other entries of
 *   the DC memory are empty; the DHT memory starts with the code counts of the table; the other tables are the sample
 *   tables.
 */
void Ut_Jpeg_Set_HuffmanTable_Valid_IsWrittenToEncoder( void )
{
#if defined(JPEG)
    jpeg_HuffmanTable_t dcTable = { { 0u }, { 0u } };
    jpeg_HuffmanTable_t acTable = { { 0u }, { 0u } };
    jpeg_ImageConfig_t  image   = UtJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, UT_JPEG_QUALITY_MID );

    dcTable.Bits[ 1u ]   = 1u;
    dcTable.Values[ 0u ] = 0u;
    acTable.Bits[ 1u ]   = 1u;
    acTable.Values[ 0u ] = UT_JPEG_AC_SYMBOL;

    UtJpeg_Init( JPEG_XFER_MODE_POLL );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_DC_0, &dcTable ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_AC_0, &acTable ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );

    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_CUSTOM_DC_WORD, JPEG->HUFFENC_DC0[ 0u ] );
    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_EMPTY_WORD, JPEG->HUFFENC_DC0[ 1u ] );
    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_EMPTY_WORD, JPEG->HUFFENC_DC0[ UT_JPEG_HUFFENC_DC_WORDS - 1u ] );
    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_CUSTOM_AC_WORD, JPEG->HUFFENC_AC0[ 0u ] );
    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_EMPTY_WORD, JPEG->HUFFENC_AC0[ 1u ] );
    UtJpeg_AssertWords( JPEG->HUFFENC_DC1, utJpeg_HuffEncDc1, UT_JPEG_HUFFENC_DC_WORDS );
    UtJpeg_AssertWords( JPEG->HUFFENC_AC1, utJpeg_HuffEncAc1, UT_JPEG_HUFFENC_AC_WORDS );
    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_CUSTOM_DHT_WORD, JPEG->DHTMEM[ 0u ] );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Set_HeaderMode() sets the header generation of the encoder.
 *
 * \par Expected results
 * - Both modes are accepted, an invalid mode and a request while a job runs are rejected.
 */
void Ut_Jpeg_Set_HeaderMode_ValidAndInvalid( void )
{
#if defined(JPEG)
    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_HeaderMode( JPEG_HEADER_OFF ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_HeaderMode( JPEG_HEADER_ON ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HeaderMode( JPEG_HEADER_CNT ) );

    UtJpeg_StartJob();
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_HeaderMode( JPEG_HEADER_OFF ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}

/* ============================== ENCODER START ============================= */

/**
 * \brief   Jpeg_Set_EncodeStart() programs the image registers for every color space and subsampling.
 *
 * \par Expected results
 * - JPEG_CONFR1 (encoder, header, count of the components, scan components, color space = count of the quantization
 *   tables - 1, YSIZE), JPEG_CONFR2 (count of the MCUs - 1), JPEG_CONFR3 (XSIZE) and the registers of the components
 *   JPEG_CONFR4 to JPEG_CONFR7 (sampling factors, blocks - 1, quantization table, AC and DC Huffman table; the unused
 *   components are zero), JPEG_CONFR0.START is set and the quantization tables the color space needs are written.
 */
void Ut_Jpeg_Set_EncodeStart_ColorSpaces_ProgramImageRegisters( void )
{
#if defined(JPEG)
    const uint32_t caseCnt = sizeof( utJpeg_ColorCases ) / sizeof( utJpeg_ColorCases[ 0u ] );

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    for( uint32_t caseIdx = 0u; caseCnt > caseIdx; caseIdx++ )
    {
        const utJpeg_ColorCase_t * const colorCase = &utJpeg_ColorCases[ caseIdx ];
        const jpeg_ImageConfig_t image = UtJpeg_Get_Image( colorCase->ColorSpace,
                                                           colorCase->Subsampling,
                                                           UT_JPEG_QUALITY_MID );
        const uint32_t           confr1 = colorCase->Confr1 | ( (uint32_t)image.Height << JPEG_CONFR1_YSIZE_Pos );
        jpeg_ImageInfo_t         info;

        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_ImageInfo( &info ) );

        TEST_ASSERT_EQUAL_HEX32( confr1, JPEG->CONFR1 );
        TEST_ASSERT_EQUAL_UINT32( info.McuCount - 1u, JPEG->CONFR2 );
        TEST_ASSERT_EQUAL_HEX32( (uint32_t)image.Width << JPEG_CONFR3_XSIZE_Pos, JPEG->CONFR3 );
        TEST_ASSERT_EQUAL_HEX32( colorCase->Confr4, JPEG->CONFR4 );
        TEST_ASSERT_EQUAL_HEX32( colorCase->Confr5, JPEG->CONFR5 );
        TEST_ASSERT_EQUAL_HEX32( colorCase->Confr6, JPEG->CONFR6 );
        TEST_ASSERT_EQUAL_HEX32( colorCase->Confr7, JPEG->CONFR7 );
        TEST_ASSERT_EQUAL_UINT32( JPEG_CONFR0_START, JPEG->CONFR0 & JPEG_CONFR0_START );

        UtJpeg_AssertQuantMemory( JPEG_QUANT_TABLE_0, utJpeg_QuantLumQ50 );

        if( 1u < colorCase->Quant )
        {
            UtJpeg_AssertQuantMemory( JPEG_QUANT_TABLE_1, utJpeg_QuantChromQ50 );
        }
        else
        {
            /* One quantization table */
        }

        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );
    }
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Set_HeaderMode( JPEG_HEADER_OFF ) clears the header bit of the encoder.
 *
 * \par Expected results
 * - JPEG_CONFR1.HDR is set by default and clear after the header generation was switched off.
 */
void Ut_Jpeg_Set_EncodeStart_HeaderOff_ClearsHeaderBit( void )
{
#if defined(JPEG)
    const jpeg_ImageConfig_t image = UtJpeg_Get_Image( JPEG_COLOR_GRAYSCALE,
                                                       JPEG_SUBSAMPLING_444,
                                                       UT_JPEG_QUALITY_MID );

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    TEST_ASSERT_EQUAL_UINT32( JPEG_CONFR1_HDR, JPEG->CONFR1 & JPEG_CONFR1_HDR );
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CONFR1 & JPEG_CONFR1_DE );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_HeaderMode( JPEG_HEADER_OFF ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CONFR1 & JPEG_CONFR1_HDR );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The quantization memory receives the sample tables scaled by the quality factor in the zigzag order.
 *
 * \par Expected results
 * - Quality 50: the sample table unchanged (first word 0x0E0C0B10, the coefficients 16, 11, 12, 14 of the zigzag
 *   order), quality 1: the coefficients limited to 255, quality 100: all coefficients 1, the quality of the module
 *   (default 75, changed by Jpeg_Set_Quality) when the image has JPEG_QUALITY_MODULE. The chrominance table is table 1.
 */
void Ut_Jpeg_Set_EncodeStart_SampleTables_WritesQuantMemory( void )
{
#if defined(JPEG)
    jpeg_ImageConfig_t image = UtJpeg_Get_Image( JPEG_COLOR_YCBCR, JPEG_SUBSAMPLING_444, UT_JPEG_QUALITY_MID );

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_QUANT_LUM_Q50_WORD0, JPEG->QMEM0[ 0u ] );
    UtJpeg_AssertQuantMemory( JPEG_QUANT_TABLE_0, utJpeg_QuantLumQ50 );
    UtJpeg_AssertQuantMemory( JPEG_QUANT_TABLE_1, utJpeg_QuantChromQ50 );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );

    image.Quality = UT_JPEG_QUALITY_LOW;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    UtJpeg_AssertQuantMemory( JPEG_QUANT_TABLE_0, utJpeg_QuantLumQ1 );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );

    image.Quality = UT_JPEG_QUALITY_HIGH;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    UtJpeg_AssertQuantMemory( JPEG_QUANT_TABLE_0, utJpeg_QuantLumQ100 );
    UtJpeg_AssertQuantMemory( JPEG_QUANT_TABLE_1, utJpeg_QuantChromQ100 );
    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_ONES_WORD, JPEG->QMEM0[ 0u ] );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );

    image.Quality = JPEG_QUALITY_MODULE;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    UtJpeg_AssertQuantMemory( JPEG_QUANT_TABLE_0, utJpeg_QuantLumQ75 );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Quality( UT_JPEG_QUALITY_MID ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    UtJpeg_AssertQuantMemory( JPEG_QUANT_TABLE_0, utJpeg_QuantLumQ50 );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The Huffman encoder memories and the DHT memory receive the sample tables.
 *
 * \par Expected results
 * - The words of the four encoder memories (the code length - 1 and the low eight bits of the code of every symbol, two
 *   per word, the fixed words behind them) and the 103 words of the DHT memory match the images calculated
 *   independently from ITU-T T.81 Annex K and C.
 */
void Ut_Jpeg_Set_EncodeStart_SampleTables_WritesHuffmanAndDhtMemory( void )
{
#if defined(JPEG)
    const jpeg_ImageConfig_t image = UtJpeg_Get_Image( JPEG_COLOR_YCBCR, JPEG_SUBSAMPLING_420, UT_JPEG_QUALITY_MID );

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    UtJpeg_AssertWords( JPEG->HUFFENC_DC0, utJpeg_HuffEncDc0, UT_JPEG_HUFFENC_DC_WORDS );
    UtJpeg_AssertWords( JPEG->HUFFENC_DC1, utJpeg_HuffEncDc1, UT_JPEG_HUFFENC_DC_WORDS );
    UtJpeg_AssertWords( JPEG->HUFFENC_AC0, utJpeg_HuffEncAc0, UT_JPEG_HUFFENC_AC_WORDS );
    UtJpeg_AssertWords( JPEG->HUFFENC_AC1, utJpeg_HuffEncAc1, UT_JPEG_HUFFENC_AC_WORDS );
    UtJpeg_AssertWords( JPEG->DHTMEM, utJpeg_Dht, JPEG_REG_DHT_WORD_CNT );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Set_EncodeStart() rejects invalid requests without any register access.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR for a NULL pointer, an invalid image, a running job, a codec that does not stop (the state
 *   stays idle), and, with the sample tables not loaded, for every image until the tables the color space needs are
 *   set (the YCbCr tables 1 are needed for YCbCr only).
 */
void Ut_Jpeg_Set_EncodeStart_Invalid_ReturnsError( void )
{
#if defined(JPEG)
    jpeg_ImageConfig_t  image   = UtJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, UT_JPEG_QUALITY_MID );
    jpeg_Config_t       config  = UtJpeg_Get_Config( JPEG_XFER_MODE_POLL );
    jpeg_QuantTable_t   quant   = jpeg_StdQuantTable[ JPEG_STD_QUANT_LUM ];
    jpeg_State_t        state   = JPEG_STATE_CNT;
    jpeg_HuffmanTable_t dcTable = jpeg_StdHuffmanTable[ JPEG_HUFFMAN_DC_0 ];
    jpeg_HuffmanTable_t acTable = jpeg_StdHuffmanTable[ JPEG_HUFFMAN_AC_0 ];

    config.StandardTables = JPEG_FUNCTION_INACTIVE;
    UtJpeg_Expect_Init( JPEG_XFER_MODE_POLL );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Init( &config ) );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_EncodeStart( NULL ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_EncodeStart( &image ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CONFR1 );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_QuantizationTable( JPEG_QUANT_TABLE_0, &quant ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_EncodeStart( &image ) );

    quant = jpeg_StdQuantTable[ JPEG_STD_QUANT_LUM ];
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_DC_0, &dcTable ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_HuffmanTable( JPEG_HUFFMAN_AC_0, &acTable ) );

    image.Width = 0u;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_EncodeStart( &image ) );
    image.Width = UT_JPEG_WIDTH;

    image.ColorSpace = JPEG_COLOR_YCBCR;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_EncodeStart( &image ) );
    image.ColorSpace = JPEG_COLOR_GRAYSCALE;

    JPEG->SR = JPEG_SR_COF;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_EncodeStart( &image ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
    JPEG->SR = 0u;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_EncodeStart( &image ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_DecodeStart() );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   After Jpeg_Set_EncodeStart() the state and the image parameters are available.
 *
 * \par Expected results
 * - Jpeg_Get_ImageInfo() fails before the first job; afterwards the state is encoding and the parameters are the
 *   ones of
 *   the image (the quality is the quality of the image or of the module), the count of the MCUs and the raw size.
 */
void Ut_Jpeg_Set_EncodeStart_Valid_ReportsStateAndImageInfo( void )
{
#if defined(JPEG)
    jpeg_ImageConfig_t image = UtJpeg_Get_Image( JPEG_COLOR_YCBCR, JPEG_SUBSAMPLING_420, JPEG_QUALITY_MODULE );
    jpeg_ImageInfo_t   info;
    jpeg_State_t       state = JPEG_STATE_CNT;
    jpeg_Size_t        length = 1u;

    image.Width = UT_JPEG_WIDTH_WIDE;

    UtJpeg_Init( JPEG_XFER_MODE_POLL );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_ImageInfo( &info ) );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_ENCODING, state );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_ImageInfo( NULL ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_ImageInfo( &info ) );
    TEST_ASSERT_EQUAL_UINT16( UT_JPEG_WIDTH_WIDE, info.Width );
    TEST_ASSERT_EQUAL_UINT16( UT_JPEG_HEIGHT, info.Height );
    TEST_ASSERT_EQUAL( JPEG_COLOR_YCBCR, info.ColorSpace );
    TEST_ASSERT_EQUAL( JPEG_SUBSAMPLING_420, info.Subsampling );
    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_QUALITY_DEFAULT, info.Quality );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_MCU_WIDE, info.McuCount );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_RAW_WIDE, info.RawSize );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_OutputLength( &length ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, length );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_OutputLength( NULL ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}

/* =============================== POLL STREAMS ============================= */

/**
 * \brief   Jpeg_Task() moves the words of the input buffer into the input FIFO.
 *
 * \par Expected results
 * - With the input FIFO not full the word is written to JPEG_DIR (the first byte in the lowest byte), the input needed
 *   callback is called once and the next buffer is accepted.
 */
void Ut_Jpeg_Task_InputBuffer_WritesWordsAndCallsInputNeeded( void )
{
#if defined(JPEG)
    UtJpeg_StartEncode( JPEG_XFER_MODE_POLL );

    utJpeg_InBuf[ 0u ] = UT_JPEG_IN_BYTE0;
    utJpeg_InBuf[ 1u ] = UT_JPEG_IN_BYTE1;
    utJpeg_InBuf[ UT_JPEG_BYTE_IDX2 ] = UT_JPEG_IN_BYTE2;
    utJpeg_InBuf[ UT_JPEG_BYTE_IDX3 ] = UT_JPEG_IN_BYTE3;
    JPEG->DIR = UT_JPEG_DIR_EMPTY;
    JPEG->SR  = JPEG_SR_IFNFF;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_InputCnt );

    Jpeg_Task();

    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_IN_WORD, JPEG->DIR );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_InputCnt );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );

    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_TWICE, utJpeg_InputCnt );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The last word of an input buffer whose size is not a multiple of 4 is padded with zeros.
 *
 * \par Expected results
 * - JPEG_DIR receives the three bytes in the three lowest bytes and zero in the highest one.
 */
void Ut_Jpeg_Task_InputBufferPartialWord_PadsWithZeros( void )
{
#if defined(JPEG)
    UtJpeg_StartEncode( JPEG_XFER_MODE_POLL );

    utJpeg_InBuf[ 0u ] = UT_JPEG_IN_BYTE0;
    utJpeg_InBuf[ 1u ] = UT_JPEG_IN_BYTE1;
    utJpeg_InBuf[ UT_JPEG_BYTE_IDX2 ] = UT_JPEG_IN_BYTE2;
    utJpeg_InBuf[ UT_JPEG_BYTE_IDX3 ] = UT_JPEG_IN_BYTE3;
    JPEG->DIR = UT_JPEG_DIR_EMPTY;
    JPEG->SR  = JPEG_SR_IFNFF;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_BUF_TAIL ) );
    Jpeg_Task();

    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_IN_WORD_TAIL, JPEG->DIR );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_InputCnt );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   A full input FIFO keeps the input buffer waiting.
 *
 * \par Expected results
 * - Nothing is written to JPEG_DIR, the callback is not called, a second buffer is rejected; when the FIFO has room
 *   the buffer is consumed.
 */
void Ut_Jpeg_Task_InputFifoFull_KeepsBufferPending( void )
{
#if defined(JPEG)
    UtJpeg_StartEncode( JPEG_XFER_MODE_POLL );

    JPEG->DIR = UT_JPEG_DIR_EMPTY;
    JPEG->SR  = 0u;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );
    Jpeg_Task();

    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_DIR_EMPTY, JPEG->DIR );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_InputCnt );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );

    JPEG->SR = JPEG_SR_IFNFF;
    Jpeg_Task();

    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_InputCnt );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   One call of Jpeg_Task() moves at most the size of the FIFO (8 words) of an input buffer.
 *
 * \par Expected results
 * - A buffer of 10 words: the first call writes 8 words (the last one is the word with the index 7), the callback is
 *   not
 *   called; the second call writes the rest (the last word is the one with the index 9) and calls the callback.
 */
void Ut_Jpeg_Task_InputBufferLarge_MovesFifoSizePerCall( void )
{
#if defined(JPEG)
    UtJpeg_StartEncode( JPEG_XFER_MODE_POLL );

    for( uint32_t idx = 0u; UT_JPEG_BUF_LARGE > idx; idx++ )
    {
        utJpeg_InBuf[ idx ] = (jpeg_Data_t)( idx + 1u );
    }

    JPEG->DIR = UT_JPEG_DIR_EMPTY;
    JPEG->SR  = JPEG_SR_IFNFF;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_BUF_LARGE ) );
    Jpeg_Task();

    TEST_ASSERT_EQUAL_HEX32( UtJpeg_Get_Word( utJpeg_InBuf, UT_JPEG_FIFO_WORDS - 1u ), JPEG->DIR );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_InputCnt );

    Jpeg_Task();

    TEST_ASSERT_EQUAL_HEX32( UtJpeg_Get_Word( utJpeg_InBuf, ( UT_JPEG_BUF_LARGE / UT_JPEG_WORD ) - 1u ), JPEG->DIR );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_InputCnt );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Set_InputBuffer() rejects invalid requests.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR when no job runs, for a NULL pointer, a zero size and while a buffer waits.
 */
void Ut_Jpeg_Set_InputBuffer_InvalidParams_ReturnsError( void )
{
#if defined(JPEG)
    UtJpeg_Init( JPEG_XFER_MODE_POLL );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );

    UtJpeg_StartJob();
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( NULL, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( utJpeg_InBuf, 0u ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Task() stores the words of the output FIFO in the output buffer.
 *
 * \par Expected results
 * - The bytes of every word are stored from the lowest one, a full buffer calls the output ready callback with its
 *   size,
 *   the total count of the produced bytes is available, the next buffer is accepted.
 */
void Ut_Jpeg_Task_OutputBuffer_StoresLittleEndianWords( void )
{
#if defined(JPEG)
    jpeg_Size_t length = 0u;

    UtJpeg_StartEncode( JPEG_XFER_MODE_POLL );

    JPEG->DOR = UT_JPEG_OUT_WORD;
    JPEG->SR  = JPEG_SR_OFNEF;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_BUF_SMALL ) );
    Jpeg_Task();

    for( uint32_t idx = 0u; UT_JPEG_BUF_SMALL > idx; idx += UT_JPEG_WORD )
    {
        TEST_ASSERT_EQUAL_UINT8( UT_JPEG_OUT_BYTE0, utJpeg_OutBuf[ idx ] );
        TEST_ASSERT_EQUAL_UINT8( UT_JPEG_OUT_BYTE1, utJpeg_OutBuf[ idx + 1u ] );
        TEST_ASSERT_EQUAL_UINT8( UT_JPEG_OUT_BYTE2, utJpeg_OutBuf[ idx + UT_JPEG_BYTE_IDX2 ] );
        TEST_ASSERT_EQUAL_UINT8( UT_JPEG_OUT_BYTE3, utJpeg_OutBuf[ idx + UT_JPEG_BYTE_IDX3 ] );
    }

    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_FILL_PATTERN, utJpeg_OutBuf[ UT_JPEG_BUF_SMALL ] );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_OutputCnt );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_SMALL, utJpeg_OutputLast );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_OutputLength( &length ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_SMALL, length );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( UT_JPEG_OUT_SECOND, UT_JPEG_BUF_SMALL ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Set_OutputBuffer() rejects invalid requests.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR when no job runs, for a NULL pointer, a size that is not a multiple of 4 bytes, a size below
 *   4 bytes and while a buffer waits.
 */
void Ut_Jpeg_Set_OutputBuffer_InvalidParams_ReturnsError( void )
{
#if defined(JPEG)
    UtJpeg_Init( JPEG_XFER_MODE_POLL );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_WORD ) );

    UtJpeg_StartJob();
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_OutputBuffer( NULL, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, 0u ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_BUF_TAIL ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_BUF_UNALIGNED ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_WORD ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   An empty output FIFO leaves the output buffer unchanged.
 *
 * \par Expected results
 * - No byte is stored, no callback is called, the produced length is zero.
 */
void Ut_Jpeg_Task_OutputFifoEmpty_StoresNothing( void )
{
#if defined(JPEG)
    jpeg_Size_t length = 1u;

    UtJpeg_StartEncode( JPEG_XFER_MODE_POLL );

    JPEG->DOR = UT_JPEG_OUT_WORD;
    JPEG->SR  = 0u;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_BUF_SMALL ) );
    Jpeg_Task();

    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_FILL_PATTERN, utJpeg_OutBuf[ 0u ] );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_OutputCnt );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_OutputLength( &length ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, length );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The end of the conversion with an empty output FIFO ends the job and reports the partial output buffer.
 *
 * \par Expected results
 * - A buffer of 64 bytes receives 32 bytes (one call moves 8 words); with JPEG_SR.EOCF set and the FIFO empty the
 *   output
 *   ready callback reports 32 bytes, the end callback reports the total 32, the state is idle, the codec is stopped
 *   (START clear, FIFOs flushed, interrupts and DMA requests disabled).
 */
void Ut_Jpeg_Task_EndOfConversion_ReportsPartialBufferAndEnds( void )
{
#if defined(JPEG)
    jpeg_State_t state  = JPEG_STATE_CNT;
    jpeg_Size_t  length = 0u;

    UtJpeg_StartEncode( JPEG_XFER_MODE_POLL );

    JPEG->DOR = UT_JPEG_OUT_WORD;
    JPEG->SR  = JPEG_SR_OFNEF;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_BUF_MID ) );
    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_OutputCnt );

    JPEG->SR = JPEG_SR_EOCF;
    Jpeg_Task();

    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_OutputCnt );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_FIFO, utJpeg_OutputLast );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_EndCnt );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_FIFO, utJpeg_EndLast );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CONFR0 & JPEG_CONFR0_START );
    TEST_ASSERT_EQUAL_UINT32( ( JPEG_CR_IFF | JPEG_CR_OFF | JPEG_CR_JCEN ), JPEG->CR );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_OutputLength( &length ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_FIFO, length );

    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_EndCnt );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The end of the conversion waits until the output FIFO is emptied.
 *
 * \par Expected results
 * - With JPEG_SR.EOCF and OFNEF set and a full buffer the job goes on (no end callback); after the next buffer was
 *   given and the FIFO is empty the job ends, the buffer without data is not reported again.
 */
void Ut_Jpeg_Task_EndOfConversionFifoNotEmpty_WaitsForDrain( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    UtJpeg_StartEncode( JPEG_XFER_MODE_POLL );

    JPEG->DOR = UT_JPEG_OUT_WORD;
    JPEG->SR  = JPEG_SR_OFNEF | JPEG_SR_EOCF;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_BUF_SMALL ) );
    Jpeg_Task();

    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_OutputCnt );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_EndCnt );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_ENCODING, state );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( UT_JPEG_OUT_SECOND, UT_JPEG_BUF_SMALL ) );
    JPEG->SR = JPEG_SR_EOCF;
    Jpeg_Task();

    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_OutputCnt );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_EndCnt );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_SMALL, utJpeg_EndLast );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Set_Pause() stops a stream and Jpeg_Set_Resume() continues it.
 *
 * \par Expected results
 * - A paused input stream writes nothing to JPEG_DIR, a paused output stream stores nothing; after the resume the
 *   buffers are served. An invalid stream and a request while no job runs are rejected.
 */
void Ut_Jpeg_Task_PauseAndResume_StopAndContinueStream( void )
{
#if defined(JPEG)
    UtJpeg_Init( JPEG_XFER_MODE_POLL );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Pause( JPEG_STREAM_INPUT ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Resume( JPEG_STREAM_INPUT ) );

    UtJpeg_StartJob();
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Pause( JPEG_STREAM_CNT ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Resume( JPEG_STREAM_CNT ) );

    JPEG->DIR = UT_JPEG_DIR_EMPTY;
    JPEG->DOR = UT_JPEG_OUT_WORD;
    JPEG->SR  = JPEG_SR_IFNFF | JPEG_SR_OFNEF;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Pause( JPEG_STREAM_INPUT ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Pause( JPEG_STREAM_OUTPUT ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_BUF_SMALL ) );
    Jpeg_Task();

    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_DIR_EMPTY, JPEG->DIR );
    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_FILL_PATTERN, utJpeg_OutBuf[ 0u ] );
    TEST_ASSERT_EQUAL_UINT32( 0u, UtJpeg_CountEvents() );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Resume( JPEG_STREAM_INPUT ) );
    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_InputCnt );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_OutputCnt );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Resume( JPEG_STREAM_OUTPUT ) );
    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_OutputCnt );
    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_OUT_BYTE0, utJpeg_OutBuf[ 0u ] );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Set_Abort() stops the running job without calling a callback.
 *
 * \par Expected results
 * - The codec is stopped (START clear, FIFOs flushed), the state is idle, no buffer is served any more, no callback is
 *   called, a second abort and the buffers of a stopped job are rejected, a new job can be started.
 */
void Ut_Jpeg_Set_Abort_RunningJob_StopsCodecWithoutCallbacks( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    UtJpeg_Init( JPEG_XFER_MODE_POLL );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Abort() );

    UtJpeg_StartJob();
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_BUF_SMALL ) );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CONFR0 & JPEG_CONFR0_START );
    TEST_ASSERT_EQUAL_UINT32( ( JPEG_CR_IFF | JPEG_CR_OFF | JPEG_CR_JCEN ), JPEG->CR );

    JPEG->SR = JPEG_SR_IFNFF | JPEG_SR_OFNEF | JPEG_SR_EOCF;
    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( 0u, UtJpeg_CountEvents() );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_Abort() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );
    JPEG->SR = 0u;
    UtJpeg_StartJob();
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}

/* ============================= INTERRUPT MODE ============================= */

/**
 * \brief   In the interrupt mode the interrupt enable bits follow the state of the job.
 *
 * \par Expected results
 * - After the start only the end of the conversion interrupt is enabled; a waiting input buffer enables the not full
 *   interrupt of the input FIFO, a waiting output buffer the not empty interrupt of the output FIFO; a paused stream
 *   disables its interrupt, a consumed input buffer disables the input interrupt.
 */
void Ut_Jpeg_Isr_Buffers_EnableAndDisableInterrupts( void )
{
#if defined(JPEG)
    UtJpeg_StartEncode( JPEG_XFER_MODE_ISR );
    TEST_ASSERT_EQUAL_HEX32( JPEG_CR_EOCIE, JPEG->CR & UT_JPEG_CR_IRQ_MASK );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );
    TEST_ASSERT_EQUAL_HEX32( ( JPEG_CR_EOCIE | JPEG_CR_IFNFIE ), JPEG->CR & UT_JPEG_CR_IRQ_MASK );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_BUF_SMALL ) );
    TEST_ASSERT_EQUAL_HEX32( ( JPEG_CR_EOCIE | JPEG_CR_IFNFIE | JPEG_CR_OFNEIE ), JPEG->CR & UT_JPEG_CR_IRQ_MASK );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Pause( JPEG_STREAM_INPUT ) );
    TEST_ASSERT_EQUAL_HEX32( ( JPEG_CR_EOCIE | JPEG_CR_OFNEIE ), JPEG->CR & UT_JPEG_CR_IRQ_MASK );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Pause( JPEG_STREAM_OUTPUT ) );
    TEST_ASSERT_EQUAL_HEX32( JPEG_CR_EOCIE, JPEG->CR & UT_JPEG_CR_IRQ_MASK );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Resume( JPEG_STREAM_INPUT ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Resume( JPEG_STREAM_OUTPUT ) );
    TEST_ASSERT_EQUAL_HEX32( ( JPEG_CR_EOCIE | JPEG_CR_IFNFIE | JPEG_CR_OFNEIE ), JPEG->CR & UT_JPEG_CR_IRQ_MASK );

    JPEG->SR = JPEG_SR_IFNFF;
    utJpeg_Isr();

    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_InputCnt );
    TEST_ASSERT_EQUAL_HEX32( ( JPEG_CR_EOCIE | JPEG_CR_OFNEIE ), JPEG->CR & UT_JPEG_CR_IRQ_MASK );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );
    TEST_ASSERT_EQUAL_HEX32( 0u, JPEG->CR & UT_JPEG_CR_IRQ_MASK );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The end of the conversion with data in the FIFO and no output buffer does not storm the interrupt.
 *
 * \par Expected results
 * - JPEG_SR.EOCF and OFNEF set and a full buffer: the end interrupt and the output interrupt are disabled until the
 *   next output buffer is given, then both are enabled again; the job ends when the FIFO is empty.
 */
void Ut_Jpeg_Isr_EndWithDataAndNoBuffer_DisablesEndInterruptUntilBuffer( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    UtJpeg_StartEncode( JPEG_XFER_MODE_ISR );

    JPEG->DOR = UT_JPEG_OUT_WORD;
    JPEG->SR  = JPEG_SR_OFNEF | JPEG_SR_EOCF;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_BUF_SMALL ) );
    utJpeg_Isr();

    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_OutputCnt );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_EndCnt );
    TEST_ASSERT_EQUAL_HEX32( 0u, JPEG->CR & UT_JPEG_CR_IRQ_MASK );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( UT_JPEG_OUT_SECOND, UT_JPEG_BUF_SMALL ) );
    TEST_ASSERT_EQUAL_HEX32( ( JPEG_CR_EOCIE | JPEG_CR_OFNEIE ), JPEG->CR & UT_JPEG_CR_IRQ_MASK );

    JPEG->SR = JPEG_SR_EOCF;
    utJpeg_Isr();

    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_EndCnt );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
    TEST_ASSERT_EQUAL_HEX32( 0u, JPEG->CR & UT_JPEG_CR_IRQ_MASK );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The interrupt handler does nothing when no job runs and is not used by the poll mode.
 *
 * \par Expected results
 * - A call of the registered handler without a job changes nothing and calls no callback; Jpeg_Task() in the interrupt
 *   mode does not serve the FIFOs.
 */
void Ut_Jpeg_Isr_Handler_NoJobOrIsrMode_DoesNothing( void )
{
#if defined(JPEG)
    UtJpeg_Init( JPEG_XFER_MODE_ISR );

    JPEG->SR = JPEG_SR_IFNFF | JPEG_SR_OFNEF | JPEG_SR_EOCF | JPEG_SR_HPDF;
    utJpeg_Isr();
    TEST_ASSERT_EQUAL_UINT32( 0u, UtJpeg_CountEvents() );

    UtJpeg_StartJob();
    JPEG->SR  = JPEG_SR_IFNFF;
    JPEG->DIR = UT_JPEG_DIR_EMPTY;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_WORD ) );
    Jpeg_Task();
    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_DIR_EMPTY, JPEG->DIR );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_InputCnt );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}

/* ================================= DECODER ================================ */

/**
 * \brief   Jpeg_Set_DecodeStart() programs the decoder.
 *
 * \par Expected results
 * - JPEG_CONFR1 has the decoder and the header bits, JPEG_CONFR0.START is set, the state is decoding, the image
 *   parameters are not available before the header was parsed; in the interrupt mode the end of the conversion and the
 *   header parsing interrupts are enabled.
 */
void Ut_Jpeg_Set_DecodeStart_Valid_ProgramsDecoder( void )
{
#if defined(JPEG)
    jpeg_State_t     state = JPEG_STATE_CNT;
    jpeg_ImageInfo_t info;

    UtJpeg_Init( JPEG_XFER_MODE_ISR );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
    TEST_ASSERT_EQUAL_HEX32( ( JPEG_CONFR1_DE | JPEG_CONFR1_HDR ), JPEG->CONFR1 );
    TEST_ASSERT_EQUAL_UINT32( JPEG_CONFR0_START, JPEG->CONFR0 & JPEG_CONFR0_START );
    TEST_ASSERT_EQUAL_HEX32( ( JPEG_CR_EOCIE | JPEG_CR_HPDIE ), JPEG->CR & UT_JPEG_CR_IRQ_MASK );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_DECODING, state );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_ImageInfo( &info ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Set_DecodeStart() rejects invalid requests.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR for a not initialized module, a running job and a codec that does not stop (the state stays
 *   idle).
 */
void Ut_Jpeg_Set_DecodeStart_Invalid_ReturnsError( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_DecodeStart() );

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    JPEG->SR = JPEG_SR_COF;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_DecodeStart() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
    JPEG->SR = 0u;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_DecodeStart() );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The first input buffer of the decoder with an unsupported frame ends the job.
 *
 * \par Expected results
 * - A progressive (SOF2), lossless (SOF3), arithmetic coded (SOF9) frame and a baseline frame of 12-bit samples:
 *   Jpeg_Set_InputBuffer() returns JPEG_REQUEST_ERROR, the error callback is called with JPEG_ERROR_UNSUPPORTED, the
 *   codec is stopped and the state is idle.
 */
void Ut_Jpeg_Set_InputBuffer_UnsupportedFrames_EndsJobWithUnsupported( void )
{
#if defined(JPEG)
    const uint32_t markers[]    = { UT_JPEG_UNSUPPORTED_MARKERS };
    const uint32_t precisions[] = { UT_JPEG_UNSUPPORTED_PRECISIONS };
    const uint32_t caseCnt      = sizeof( markers ) / sizeof( markers[ 0u ] );

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    for( uint32_t caseIdx = 0u; caseCnt > caseIdx; caseIdx++ )
    {
        jpeg_State_t state = JPEG_STATE_CNT;
        const uint32_t size = UtJpeg_Build_Header( utJpeg_InBuf, markers[ caseIdx ], precisions[ caseIdx ] );

        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
        TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( utJpeg_InBuf, size ) );
        TEST_ASSERT_EQUAL_UINT32( caseIdx + 1u, utJpeg_ErrorCnt );
        TEST_ASSERT_EQUAL( JPEG_ERROR_UNSUPPORTED, utJpeg_ErrorLast );
        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
        TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
        TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CONFR0 & JPEG_CONFR0_START );
    }
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The first input buffer of the decoder which is not the start of a JPEG stream ends the job.
 *
 * \par Expected results
 * - A buffer without the SOI marker, a marker segment without the marker prefix and a scan or the end of the image in
 *   front of the frame: JPEG_REQUEST_ERROR, the error callback is called with JPEG_ERROR_STREAM, the state is idle.
 */
void Ut_Jpeg_Set_InputBuffer_NotJpeg_EndsJobWithStreamError( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_BUF_SMALL ) );
    TEST_ASSERT_EQUAL( JPEG_ERROR_STREAM, utJpeg_ErrorLast );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_ErrorCnt );

    ( void )UtJpeg_Build_Header( utJpeg_InBuf, UT_JPEG_SOF0, UT_JPEG_PRECISION_8 );
    utJpeg_InBuf[ UT_JPEG_APP0_OFFSET ] = 0u;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_HEADER_SIZE ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_TWICE, utJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL( JPEG_ERROR_STREAM, utJpeg_ErrorLast );

    ( void )UtJpeg_Build_Header( utJpeg_InBuf, UT_JPEG_MARKER_SOS, UT_JPEG_PRECISION_8 );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_HEADER_SIZE ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_THREE_TIMES, utJpeg_ErrorCnt );

    ( void )UtJpeg_Build_Header( utJpeg_InBuf, UT_JPEG_MARKER_EOI, UT_JPEG_PRECISION_8 );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( utJpeg_InBuf, UT_JPEG_HEADER_SIZE ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_FOUR_TIMES, utJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The first input buffer of the decoder with a supported frame, or too short to tell, is accepted.
 *
 * \par Expected results
 * - A baseline frame (SOF0) and an extended sequential frame (SOF1) of 8-bit samples behind the markers APP0, DQT, TEM
 *   and RST0, a buffer with the SOI marker only, a buffer which ends inside a marker segment and a buffer which ends
 *   inside the start of frame: JPEG_REQUEST_OK, no error, the state stays decoding.
 */
void Ut_Jpeg_Set_InputBuffer_BaselineHeader_IsAccepted( void )
{
#if defined(JPEG)
    const uint32_t sizes[] = { UT_JPEG_ACCEPTED_SIZES };
    const uint32_t sizeCnt = sizeof( sizes ) / sizeof( sizes[ 0u ] );
    jpeg_State_t   state   = JPEG_STATE_CNT;

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    for( uint32_t sofIdx = 0u; UT_JPEG_TWICE > sofIdx; sofIdx++ )
    {
        const uint32_t marker = UT_JPEG_SOF0 + sofIdx;

        for( uint32_t sizeIdx = 0u; sizeCnt > sizeIdx; sizeIdx++ )
        {
            ( void )UtJpeg_Build_Header( utJpeg_InBuf, marker, UT_JPEG_PRECISION_8 );

            TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
            TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( utJpeg_InBuf, sizes[ sizeIdx ] ) );
            TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
            TEST_ASSERT_EQUAL( JPEG_STATE_DECODING, state );
            TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );
        }
    }

    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_ErrorCnt );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The parsed header is reported: the image parameters are read from the registers.
 *
 * \par Expected results
 * - JPEG_SR.HPDF set: the header callback is called once with the width, the height, the color space (1 component:
 * grayscale, 3: YCbCr, 4: CMYK), the subsampling (sampling factors 1 x 1, 2 x 1, 2 x 2), the count of the MCUs and
 *   the raw size; the flag is cleared (JPEG_CFR is exactly 0x40, CHPDF bit 6), Jpeg_Get_ImageInfo() returns the same
 *   parameters and the header interrupt is disabled.
 */
void Ut_Jpeg_Task_HeaderParsed_ReportsImageInfo( void )
{
#if defined(JPEG)
    const uint32_t components[] = { UT_JPEG_HEADER_COMPONENTS };
    const uint32_t horizontal[] = { UT_JPEG_HEADER_HORIZONTAL };
    const uint32_t vertical[]   = { UT_JPEG_HEADER_VERTICAL };
    const jpeg_ColorSpace_t colorSpaces[] = { UT_JPEG_HEADER_COLORS };
    const jpeg_Subsampling_t subsamplings[] = { UT_JPEG_HEADER_SUBSAMPLINGS };
    const jpeg_Count_t mcuCounts[] = { UT_JPEG_HEADER_MCUS };
    const jpeg_Size_t rawSizes[] = { UT_JPEG_HEADER_RAWS };
    const uint32_t caseCnt = sizeof( components ) / sizeof( components[ 0u ] );

    UtJpeg_Init( JPEG_XFER_MODE_ISR );

    for( uint32_t caseIdx = 0u; caseCnt > caseIdx; caseIdx++ )
    {
        jpeg_ImageInfo_t info;

        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
        UtJpeg_Preset_Header( components[ caseIdx ], horizontal[ caseIdx ], vertical[ caseIdx ] );
        JPEG->CFR = 0u;

        utJpeg_Isr();

        TEST_ASSERT_EQUAL_UINT32( caseIdx + 1u, utJpeg_HeaderCnt );
        TEST_ASSERT_EQUAL_UINT16( UT_JPEG_WIDTH_WIDE, utJpeg_HeaderInfo.Width );
        TEST_ASSERT_EQUAL_UINT16( UT_JPEG_HEIGHT, utJpeg_HeaderInfo.Height );
        TEST_ASSERT_EQUAL( colorSpaces[ caseIdx ], utJpeg_HeaderInfo.ColorSpace );
        TEST_ASSERT_EQUAL( subsamplings[ caseIdx ], utJpeg_HeaderInfo.Subsampling );
        TEST_ASSERT_EQUAL_UINT32( mcuCounts[ caseIdx ], utJpeg_HeaderInfo.McuCount );
        TEST_ASSERT_EQUAL_UINT32( rawSizes[ caseIdx ], utJpeg_HeaderInfo.RawSize );
        TEST_ASSERT_EQUAL_HEX32( UT_JPEG_CFR_CLEAR_HEADER, JPEG->CFR );
        TEST_ASSERT_EQUAL_HEX32( JPEG_CR_EOCIE, JPEG->CR & UT_JPEG_CR_IRQ_MASK );
        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_ImageInfo( &info ) );
        TEST_ASSERT_EQUAL_UINT16( UT_JPEG_WIDTH_WIDE, info.Width );
        TEST_ASSERT_EQUAL_UINT32( rawSizes[ caseIdx ], info.RawSize );

        utJpeg_Isr();
        TEST_ASSERT_EQUAL_UINT32( caseIdx + 1u, utJpeg_HeaderCnt );
        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );
    }
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   A parsed header the module can not describe ends the job.
 *
 * \par Expected results
 * - Two components, sampling factors 4 x 1 and a zero width: the header callback is not called, the error callback is
 *   called with JPEG_ERROR_UNSUPPORTED, the state is idle.
 */
void Ut_Jpeg_Task_HeaderParsedUnsupported_EndsJob( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    UtJpeg_Init( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
    UtJpeg_Preset_Header( UT_JPEG_NF_TWO, UT_JPEG_FACTOR_ONE, UT_JPEG_FACTOR_ONE );
    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL( JPEG_ERROR_UNSUPPORTED, utJpeg_ErrorLast );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
    UtJpeg_Preset_Header( UT_JPEG_NF_YCBCR, UT_JPEG_FACTOR_FOUR, UT_JPEG_FACTOR_ONE );
    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_TWICE, utJpeg_ErrorCnt );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
    UtJpeg_Preset_Header( UT_JPEG_NF_YCBCR, UT_JPEG_FACTOR_ONE, UT_JPEG_FACTOR_ONE );
    JPEG->CONFR3 = 0u;
    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_THREE_TIMES, utJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_HeaderCnt );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Get_ImageInfo( &utJpeg_HeaderInfo ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The quality factor of the parsed image is estimated from the quantization table 0.
 *
 * \par Expected results
 * - The tables the encoder wrote for the quality 50 (the sample luminance table) give the estimate 50, the tables for
 *   the quality 100 (all coefficients 1) give 100.
 */
void Ut_Jpeg_Task_HeaderParsed_EstimatesQuality( void )
{
#if defined(JPEG)
    const uint32_t qualities[] = { UT_JPEG_QUALITY_MID, UT_JPEG_QUALITY_HIGH };
    jpeg_Config_t  config      = UtJpeg_Get_Config( JPEG_XFER_MODE_POLL );

    UtJpeg_Expect_Init( JPEG_XFER_MODE_POLL );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Init( &config ) );

    for( uint32_t caseIdx = 0u; UT_JPEG_TWICE > caseIdx; caseIdx++ )
    {
        const jpeg_ImageConfig_t image = UtJpeg_Get_Image( JPEG_COLOR_YCBCR,
                                                           JPEG_SUBSAMPLING_420,
                                                           (jpeg_Quality_t)qualities[ caseIdx ] );

        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );

        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
        UtJpeg_Preset_Header( UT_JPEG_NF_YCBCR, UT_JPEG_FACTOR_TWO, UT_JPEG_FACTOR_TWO );
        Jpeg_Task();

        TEST_ASSERT_EQUAL_UINT32( caseIdx + 1u, utJpeg_HeaderCnt );
        TEST_ASSERT_EQUAL_UINT8( qualities[ caseIdx ], utJpeg_HeaderInfo.Quality );
        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );
    }
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   A decoder job which ends before its header was parsed ends with an error.
 *
 * \par Expected results
 * - JPEG_SR.EOCF set without the header: the error callback is called with JPEG_ERROR_STREAM, the end callback is not
 *   called, the state is idle.
 */
void Ut_Jpeg_Task_EndBeforeHeader_EndsWithStreamError( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    UtJpeg_StartDecode( JPEG_XFER_MODE_POLL );

    JPEG->SR = JPEG_SR_EOCF;
    Jpeg_Task();

    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL( JPEG_ERROR_STREAM, utJpeg_ErrorLast );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_EndCnt );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   A decoder job with a parsed header delivers the MCUs and ends with the end of the conversion.
 *
 * \par Expected results
 * - The header callback, then the full output buffer (32 bytes), then at JPEG_SR.EOCF the end callback with the total
 *   32 bytes; no error.
 */
void Ut_Jpeg_Task_DecodeEnd_ReportsOutputAndEnd( void )
{
#if defined(JPEG)
    UtJpeg_StartDecode( JPEG_XFER_MODE_POLL );

    UtJpeg_Preset_Header( UT_JPEG_NF_YCBCR, UT_JPEG_FACTOR_TWO, UT_JPEG_FACTOR_TWO );
    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_HeaderCnt );

    JPEG->DOR = UT_JPEG_OUT_WORD;
    JPEG->SR  = JPEG_SR_OFNEF;
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( utJpeg_OutBuf, UT_JPEG_BUF_FIFO ) );
    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_OutputCnt );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_FIFO, utJpeg_OutputLast );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_EndCnt );

    JPEG->SR = JPEG_SR_EOCF;
    Jpeg_Task();
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_EndCnt );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_FIFO, utJpeg_EndLast );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_ErrorCnt );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}

/* ================================= DMA MODE =============================== */

/**
 * \brief   In the DMA mode the job start enables the DMA requests of both FIFOs.
 *
 * \par Expected results
 * - JPEG_CR has IDMAEN and ODMAEN and, from the interrupts, only the end of the conversion interrupt.
 */
void Ut_Jpeg_Set_EncodeStart_Dma_EnablesDmaRequests( void )
{
#if defined(JPEG)
    UtJpeg_StartDmaEncode();

    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_CR_DMA_MASK, JPEG->CR & UT_JPEG_CR_DMA_MASK );
    TEST_ASSERT_EQUAL_HEX32( JPEG_CR_EOCIE, JPEG->CR & UT_JPEG_CR_IRQ_MASK );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );
    TEST_ASSERT_EQUAL_HEX32( 0u, JPEG->CR & ( UT_JPEG_CR_DMA_MASK | UT_JPEG_CR_IRQ_MASK ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_INACTIVE, 0u ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_INACTIVE, 1u ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Buffers in the DMA mode arm the Gpdma channels.
 *
 * \par Expected results
 * - The input buffer: transfer memory to JPEG_DIR, request 140, 32-bit words, the block size, the source address and
 *   the destination address JPEG_DIR written to channel 0 and the channel enabled; the output buffer: the same with
 *   JPEG_DOR to the memory, request 141 on channel 1. The buffer of the maximum size (65532 bytes) is accepted.
 */
void Ut_Jpeg_Set_InputBuffer_Dma_ArmsChannel( void )
{
#if defined(JPEG)
    jpeg_Data_t * const input  = UtJpeg_Get_DmaInput();
    jpeg_Data_t * const output = UtJpeg_Get_DmaOutput();

    UtJpeg_StartDmaEncode();
    UtJpeg_ResetGpdmaLog();

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( input, UT_JPEG_BUF_MID ) );
    TEST_ASSERT_EQUAL( GPDMA_DIR_MEMORY_TO_PERIPH, utJpeg_DmaXfer.Direction );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_DMA_REQUEST_INPUT, utJpeg_DmaXfer.RequestSource );
    TEST_ASSERT_EQUAL( GPDMA_DATA_SIZE_32BITS, utJpeg_DmaXfer.SourceDataSize );
    TEST_ASSERT_EQUAL( GPDMA_ADDR_INCREMENT, utJpeg_DmaXfer.SourceAddrMode );
    TEST_ASSERT_EQUAL( GPDMA_ADDR_STATIC, utJpeg_DmaXfer.DestinationAddrMode );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_MID, UtJpeg_GetGpdmaValue( UT_JPEG_GPDMA_BLOCK_SIZE, 0u ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_ADDR( input ), UtJpeg_GetGpdmaValue( UT_JPEG_GPDMA_SRC_ADDR, 0u ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_ADDR( &JPEG->DIR ), UtJpeg_GetGpdmaValue( UT_JPEG_GPDMA_DST_ADDR, 0u ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_ACTIVE, 0u ) );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( output, UT_JPEG_BUF_MID ) );
    TEST_ASSERT_EQUAL( GPDMA_DIR_PERIPH_TO_MEMORY, utJpeg_DmaXfer.Direction );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_DMA_REQUEST_OUTPUT, utJpeg_DmaXfer.RequestSource );
    TEST_ASSERT_EQUAL( GPDMA_ADDR_STATIC, utJpeg_DmaXfer.SourceAddrMode );
    TEST_ASSERT_EQUAL( GPDMA_ADDR_INCREMENT, utJpeg_DmaXfer.DestinationAddrMode );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_MID, UtJpeg_GetGpdmaValue( UT_JPEG_GPDMA_BLOCK_SIZE, 1u ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_ADDR( &JPEG->DOR ), UtJpeg_GetGpdmaValue( UT_JPEG_GPDMA_SRC_ADDR, 1u ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_ADDR( output ), UtJpeg_GetGpdmaValue( UT_JPEG_GPDMA_DST_ADDR, 1u ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_ACTIVE, 1u ) );
    TEST_ASSERT_EQUAL_HEX32( UT_JPEG_CR_DMA_MASK, JPEG->CR & UT_JPEG_CR_DMA_MASK );
    TEST_ASSERT_EQUAL_HEX32( JPEG_CR_EOCIE, JPEG->CR & UT_JPEG_CR_IRQ_MASK );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Buffers which the Gpdma can not move are rejected in the DMA mode.
 *
 * \par Expected results
 * - JPEG_REQUEST_ERROR and no channel is armed for a size that is not a multiple of 4 bytes, an address that is not
 *   aligned to 4 bytes and a size above 65532 bytes (the input; the output with the same exceptions, for a size that is
 *   not a multiple of 4 already in the common check); after the errors a valid buffer is accepted.
 */
void Ut_Jpeg_Set_InputBuffer_DmaInvalid_ReturnsError( void )
{
#if defined(JPEG)
    jpeg_Data_t * const input  = UtJpeg_Get_DmaInput();
    jpeg_Data_t * const output = UtJpeg_Get_DmaOutput();

    UtJpeg_StartDmaEncode();
    UtJpeg_ResetGpdmaLog();

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( input, UT_JPEG_BUF_UNALIGNED ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( &input[ 1u ], UT_JPEG_BUF_MID ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( input, UT_JPEG_BUF_TOO_BIG ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_OutputBuffer( &output[ 1u ], UT_JPEG_BUF_MID ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_OutputBuffer( output, UT_JPEG_BUF_TOO_BIG ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_ACTIVE, UT_JPEG_NA ) );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( input, UT_JPEG_BUF_DMA_MAX ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_DMA_MAX, UtJpeg_GetGpdmaValue( UT_JPEG_GPDMA_BLOCK_SIZE, 0u ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( output, UT_JPEG_BUF_DMA_MAX ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_DMA_STREAMS, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_ACTIVE, UT_JPEG_NA ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The transfer complete callbacks of the channels report the input and the output buffer.
 *
 * \par Expected results
 * - The input callback calls the input needed callback once and a new input buffer is accepted; the output callback
 *   calls the output ready callback with the size of the buffer, the total count of the bytes grows by the size;
 *   callbacks of a channel without a buffer are ignored.
 */
void Ut_Jpeg_Dma_Callbacks_ReportInputAndOutput( void )
{
#if defined(JPEG)
    jpeg_Data_t * const input  = UtJpeg_Get_DmaInput();
    jpeg_Data_t * const output = UtJpeg_Get_DmaOutput();
    jpeg_Size_t         length = 0u;

    UtJpeg_StartDmaEncode();

    utJpeg_DmaInitConfig[ 0u ].TransferCompleteIsr();
    utJpeg_DmaInitConfig[ 1u ].TransferCompleteIsr();
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_InputCnt + utJpeg_OutputCnt );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( input, UT_JPEG_BUF_MID ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( output, UT_JPEG_BUF_MID ) );

    utJpeg_DmaInitConfig[ 0u ].TransferCompleteIsr();
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_InputCnt );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( input, UT_JPEG_BUF_MID ) );

    utJpeg_DmaInitConfig[ 1u ].TransferCompleteIsr();
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_OutputCnt );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_MID, utJpeg_OutputLast );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_OutputLength( &length ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_MID, length );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( output, UT_JPEG_BUF_MID ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   The end of the conversion in the DMA mode stops the channels and reads the rest of the FIFO by the CPU.
 *
 * \par Expected results
 * - The DMA requests are disabled, both channels are stopped, the bytes the output channel moved follow from its
 * remaining block size (64 - 16 = 48 bytes), the FIFO is empty: the output ready callback reports 48 bytes and the
 *   end
 *   callback the total 48. With one more word in the FIFO the rest is stored behind the 48 bytes (a full buffer of
 *   64 bytes) and the job ends when the FIFO is empty.
 */
void Ut_Jpeg_Dma_EndOfConversion_StopsChannelsAndReadsRest( void )
{
#if defined(JPEG)
    jpeg_Data_t * const output = UtJpeg_Get_DmaOutput();
    jpeg_State_t        state  = JPEG_STATE_CNT;

    UtJpeg_StartDmaEncode();
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( output, UT_JPEG_BUF_MID ) );
    UtJpeg_ResetGpdmaLog();

    utJpeg_DmaRemaining = UT_JPEG_DMA_REMAINING;
    JPEG->SR            = JPEG_SR_EOCF;
    utJpeg_Isr();

    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CR & UT_JPEG_CR_DMA_MASK );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_INACTIVE, 0u ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_INACTIVE, 1u ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_OutputCnt );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_DMA_MOVED, utJpeg_OutputLast );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_EndCnt );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_DMA_MOVED, utJpeg_EndLast );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );

    UtJpeg_StartJob();
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( output, UT_JPEG_BUF_MID ) );

    utJpeg_DmaRemaining = UT_JPEG_DMA_REMAINING;
    JPEG->DOR           = UT_JPEG_OUT_WORD;
    JPEG->SR            = JPEG_SR_EOCF | JPEG_SR_OFNEF;
    utJpeg_Isr();

    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_TWICE, utJpeg_OutputCnt );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_MID, utJpeg_OutputLast );
    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_OUT_BYTE0, output[ UT_JPEG_DMA_MOVED ] );
    TEST_ASSERT_EQUAL_UINT8( UT_JPEG_OUT_BYTE3, output[ UT_JPEG_DMA_MOVED + UT_JPEG_WORD - 1u ] );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_EndCnt );

    JPEG->SR = JPEG_SR_EOCF;
    utJpeg_Isr();
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_TWICE, utJpeg_EndCnt );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_MID, utJpeg_EndLast );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   A Gpdma error ends the job with an error.
 *
 * \par Expected results
 * - The error callback of the channel stops both channels and the codec, the error callback is called with
 *   JPEG_ERROR_DMA and the state is idle; an error without a job is ignored.
 */
void Ut_Jpeg_Dma_Error_EndsJobWithDmaError( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    UtJpeg_StartDmaEncode();
    UtJpeg_ResetGpdmaLog();

    utJpeg_DmaInitConfig[ 1u ].ErrorIsr( GPDMA_ERROR_TRANSFER );

    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL( JPEG_ERROR_DMA, utJpeg_ErrorLast );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_INACTIVE, 0u ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_CH_INACTIVE, 1u ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, JPEG->CONFR0 & JPEG_CONFR0_START );

    utJpeg_DmaInitConfig[ 0u ].ErrorIsr( GPDMA_ERROR_TRANSFER );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_ErrorCnt );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   Pause and resume suspend and release the Gpdma channels of the streams.
 *
 * \par Expected results
 * - With an armed buffer the pause suspends the channel of the stream and the resume releases it; a stream paused
 *   before its buffer is given is suspended when the buffer arms the channel.
 */
void Ut_Jpeg_Dma_PauseAndResume_SuspendChannels( void )
{
#if defined(JPEG)
    jpeg_Data_t * const input  = UtJpeg_Get_DmaInput();
    jpeg_Data_t * const output = UtJpeg_Get_DmaOutput();

    UtJpeg_StartDmaEncode();
    UtJpeg_ResetGpdmaLog();

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( input, UT_JPEG_BUF_MID ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Pause( JPEG_STREAM_INPUT ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_PAUSE_ACTIVE, 0u ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Resume( JPEG_STREAM_INPUT ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_PAUSE_INACTIVE, 0u ) );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Pause( JPEG_STREAM_OUTPUT ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_PAUSE_ACTIVE, 1u ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( output, UT_JPEG_BUF_MID ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_PAUSE_ACTIVE, 1u ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Resume( JPEG_STREAM_OUTPUT ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtJpeg_CountGpdma( UT_JPEG_GPDMA_PAUSE_INACTIVE, 1u ) );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}


/**
 * \brief   With the data cache enabled the module maintains the cache around the transfers.
 *
 * \par Expected results
 * - The input buffer is cleaned, the output buffer is cleaned and invalidated before the transfer and invalidated when
 *   it is full and at the end of the conversion; a buffer which is not aligned to 16 bytes is rejected, with the data
 *   cache disabled the buffers need no alignment and no maintenance is done.
 */
void Ut_Jpeg_Dma_DataCacheEnabled_MaintainsCache( void )
{
#if defined(JPEG)
    jpeg_Data_t * const input  = UtJpeg_Get_DmaInput();
    jpeg_Data_t * const output = UtJpeg_Get_DmaOutput();

    UtJpeg_StartDmaEncode();

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( input, UT_JPEG_BUF_MID ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_CacheCnt );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Abort() );

    utJpeg_CacheEnabled = CACHE_FLAG_ACTIVE;
    UtJpeg_StartJob();

    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( input, UT_JPEG_BUF_UNALIGNED_CACHE ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_OutputBuffer( &output[ UT_JPEG_WORD ], UT_JPEG_BUF_MID ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, utJpeg_CacheCnt );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( input, UT_JPEG_BUF_MID ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, utJpeg_CacheCnt );
    TEST_ASSERT_EQUAL( UT_JPEG_CACHE_CLEAN, utJpeg_CacheLog[ 0u ].Operation );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_ADDR( input ), utJpeg_CacheLog[ 0u ].Address );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_BUF_MID, utJpeg_CacheLog[ 0u ].Size );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( output, UT_JPEG_BUF_MID ) );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_TWICE, utJpeg_CacheCnt );
    TEST_ASSERT_EQUAL( UT_JPEG_CACHE_FLUSH, utJpeg_CacheLog[ 1u ].Operation );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_ADDR( output ), utJpeg_CacheLog[ 1u ].Address );

    utJpeg_DmaInitConfig[ 1u ].TransferCompleteIsr();
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_THREE_TIMES, utJpeg_CacheCnt );
    TEST_ASSERT_EQUAL( UT_JPEG_CACHE_INVALIDATE, utJpeg_CacheLog[ UT_JPEG_THIRD_IDX ].Operation );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( output, UT_JPEG_BUF_MID ) );
    utJpeg_DmaRemaining = UT_JPEG_DMA_REMAINING;
    JPEG->SR            = JPEG_SR_EOCF;
    utJpeg_Isr();
    TEST_ASSERT_EQUAL( UT_JPEG_CACHE_INVALIDATE, utJpeg_CacheLog[ UT_JPEG_FOUR_TIMES ].Operation );
    TEST_ASSERT_EQUAL_UINT32( UT_JPEG_ADDR( output ), utJpeg_CacheLog[ UT_JPEG_FOUR_TIMES ].Address );
#else
    TEST_IGNORE_MESSAGE( UT_JPEG_NO_PERIPH_MSG );
#endif /* JPEG */
}

/* ============================ HELPER FUNCTIONS ============================ */

#if defined(JPEG)

/** Stub of Nvic_Set_PeriphIrq_Handler: stores the registered JPEG ISR and returns the configured result */
static nvic_RequestState_t UtJpeg_NvicSetHandlerStub( nvic_PeriphIrqList_t irqId,
                                                      const nvic_IsrCallback_t irqHandler,
                                                      int callCnt )
{
    (void)callCnt;

    TEST_ASSERT_EQUAL( NVIC_PERIPH_IRQ_JPEG, irqId );

    utJpeg_Isr = irqHandler;

    return ( utJpeg_NvicHandlerResult );
}


/** Header parsed callback of the tests: keeps a copy of the image parameters */
static void UtJpeg_HeaderParsedCallback( const jpeg_ImageInfo_t * const imageInfo )
{
    utJpeg_HeaderInfo = *imageInfo;
    utJpeg_HeaderCnt ++;
}


/** Input needed callback of the tests: counts the calls */
static void UtJpeg_InputNeededCallback( void )
{
    utJpeg_InputCnt ++;
}


/** Output ready callback of the tests: counts the calls and keeps the last length */
static void UtJpeg_OutputReadyCallback( jpeg_Size_t length )
{
    utJpeg_OutputLast = length;
    utJpeg_OutputCnt ++;
}


/** End of conversion callback of the tests: counts the calls and keeps the last total length */
static void UtJpeg_EndOfConversionCallback( jpeg_Size_t totalLength )
{
    utJpeg_EndLast = totalLength;
    utJpeg_EndCnt ++;
}


/** Error callback of the tests: counts the calls and keeps the last error mask */
static void UtJpeg_ErrorCallback( jpeg_Error_t errorMask )
{
    utJpeg_ErrorLast = errorMask;
    utJpeg_ErrorCnt ++;
}


/**
 * \brief Returns the default configuration of the module with the callbacks of the tests.
 *
 * \param xferMode [in]: Data transfer mode.
 *
 * \return Configuration for Jpeg_Init().
 */
static jpeg_Config_t UtJpeg_Get_Config( jpeg_XferMode_t xferMode )
{
    jpeg_Config_t config;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_DefaultConfig( &config ) );

    config.XferMode                = xferMode;
    config.IrqPriority             = UT_JPEG_IRQ_PRIO;
    config.HeaderParsedCallback    = UtJpeg_HeaderParsedCallback;
    config.InputNeededCallback     = UtJpeg_InputNeededCallback;
    config.OutputReadyCallback     = UtJpeg_OutputReadyCallback;
    config.EndOfConversionCallback = UtJpeg_EndOfConversionCallback;
    config.ErrorCallback           = UtJpeg_ErrorCallback;

    return ( config );
}


/**
 * \brief Sets the expectations of the calls of Jpeg_Init(): the clock, in the interrupt and DMA mode the interrupt.
 *
 * \param xferMode [in]: Data transfer mode.
 */
static void UtJpeg_Expect_Init( jpeg_XferMode_t xferMode )
{
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_JPEG, RCC_REQUEST_OK );

    if( JPEG_XFER_MODE_POLL != xferMode )
    {
        Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_JPEG, UT_JPEG_IRQ_PRIO, NVIC_REQUEST_OK );
        Nvic_Set_PeriphIrq_Active_ExpectAndReturn( NVIC_PERIPH_IRQ_JPEG, NVIC_REQUEST_OK );
    }
    else
    {
        /* The poll mode does not use the interrupt */
    }
}


/**
 * \brief Sets the expectations of the calls of the release of the peripheral: the interrupt (not in the poll mode),
 *        the reset of the peripheral and the clock.
 *
 * \param xferMode [in]: Data transfer mode.
 */
static void UtJpeg_Expect_Release( jpeg_XferMode_t xferMode )
{
    if( JPEG_XFER_MODE_POLL != xferMode )
    {
        Nvic_Set_PeriphIrq_Inactive_ExpectAndReturn( NVIC_PERIPH_IRQ_JPEG, NVIC_REQUEST_OK );
    }
    else
    {
        /* The poll mode does not use the interrupt */
    }

    Rcc_Set_ResetActive_ExpectAndReturn( RCC_PERIPH_JPEG, RCC_REQUEST_OK );
    Rcc_Set_ResetInactive_ExpectAndReturn( RCC_PERIPH_JPEG, RCC_REQUEST_OK );
    Rcc_Set_PeriphInactive_ExpectAndReturn( RCC_PERIPH_JPEG, RCC_REQUEST_OK );
}


/**
 * \brief Initializes the module with the default configuration of the tests and checks the result.
 *
 * \param xferMode [in]: Data transfer mode.
 */
static void UtJpeg_Init( jpeg_XferMode_t xferMode )
{
    jpeg_Config_t config = UtJpeg_Get_Config( xferMode );

    UtJpeg_Expect_Init( xferMode );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Init( &config ) );
}


/**
 * \brief Returns an image of the tests (16 x 16 pixels).
 *
 * \param colorSpace  [in]: Color space.
 * \param subsampling [in]: Chroma subsampling.
 * \param quality     [in]: Quality factor.
 *
 * \return The image.
 */
static jpeg_ImageConfig_t UtJpeg_Get_Image( jpeg_ColorSpace_t colorSpace,
                                            jpeg_Subsampling_t subsampling,
                                            jpeg_Quality_t quality )
{
    jpeg_ImageConfig_t image;

    image.Width       = UT_JPEG_WIDTH;
    image.Height      = UT_JPEG_HEIGHT;
    image.ColorSpace  = colorSpace;
    image.Subsampling = subsampling;
    image.Quality     = quality;

    return ( image );
}


/** Returns the count of the calls of all callbacks of the tests */
static uint32_t UtJpeg_CountEvents( void )
{
    return ( utJpeg_InputCnt + utJpeg_OutputCnt + utJpeg_EndCnt + utJpeg_ErrorCnt + utJpeg_HeaderCnt );
}


/** Starts an encoder job of a grayscale image on the initialized module and checks the result */
static void UtJpeg_StartJob( void )
{
    const jpeg_ImageConfig_t image = UtJpeg_Get_Image( JPEG_COLOR_GRAYSCALE,
                                                       JPEG_SUBSAMPLING_444,
                                                       UT_JPEG_QUALITY_MID );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
}


/**
 * \brief Initializes the module and starts an encoder job of a grayscale image.
 *
 * \param xferMode [in]: Data transfer mode.
 */
static void UtJpeg_StartEncode( jpeg_XferMode_t xferMode )
{
    UtJpeg_Init( xferMode );
    UtJpeg_StartJob();
}


/**
 * \brief Initializes the module and starts a decoder job.
 *
 * \param xferMode [in]: Data transfer mode.
 */
static void UtJpeg_StartDecode( jpeg_XferMode_t xferMode )
{
    UtJpeg_Init( xferMode );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
}


/** Initializes the module in the DMA mode and starts an encoder job of a grayscale image */
static void UtJpeg_StartDmaEncode( void )
{
    UtJpeg_StartEncode( JPEG_XFER_MODE_DMA );
}


/**
 * \brief Presets the registers of a parsed header (32 x 16 pixels) and the header flag.
 *
 * \param components [in]: JPEG_CONFR1.NF (count of the components - 1).
 * \param horizontal [in]: Horizontal sampling factor of the first component.
 * \param vertical   [in]: Vertical sampling factor of the first component.
 */
static void UtJpeg_Preset_Header( uint32_t components, uint32_t horizontal, uint32_t vertical )
{
    JPEG->CONFR1 = ( components << JPEG_CONFR1_NF_Pos ) | JPEG_CONFR1_DE | JPEG_CONFR1_HDR |
                   ( (uint32_t)UT_JPEG_HEIGHT << JPEG_CONFR1_YSIZE_Pos );
    JPEG->CONFR3 = ( (uint32_t)UT_JPEG_WIDTH_WIDE << JPEG_CONFR3_XSIZE_Pos );
    JPEG->CONFR4 = ( horizontal << JPEG_CONFR4_HSF_Pos ) | ( vertical << JPEG_CONFR4_VSF_Pos );
    JPEG->SR     = JPEG_SR_HPDF;
}


/**
 * \brief Writes a marker segment: the prefix, the marker, the length (it includes the length field) and zero data.
 *
 * \param data   [out]: Buffer of the stream.
 * \param pos    [in]:  Position of the segment.
 * \param marker [in]:  Second byte of the marker.
 * \param length [in]:  Length of the segment without the marker.
 *
 * \return Position behind the segment.
 */
static uint32_t UtJpeg_Put_Segment( jpeg_Data_t * const data, uint32_t pos, uint32_t marker, uint32_t length )
{
    data[ pos ]      = UT_JPEG_MARKER_PREFIX;
    data[ pos + 1u ] = (jpeg_Data_t)marker;
    data[ pos + UT_JPEG_MARKER_SIZE ]      = (jpeg_Data_t)( ( length >> UT_JPEG_BYTE_SHIFT ) & UT_JPEG_BYTE_MASK );
    data[ pos + UT_JPEG_MARKER_SIZE + 1u ] = (jpeg_Data_t)( length & UT_JPEG_BYTE_MASK );

    return ( pos + UT_JPEG_MARKER_SIZE + length );
}


/**
 * \brief Builds the header of a JPEG stream in the buffer: SOI, APP0, DQT, TEM, RST0 and a start of frame segment.
 *
 * \param data      [out]: Buffer of at least \ref UT_JPEG_HEADER_SIZE bytes.
 * \param sofMarker [in]:  Second byte of the marker in the place of the start of frame.
 * \param precision [in]:  Precision (bits per sample) of the start of frame segment.
 *
 * \return Count of the bytes of the header.
 */
static uint32_t UtJpeg_Build_Header( jpeg_Data_t * const data, uint32_t sofMarker, uint32_t precision )
{
    uint32_t pos = UT_JPEG_MARKER_SIZE;

    UtJpeg_FillBuffer( data, UT_JPEG_HEADER_SIZE, 0u );

    data[ 0u ] = UT_JPEG_MARKER_PREFIX;
    data[ 1u ] = UT_JPEG_MARKER_SOI;

    pos = UtJpeg_Put_Segment( data, pos, UT_JPEG_MARKER_APP0, UT_JPEG_APP0_LENGTH );
    pos = UtJpeg_Put_Segment( data, pos, UT_JPEG_MARKER_DQT, UT_JPEG_DQT_LENGTH );

    data[ pos ]      = UT_JPEG_MARKER_PREFIX;
    data[ pos + 1u ] = UT_JPEG_MARKER_TEM;
    pos += UT_JPEG_MARKER_SIZE;

    data[ pos ]      = UT_JPEG_MARKER_PREFIX;
    data[ pos + 1u ] = UT_JPEG_MARKER_RST0;
    pos += UT_JPEG_MARKER_SIZE;

    data[ pos + UT_JPEG_SOF_PRECISION_OFFSET ] = (jpeg_Data_t)precision;

    return ( UtJpeg_Put_Segment( data, pos, sofMarker, UT_JPEG_SOF_LENGTH ) );
}


/**
 * \brief Fills a buffer with a pattern.
 *
 * \param data    [out]: Buffer.
 * \param size    [in]:  Count of the bytes.
 * \param pattern [in]:  Value of the bytes.
 */
static void UtJpeg_FillBuffer( jpeg_Data_t * const data, jpeg_Size_t size, jpeg_Data_t pattern )
{
    for( jpeg_Size_t idx = 0u; size > idx; idx++ )
    {
        data[ idx ] = pattern;
    }
}


/**
 * \brief Returns a word of a buffer (little endian, as the module writes it to the input FIFO).
 *
 * \param data    [in]: Buffer.
 * \param wordIdx [in]: Index of the word.
 *
 * \return The word.
 */
static uint32_t UtJpeg_Get_Word( const jpeg_Data_t * const data, uint32_t wordIdx )
{
    uint32_t wordValue = 0u;

    for( uint32_t byteIdx = 0u; UT_JPEG_WORD > byteIdx; byteIdx++ )
    {
        const uint32_t dataByte = data[ ( wordIdx * UT_JPEG_WORD ) + byteIdx ];

        wordValue |= ( dataByte << ( UT_JPEG_BYTE_SHIFT * byteIdx ) );
    }

    return ( wordValue );
}


/**
 * \brief Compares the words of a register memory with the expected words.
 *
 * \param actual   [in]: Register memory.
 * \param expected [in]: Expected words.
 * \param wordCnt  [in]: Count of the words.
 */
static void UtJpeg_AssertWords( const volatile uint32_t * const actual,
                                const uint32_t * const expected,
                                uint32_t wordCnt )
{
    for( uint32_t wordIdx = 0u; wordCnt > wordIdx; wordIdx++ )
    {
        TEST_ASSERT_EQUAL_HEX32_MESSAGE( expected[ wordIdx ], actual[ wordIdx ], "word of the register memory" );
    }
}


/**
 * \brief Compares a quantization memory with the expected words.
 *
 * \param tableId  [in]: Quantization table.
 * \param expected [in]: The 16 expected words.
 */
static void UtJpeg_AssertQuantMemory( jpeg_QuantId_t tableId, const uint32_t * const expected )
{
    const volatile uint32_t * const memories[ JPEG_QUANT_TABLE_CNT ] =
    {
        [JPEG_QUANT_TABLE_0] = JPEG->QMEM0,
        [JPEG_QUANT_TABLE_1] = JPEG->QMEM1,
        [JPEG_QUANT_TABLE_2] = JPEG->QMEM2,
        [JPEG_QUANT_TABLE_3] = JPEG->QMEM3,
    };

    UtJpeg_AssertWords( memories[ tableId ], expected, JPEG_REG_QUANT_WORD_CNT );
}


/** Returns the input buffer of the DMA tests in the emulated SRAM */
static jpeg_Data_t * UtJpeg_Get_DmaInput( void )
{
    return ( REGMEM_SRAM_PTR( jpeg_Data_t, UT_JPEG_SRAM_INPUT ) );
}


/** Returns the output buffer of the DMA tests in the emulated SRAM */
static jpeg_Data_t * UtJpeg_Get_DmaOutput( void )
{
    return ( REGMEM_SRAM_PTR( jpeg_Data_t, UT_JPEG_SRAM_OUTPUT ) );
}


/**
 * \brief Appends a record to the log of the Gpdma calls.
 *
 * \param callId    [in]: Gpdma function.
 * \param periphId  [in]: Controller of the call.
 * \param channelId [in]: Channel of the call.
 * \param value     [in]: Value of the call the tests check.
 *
 * \return Always \ref GPDMA_REQUEST_OK.
 */
static gpdma_RequestState_t UtJpeg_GpdmaRecord( utJpeg_GpdmaCall_t callId,
                                                uint32_t periphId,
                                                uint32_t channelId,
                                                uint32_t value )
{
    if( UT_JPEG_DMA_LOG_SIZE > utJpeg_GpdmaCnt )
    {
        utJpeg_GpdmaLog[ utJpeg_GpdmaCnt ].Id      = callId;
        utJpeg_GpdmaLog[ utJpeg_GpdmaCnt ].Periph  = periphId;
        utJpeg_GpdmaLog[ utJpeg_GpdmaCnt ].Channel = channelId;
        utJpeg_GpdmaLog[ utJpeg_GpdmaCnt ].Value   = value;
    }
    else
    {
        /* The log is full */
    }

    utJpeg_GpdmaCnt ++;

    return ( GPDMA_REQUEST_OK );
}


/** Clears the log of the Gpdma calls (the copies of the Init configurations stay) */
static void UtJpeg_ResetGpdmaLog( void )
{
    utJpeg_GpdmaCnt = 0u;
}


/**
 * \brief Counts the records of the log of the Gpdma calls.
 *
 * \param callId    [in]: Gpdma function.
 * \param channelId [in]: Channel of the call, \ref UT_JPEG_NA for any channel.
 *
 * \return Count of the records.
 */
static uint32_t UtJpeg_CountGpdma( utJpeg_GpdmaCall_t callId, uint32_t channelId )
{
    uint32_t retCnt = 0u;

    for( uint32_t logIdx = 0u;
         utJpeg_GpdmaCnt > logIdx;
         logIdx++ )
    {
        const uint32_t logChannel = utJpeg_GpdmaLog[ logIdx % UT_JPEG_DMA_LOG_SIZE ].Channel;

        if( callId == utJpeg_GpdmaLog[ logIdx % UT_JPEG_DMA_LOG_SIZE ].Id )
        {
            if( UT_JPEG_NA == channelId )
            {
                retCnt ++;
            }
            else if( channelId == logChannel )
            {
                retCnt ++;
            }
            else
            {
                /* Another channel */
            }
        }
        else
        {
            /* Another call */
        }
    }

    return ( retCnt );
}


/**
 * \brief Returns the value of the last record of a Gpdma function and channel.
 *
 * \param callId    [in]: Gpdma function.
 * \param channelId [in]: Channel of the call.
 *
 * \return The value, \ref UT_JPEG_NA if there is no such record.
 */
static uint32_t UtJpeg_GetGpdmaValue( utJpeg_GpdmaCall_t callId, uint32_t channelId )
{
    uint32_t retValue = UT_JPEG_NA;

    for( uint32_t logIdx = 0u;
         utJpeg_GpdmaCnt > logIdx;
         logIdx++ )
    {
        if( ( callId    == utJpeg_GpdmaLog[ logIdx % UT_JPEG_DMA_LOG_SIZE ].Id      ) &&
            ( channelId == utJpeg_GpdmaLog[ logIdx % UT_JPEG_DMA_LOG_SIZE ].Channel )    )
        {
            retValue = utJpeg_GpdmaLog[ logIdx % UT_JPEG_DMA_LOG_SIZE ].Value;
        }
        else
        {
            /* Another call */
        }
    }

    return ( retValue );
}


/** Stub of Gpdma_Init: keeps a copy of the configuration (and of its transfer) of the first two calls */
static gpdma_RequestState_t UtJpeg_GpdmaInitStub( gpdma_ConfigStruct_t * const configStruct, int callCnt )
{
    const uint32_t slot = UtJpeg_CountGpdma( UT_JPEG_GPDMA_INIT, UT_JPEG_NA );

    (void)callCnt;

    if( UT_JPEG_DMA_STREAMS > slot )
    {
        utJpeg_DmaInitConfig[ slot ]                = *configStruct;
        utJpeg_DmaInitXfer[ slot ]                  = *configStruct->TransferConfig;
        utJpeg_DmaInitConfig[ slot ].TransferConfig = &utJpeg_DmaInitXfer[ slot ];
    }
    else
    {
        /* Only the two channels of the module are kept */
    }

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_INIT, configStruct->PeriphId, configStruct->ChannelId, 0u ) );
}


/** Stub of Gpdma_Deinit */
static gpdma_RequestState_t UtJpeg_GpdmaDeinitStub( gpdma_PeriphId_t periphId,
                                                    gpdma_ChannelId_t channelId,
                                                    int callCnt )
{
    (void)callCnt;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_DEINIT, periphId, channelId, 0u ) );
}


/** Stub of Gpdma_Set_InterruptActive */
static gpdma_RequestState_t UtJpeg_GpdmaIrqActiveStub( gpdma_PeriphId_t periphId,
                                                       gpdma_ChannelId_t channelId,
                                                       int callCnt )
{
    (void)callCnt;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_IRQ_ACTIVE, periphId, channelId, 0u ) );
}


/** Stub of Gpdma_Get_XferListConfig: keeps a copy of the transfer configuration of the last call */
static gpdma_RequestState_t UtJpeg_GpdmaListConfigStub( gpdma_TransferConfig_t * const transferConfig,
                                                        gpdma_TransfersCount_t transferCount,
                                                        gpdma_ChannelType_t channelType,
                                                        volatile gpdma_XferList_t * transferList,
                                                        int callCnt )
{
    (void)transferCount;
    (void)transferList;
    (void)callCnt;

    utJpeg_DmaXfer = *transferConfig;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_LIST_CONFIG, UT_JPEG_NA, UT_JPEG_NA, (uint32_t)channelType ) );
}


/** Stub of Gpdma_Get_XferList_LinkReg: returns the marker as the link register */
static gpdma_RequestState_t UtJpeg_GpdmaLinkRegStub( volatile gpdma_XferList_t * const transferList,
                                                     gpdma_ChannelType_t channelType,
                                                     gpdma_XferLinkReg_t * const linkReg,
                                                     int callCnt )
{
    (void)transferList;
    (void)callCnt;

    *linkReg = UT_JPEG_LINKREG_MARK;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_LINK_REG, UT_JPEG_NA, UT_JPEG_NA, (uint32_t)channelType ) );
}


/** Stub of Gpdma_Set_XferListLink */
static gpdma_RequestState_t UtJpeg_GpdmaListLinkStub( gpdma_PeriphId_t periphId,
                                                      gpdma_ChannelId_t channelId,
                                                      gpdma_XferLinkReg_t linkReg,
                                                      int callCnt )
{
    (void)callCnt;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_LIST_LINK, periphId, channelId, linkReg ) );
}


/** Stub of Gpdma_Set_BlockSize */
static gpdma_RequestState_t UtJpeg_GpdmaBlockSizeStub( gpdma_PeriphId_t periphId,
                                                       gpdma_ChannelId_t channelId,
                                                       gpdma_BlockSize_t blockSize,
                                                       int callCnt )
{
    (void)callCnt;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_BLOCK_SIZE, periphId, channelId, blockSize ) );
}


/** Stub of Gpdma_Set_SourceAddr */
static gpdma_RequestState_t UtJpeg_GpdmaSrcAddrStub( gpdma_PeriphId_t periphId,
                                                     gpdma_ChannelId_t channelId,
                                                     gpdma_SrcAddr_t sourceAddr,
                                                     int callCnt )
{
    (void)callCnt;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_SRC_ADDR, periphId, channelId, sourceAddr ) );
}


/** Stub of Gpdma_Set_DestinationAddr */
static gpdma_RequestState_t UtJpeg_GpdmaDstAddrStub( gpdma_PeriphId_t periphId,
                                                     gpdma_ChannelId_t channelId,
                                                     gpdma_DstAddr_t destAddr,
                                                     int callCnt )
{
    (void)callCnt;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_DST_ADDR, periphId, channelId, destAddr ) );
}


/** Stub of Gpdma_Set_ChannelActive */
static gpdma_RequestState_t UtJpeg_GpdmaChActiveStub( gpdma_PeriphId_t periphId,
                                                      gpdma_ChannelId_t channelId,
                                                      int callCnt )
{
    (void)callCnt;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_CH_ACTIVE, periphId, channelId, 0u ) );
}


/** Stub of Gpdma_Set_ChannelInactive */
static gpdma_RequestState_t UtJpeg_GpdmaChInactiveStub( gpdma_PeriphId_t periphId,
                                                        gpdma_ChannelId_t channelId,
                                                        int callCnt )
{
    (void)callCnt;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_CH_INACTIVE, periphId, channelId, 0u ) );
}


/** Stub of Gpdma_Set_PauseActive */
static gpdma_RequestState_t UtJpeg_GpdmaPauseActiveStub( gpdma_PeriphId_t periphId,
                                                         gpdma_ChannelId_t channelId,
                                                         int callCnt )
{
    (void)callCnt;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_PAUSE_ACTIVE, periphId, channelId, 0u ) );
}


/** Stub of Gpdma_Set_PauseInactive */
static gpdma_RequestState_t UtJpeg_GpdmaPauseInactiveStub( gpdma_PeriphId_t periphId,
                                                           gpdma_ChannelId_t channelId,
                                                           int callCnt )
{
    (void)callCnt;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_PAUSE_INACTIVE, periphId, channelId, 0u ) );
}


/** Stub of Gpdma_Get_BlockSize: returns the remaining block size preset by the test */
static gpdma_RequestState_t UtJpeg_GpdmaGetBlockStub( gpdma_PeriphId_t periphId,
                                                      gpdma_ChannelId_t channelId,
                                                      gpdma_BlockSize_t * const blockSize,
                                                      int callCnt )
{
    (void)callCnt;

    *blockSize = utJpeg_DmaRemaining;

    return ( UtJpeg_GpdmaRecord( UT_JPEG_GPDMA_GET_BLOCK, periphId, channelId, utJpeg_DmaRemaining ) );
}


/** Installs the stubs of the Gpdma functions the module calls */
static void UtJpeg_GpdmaStubs( void )
{
    Gpdma_Init_Stub( UtJpeg_GpdmaInitStub );
    Gpdma_Deinit_Stub( UtJpeg_GpdmaDeinitStub );
    Gpdma_Set_InterruptActive_Stub( UtJpeg_GpdmaIrqActiveStub );
    Gpdma_Get_XferListConfig_Stub( UtJpeg_GpdmaListConfigStub );
    Gpdma_Get_XferList_LinkReg_Stub( UtJpeg_GpdmaLinkRegStub );
    Gpdma_Set_XferListLink_Stub( UtJpeg_GpdmaListLinkStub );
    Gpdma_Set_BlockSize_Stub( UtJpeg_GpdmaBlockSizeStub );
    Gpdma_Set_SourceAddr_Stub( UtJpeg_GpdmaSrcAddrStub );
    Gpdma_Set_DestinationAddr_Stub( UtJpeg_GpdmaDstAddrStub );
    Gpdma_Set_ChannelActive_Stub( UtJpeg_GpdmaChActiveStub );
    Gpdma_Set_ChannelInactive_Stub( UtJpeg_GpdmaChInactiveStub );
    Gpdma_Set_PauseActive_Stub( UtJpeg_GpdmaPauseActiveStub );
    Gpdma_Set_PauseInactive_Stub( UtJpeg_GpdmaPauseInactiveStub );
    Gpdma_Get_BlockSize_Stub( UtJpeg_GpdmaGetBlockStub );
}


/** Stub of Cache_Get_State: the data cache is enabled or disabled by the test */
static cache_RequestState_t UtJpeg_CacheGetStateStub( cache_CacheId_t cacheId,
                                                      cache_State_t * const cacheState,
                                                      int callCnt )
{
    (void)callCnt;

    TEST_ASSERT_EQUAL( CACHE_DATA, cacheId );

    cacheState->Requested  = utJpeg_CacheEnabled;
    cacheState->Enabled    = utJpeg_CacheEnabled;
    cacheState->SuspendCnt = 0u;

    return ( CACHE_REQUEST_OK );
}


/**
 * \brief Appends a record to the log of the Cache calls.
 *
 * \param operation [in]: Cache function.
 * \param address   [in]: Address of the call.
 * \param size      [in]: Size of the call.
 *
 * \return Always \ref CACHE_REQUEST_OK.
 */
static cache_RequestState_t UtJpeg_CacheRecord( utJpeg_CacheOp_t operation, uint32_t address, uint32_t size )
{
    if( UT_JPEG_CACHE_LOG_SIZE > utJpeg_CacheCnt )
    {
        utJpeg_CacheLog[ utJpeg_CacheCnt ].Operation = operation;
        utJpeg_CacheLog[ utJpeg_CacheCnt ].Address   = address;
        utJpeg_CacheLog[ utJpeg_CacheCnt ].Size      = size;
    }
    else
    {
        /* The log is full */
    }

    utJpeg_CacheCnt ++;

    return ( CACHE_REQUEST_OK );
}


/** Stub of Cache_Set_CleanRange */
static cache_RequestState_t UtJpeg_CacheCleanStub( cache_CacheId_t cacheId,
                                                   uint32_t address,
                                                   uint32_t size,
                                                   int callCnt )
{
    (void)cacheId;
    (void)callCnt;

    return ( UtJpeg_CacheRecord( UT_JPEG_CACHE_CLEAN, address, size ) );
}


/** Stub of Cache_Set_CleanInvalidateRange */
static cache_RequestState_t UtJpeg_CacheFlushStub( cache_CacheId_t cacheId,
                                                   uint32_t address,
                                                   uint32_t size,
                                                   int callCnt )
{
    (void)cacheId;
    (void)callCnt;

    return ( UtJpeg_CacheRecord( UT_JPEG_CACHE_FLUSH, address, size ) );
}


/** Stub of Cache_Set_InvalidateRange */
static cache_RequestState_t UtJpeg_CacheInvalidateStub( cache_CacheId_t cacheId,
                                                        uint32_t address,
                                                        uint32_t size,
                                                        int callCnt )
{
    (void)cacheId;
    (void)callCnt;

    return ( UtJpeg_CacheRecord( UT_JPEG_CACHE_INVALIDATE, address, size ) );
}


/** Installs the stubs of the Cache functions the module calls */
static void UtJpeg_CacheStubs( void )
{
    Cache_Get_State_Stub( UtJpeg_CacheGetStateStub );
    Cache_Set_CleanRange_Stub( UtJpeg_CacheCleanStub );
    Cache_Set_CleanInvalidateRange_Stub( UtJpeg_CacheFlushStub );
    Cache_Set_InvalidateRange_Stub( UtJpeg_CacheInvalidateStub );
}

#endif /* JPEG */
