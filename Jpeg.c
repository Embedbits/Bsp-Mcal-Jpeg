/**
 * \author Mr.Nobody
 * \file Jpeg.c
 * \ingroup Jpeg
 * \brief Jpeg module common functionality
 *
 * # Introduction #
 * The JPEG codec (JPEG) encodes the raw image data (the MCU blocks of 8 bit components) into a JPEG stream and
 * decodes a JPEG stream into the raw MCU blocks, RM0517 chapter "JPEG codec". The module owns the codec: it
 * programs the image parameters, the quantization tables and the Huffman tables, moves the two streams (input and
 * output FIFO of the codec) and reports the events by callbacks.
 *
 * # Device differences (STM32H5) #
 * The JPEG codec exists on the STM32H5E and STM32H5F lines (CMSIS device headers). On the other lines every public
 * function checks the CMSIS instance macro and returns \ref JPEG_REQUEST_ERROR.
 *
 * # Encoder #
 * \ref Jpeg_Set_EncodeStart programs the image (the size, the color space, the subsampling), the quantization
 * tables scaled by the quality factor (the factor of the common libraries: 5000 / quality below 50, 200 - 2 *
 * quality from 50), the Huffman tables (the encoder memory with the code / length pairs and the DHT memory the
 * header generator copies into the DHT marker) and starts the codec. The raw data are the MCUs in the line order of
 * the MCUs. An MCU is 8 x 8 pixels (grayscale, RGB and CMYK, YCbCr 4:4:4), 16 x 8 pixels (YCbCr 4:2:2) or 16 x 16
 * pixels (YCbCr 4:2:0), every component of the MCU is a group of 8 x 8 blocks of 64 bytes. The encoder ends by
 * itself when it consumed the count of the MCUs of the image.
 *
 * # Streams #
 * The input stream is given by \ref Jpeg_Set_InputBuffer, the output stream by \ref Jpeg_Set_OutputBuffer. The
 * hardware works on 32-bit words (little endian), so the sizes of the output buffers are multiples of 4 bytes and
 * only the last input buffer of a stream may have another size (the last word is padded with zeros). When an input
 * buffer is consumed the \ref jpeg_InputNeededCallback_t is called, when an output buffer is full the
 * \ref jpeg_OutputReadyCallback_t is called; the codec waits (its FIFOs stall) until the next buffer is given.
 * The job ends when the codec reports the end of the conversion and its output FIFO is empty: the last output
 * buffer is reported (if it holds data) and then the \ref jpeg_EndOfConversionCallback_t is called.
 *
 * # Transfer modes #
 * - \ref JPEG_XFER_MODE_POLL - the FIFOs are served by \ref Jpeg_Task,
 * - \ref JPEG_XFER_MODE_ISR - by the JPEG interrupt (the not full interrupt of the input FIFO and the not empty
 *   interrupt of the output FIFO are enabled only while a buffer is waiting),
 * - \ref JPEG_XFER_MODE_DMA - by two Gpdma channels (one 32-bit word per request): the buffers are multiples of 4
 *   bytes, aligned to 4 bytes and at most 65532 bytes, the input data cache is cleaned and the output data cache is
 *   invalidated (Cache module, buffers aligned to 16 bytes when the data cache is enabled). At the end of the
 *   conversion the channels are stopped, the count of the bytes the output channel moved is calculated from its
 *   remaining block size and the rest of the output FIFO is moved by the CPU. The Gpdma interrupts shall have a
 *   higher priority than the JPEG interrupt.
 *
 * # Quality and tables #
 * The quantization tables and the Huffman tables are kept by the module (the sample tables of the specification are
 * loaded by \ref Jpeg_Init unless \ref jpeg_Config_t::StandardTables is inactive) and written to the codec at the
 * start of every job. A custom table is given in the row order (quantization) or as the DHT segment (Huffman). The
 * quality scales every quantization table of the job, the value 50 leaves a table as it was given.
 *
 */
/* ============================== INCLUDES ================================== */
#include "Jpeg.h"                           /* Self include                   */
#include "Jpeg_Port.h"                      /* Own port file include          */
#include "Jpeg_Types.h"                     /* Module types definitions       */
#include "Jpeg_Reg.h"                       /* Register access                */
#include "Jpeg_Tables.h"                    /* Standard tables                */
#include "Nvic_Port.h"                      /* NVIC handler functionality     */
#include "Rcc_Port.h"                       /* RCC handler functionality      */
#include "Gpdma_Port.h"                     /* DMA handler functionality      */
#include "Cache_Port.h"                     /* Cache handler functionality    */
#include "Stm32.h"                          /* JPEG registers definition      */
/* ============================== TYPEDEFS ================================== */

#if defined(JPEG)

/** \brief Gpdma channels of the DMA mode */
typedef enum
{
    JPEG_DMA_STREAM_INPUT = 0u, /**< Channel of the input stream (memory to JPEG_DIR)  */
    JPEG_DMA_STREAM_OUTPUT,     /**< Channel of the output stream (JPEG_DOR to memory) */
    JPEG_DMA_STREAM_CNT         /**< Count of the channels                             */
}   jpeg_DmaStream_t;

/** \brief Result of the scan of the first bytes of a JPEG stream for the features the hardware does not support */
typedef enum
{
    JPEG_SCAN_INCOMPLETE = 0u, /**< The buffer ends before the start of frame segment, nothing can be said     */
    JPEG_SCAN_SUPPORTED,       /**< The start of frame segment is a baseline frame of 8-bit samples            */
    JPEG_SCAN_UNSUPPORTED,     /**< The frame is progressive, lossless, hierarchical, arithmetic or not 8 bit  */
    JPEG_SCAN_CORRUPT,         /**< The buffer is not the start of a JPEG stream                               */
    JPEG_SCAN_CNT              /**< Count of the results                                                       */
}   jpeg_ScanResult_t;

/** \brief Operation on the data cache around a transfer of the Gpdma */
typedef enum
{
    JPEG_CACHE_CLEAN = 0u, /**< Write back the input data of the codec to the memory                   */
    JPEG_CACHE_FLUSH,      /**< Write back and invalidate the buffer the codec is going to write       */
    JPEG_CACHE_INVALIDATE, /**< Drop the lines of the buffer the codec wrote, the CPU reads the memory */
    JPEG_CACHE_CNT         /**< Count of the operations                                                */
}   jpeg_CacheOp_t;

/** \brief Runtime data of the module */
typedef struct
{
    jpeg_FunctionState_t           InitState;               /**< Jpeg_Init succeeded                                */
    jpeg_XferMode_t                XferMode;                /**< Configured data transfer mode                      */
    jpeg_FunctionState_t           CpuServed;               /**< The FIFOs are served by the CPU (poll, interrupt)  */
    gpdma_PeriphId_t               DmaPeriphId;             /**< Gpdma controller (DMA mode only)                   */
    gpdma_ChannelId_t              DmaInputChannelId;       /**< Gpdma channel of the input stream                  */
    gpdma_ChannelId_t              DmaOutputChannelId;      /**< Gpdma channel of the output stream                 */
    gpdma_Priority_t               DmaPriority;             /**< Priority of the Gpdma channels                     */
    jpeg_HeaderMode_t              HeaderMode;              /**< Header generation of the encoder                   */
    jpeg_Quality_t                 Quality;                 /**< Quality factor of the module                       */
    jpeg_HeaderParsedCallback_t  * HeaderParsedCallback;    /**< Header parsed callback                             */
    jpeg_InputNeededCallback_t   * InputNeededCallback;     /**< Input needed callback                              */
    jpeg_OutputReadyCallback_t   * OutputReadyCallback;     /**< Output ready callback                              */
    jpeg_EndOfConversionCallback_t * EndOfConversionCallback; /**< End of conversion callback                       */
    jpeg_ErrorCallback_t         * ErrorCallback;           /**< Error callback                                     */
    jpeg_State_t                   State;                   /**< State of the codec                                 */
    jpeg_FunctionState_t           HeaderParsed;            /**< The header of the decoder job is parsed            */
    jpeg_FunctionState_t           HeaderScanned;           /**< The first input buffer of the decoder was scanned  */
    jpeg_FunctionState_t           EocWait;                 /**< End of conversion seen, output FIFO not empty yet  */
    jpeg_ImageInfo_t               ImageInfo;               /**< Parameters of the image of the job                 */
    jpeg_FunctionState_t           ImageInfoValid;          /**< ImageInfo holds the parameters of a job            */
    jpeg_QuantTable_t              QuantTable[ JPEG_QUANT_TABLE_CNT ]; /**< Base quantization tables                */
    jpeg_FunctionState_t           QuantTableSet[ JPEG_QUANT_TABLE_CNT ]; /**< The base table is available          */
    jpeg_HuffmanTable_t            HuffmanTable[ JPEG_HUFFMAN_CNT ];  /**< Huffman tables                           */
    jpeg_FunctionState_t           HuffmanTableSet[ JPEG_HUFFMAN_CNT ]; /**< The Huffman table is available         */
    const jpeg_Data_t            * InData;                  /**< Input buffer                                       */
    jpeg_Size_t                    InSize;                  /**< Size of the input buffer                           */
    jpeg_Size_t                    InIdx;                   /**< Count of the bytes of the input buffer moved       */
    jpeg_FunctionState_t           InPending;               /**< An input buffer is being served                    */
    jpeg_FunctionState_t           InPaused;                /**< The input stream is paused                         */
    jpeg_Data_t                  * OutData;                 /**< Output buffer                                      */
    jpeg_Size_t                    OutSize;                 /**< Size of the output buffer                          */
    jpeg_Size_t                    OutIdx;                  /**< Count of the bytes in the output buffer            */
    jpeg_FunctionState_t           OutPending;              /**< An output buffer is being served                   */
    jpeg_FunctionState_t           OutPaused;               /**< The output stream is paused                        */
    jpeg_Size_t                    TotalOut;                /**< Count of the bytes the job produced                */
    jpeg_FunctionState_t           DmaChannelInit[ JPEG_DMA_STREAM_CNT ]; /**< Gpdma channel initialized      */
}   jpeg_RuntimeData_t;

#endif /* JPEG */

/* ======================== FORWARD DECLARATIONS ============================ */

#if defined(JPEG)

static jpeg_RequestState_t Jpeg_Check_Config       ( const jpeg_Config_t * const jpegConfig );
static jpeg_RequestState_t Jpeg_Check_ImageConfig  ( const jpeg_ImageConfig_t * const imageConfig );
static jpeg_RequestState_t Jpeg_Check_QuantTable   ( const jpeg_QuantTable_t * const table );
static jpeg_RequestState_t Jpeg_Check_HuffmanTable ( jpeg_HuffmanId_t tableId,
                                                     const jpeg_HuffmanTable_t * const table );
static void                Jpeg_Get_Layout         ( jpeg_ColorSpace_t colorSpace,
                                                     jpeg_Subsampling_t subsampling,
                                                     jpeg_McuLayout_t * const layout );
static jpeg_RequestState_t Jpeg_Calc_ImageInfo     ( const jpeg_ImageConfig_t * const imageConfig,
                                                     jpeg_ImageInfo_t * const imageInfo );
static jpeg_Count_t        Jpeg_Calc_Scale         ( jpeg_Quality_t quality );
static void                Jpeg_Build_QuantWords   ( const jpeg_QuantTable_t * const table,
                                                     jpeg_Count_t scale,
                                                     uint32_t * const wordList );
static jpeg_RequestState_t Jpeg_Build_HuffEnc      ( jpeg_HuffmanId_t tableId,
                                                     const jpeg_HuffmanTable_t * const table,
                                                     uint32_t * const wordList,
                                                     uint32_t * const wordCnt );
static void                Jpeg_Build_DhtWords     ( uint32_t * const wordList );
static void                Jpeg_Build_RegConfig    ( const jpeg_ImageConfig_t * const imageConfig,
                                                     const jpeg_ImageInfo_t * const imageInfo,
                                                     jpeg_RegImageConfig_t * const regConfig );
static jpeg_FunctionState_t Jpeg_Is_TableReady     ( jpeg_ColorSpace_t colorSpace );
static jpeg_RequestState_t Jpeg_Load_Tables        ( jpeg_ColorSpace_t colorSpace, jpeg_Quality_t quality );
static jpeg_RequestState_t Jpeg_Start_Job          ( const jpeg_ImageConfig_t * const imageConfig );
static void                Jpeg_Reset_Stream       ( void );
static void                Jpeg_Update_Irq         ( void );
static jpeg_RequestState_t Jpeg_Init_Hardware     ( jpeg_IrqPrio_t irqPrio );
static void                Jpeg_Load_StandardTables ( void );
static jpeg_RequestState_t Jpeg_Take_Input         ( const jpeg_Data_t * const buffer, jpeg_Size_t size );
static jpeg_RequestState_t Jpeg_Take_Output        ( jpeg_Data_t * const buffer, jpeg_Size_t size );
static uint32_t            Jpeg_Get_InputWord      ( void );
static void                Jpeg_Put_OutputWord     ( uint32_t dataWord );
static void                Jpeg_Service            ( void );
static void                Jpeg_Service_Dma        ( void );
static void                Jpeg_Feed               ( void );
static void                Jpeg_Drain              ( void );
static void                Jpeg_Check_End          ( void );
static void                Jpeg_Complete_Job       ( void );
static void                Jpeg_Fail_Job           ( jpeg_Error_t errorMask );
static jpeg_RequestState_t Jpeg_Abort_Job          ( void );
static void                Jpeg_Enter_Idle         ( void );
static jpeg_RequestState_t Jpeg_Start_Decode       ( void );
static jpeg_FunctionState_t Jpeg_Is_SofMarker      ( jpeg_Size_t marker );
static jpeg_FunctionState_t Jpeg_Is_StandaloneMarker ( jpeg_Size_t marker );
static jpeg_ScanResult_t   Jpeg_Check_Sof          ( const jpeg_Data_t * const data,
                                                     jpeg_Size_t size,
                                                     jpeg_Size_t pos );
static jpeg_ScanResult_t   Jpeg_Scan_Segments      ( const jpeg_Data_t * const data, jpeg_Size_t size );
static jpeg_ScanResult_t   Jpeg_Scan_Header        ( const jpeg_Data_t * const data, jpeg_Size_t size );
static jpeg_Quality_t      Jpeg_Calc_Quality       ( void );
static jpeg_RequestState_t Jpeg_Read_ImageInfo     ( jpeg_ImageInfo_t * const imageInfo );
static void                Jpeg_Check_Header       ( void );
static void                Jpeg_Notify_HeaderParsed ( const jpeg_ImageInfo_t * const imageInfo );
static void                Jpeg_Notify_InputNeeded ( void );
static void                Jpeg_Notify_OutputReady ( jpeg_Size_t length );
static void                Jpeg_Notify_EndOfConversion ( jpeg_Size_t totalLength );
static void                Jpeg_Notify_Error       ( jpeg_Error_t errorMask );
static jpeg_RequestState_t Jpeg_Cache_Maintain     ( jpeg_CacheOp_t operation,
                                                     uint32_t address,
                                                     jpeg_Size_t size );
static jpeg_RequestState_t Jpeg_Dma_Init           ( void );
static jpeg_RequestState_t Jpeg_Dma_Deinit         ( void );
static jpeg_RequestState_t Jpeg_Dma_Stop           ( void );
static jpeg_RequestState_t Jpeg_Dma_Arm            ( jpeg_DmaStream_t stream,
                                                     uint32_t memoryAddr,
                                                     jpeg_Size_t byteCnt );
static void                Jpeg_Dma_Get_XferConfig ( jpeg_DmaStream_t stream,
                                                     uint32_t memoryAddr,
                                                     jpeg_Size_t byteCnt,
                                                     gpdma_TransferConfig_t * const xferConfig );
static gpdma_ChannelId_t   Jpeg_Dma_Get_ChannelId  ( jpeg_DmaStream_t stream );
static void                Jpeg_Dma_End            ( void );
static void                Jpeg_Dma_InputComplete  ( void );
static void                Jpeg_Dma_OutputComplete ( void );
static void                Jpeg_Dma_Error          ( gpdma_ErrorMaskId_t errorMask );
static jpeg_RequestState_t Jpeg_Release            ( void );
static void                Jpeg_Reset_Runtime      ( void );
static void                Jpeg_IsrHandler         ( void );

#endif /* JPEG */

/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Value of major version of SW module */
#define JPEG_MAJOR_VERSION               ( 0u )

/** Value of minor version of SW module */
#define JPEG_MINOR_VERSION               ( 1u )

/** Value of patch version of SW module */
#define JPEG_PATCH_VERSION               ( 0u )

/** Default interrupt priority used by \ref Jpeg_Get_DefaultConfig */
#define JPEG_DEFAULT_IRQ_PRIO            ( 0u )

/** Quality factor of the module after \ref Jpeg_Init */
#define JPEG_DEFAULT_QUALITY             ( 75u )

/** Register value without any bit set */
#define JPEG_BITS_CLEAR                  ( 0u )

/** Size of a word of the FIFOs in bytes */
#define JPEG_WORD_SIZE                   ( 4u )

/** Count of the bits of a byte */
#define JPEG_BYTE_BITS                   ( 8u )

/** Mask of a byte */
#define JPEG_BYTE_MASK                   ( 0xFFu )

/** Count of the words a FIFO of the codec holds (32 bytes), the limit of the words moved in one call */
#define JPEG_FIFO_WORDS                  ( 8u )

/** Size of an 8 x 8 block in bytes (one byte per sample) */
#define JPEG_BLOCK_BYTES                 ( 64u )

/** Side of an 8 x 8 block in pixels */
#define JPEG_BLOCK_SIDE                  ( 8u )

/** Quality factor from which the scale falls linearly */
#define JPEG_QUALITY_SPLIT               ( 50u )

/** Numerator of the scale below \ref JPEG_QUALITY_SPLIT */
#define JPEG_QUALITY_LOW_REF             ( 5000u )

/** Offset of the scale from \ref JPEG_QUALITY_SPLIT */
#define JPEG_QUALITY_HIGH_REF            ( 200u )

/** Slope of the scale from \ref JPEG_QUALITY_SPLIT */
#define JPEG_QUALITY_HIGH_SLOPE          ( 2u )

/** Divisor of the scaled quantization value (the scale is in percent) */
#define JPEG_QUANT_SCALE_DIV             ( 100u )

/** Rounding offset of the scaled quantization value */
#define JPEG_QUANT_ROUND                 ( 50u )

/** Largest value of a quantization table */
#define JPEG_QUANT_VALUE_MAX             ( 255u )

/** Smallest value of a quantization table */
#define JPEG_QUANT_VALUE_MIN             ( 1u )

/** Count of the coefficients of the quantization memory word */
#define JPEG_QUANT_PER_WORD              ( 4u )

/** Count of the entries of a DC Huffman encoder table (the categories 0 to 11) */
#define JPEG_HUFF_DC_ENTRIES             ( 12u )

/** Count of the entries of an AC Huffman encoder table in the order of the hardware (162 symbols) */
#define JPEG_HUFF_AC_ENTRIES             ( 162u )

/** Index of the entry of the symbol EOB (0x00) in an AC Huffman encoder table */
#define JPEG_HUFF_AC_EOB_INDEX           ( 160u )

/** Index of the entry of the symbol ZRL (0xF0) in an AC Huffman encoder table */
#define JPEG_HUFF_AC_ZRL_INDEX           ( 161u )

/** AC symbol EOB (end of block) */
#define JPEG_HUFF_SYMBOL_EOB             ( 0x00u )

/** AC symbol ZRL (run of 16 zeros) */
#define JPEG_HUFF_SYMBOL_ZRL             ( 0xF0u )

/** Mask of the size (the low nibble) of an AC symbol */
#define JPEG_HUFF_SIZE_MASK              ( 0x0Fu )

/** Shift of the run (the high nibble) of an AC symbol */
#define JPEG_HUFF_RUN_SHIFT              ( 4u )

/** Count of the sizes 1 to 10 of an AC symbol: the index of the entry is run * 10 + size - 1 */
#define JPEG_HUFF_SIZE_CNT               ( 10u )

/** Entry of a symbol which is not in the Huffman table (length 16 and code 0xFF) */
#define JPEG_HUFF_ENTRY_EMPTY            ( 0x0FFFu )

/** Marker of an entry whose symbol was not met while the entries are assigned (not a valid entry) */
#define JPEG_HUFF_ENTRY_FREE             ( 0xFFFFFFFFu )

/** Mask of the code of an entry (the low eight bits of the code are stored) */
#define JPEG_HUFF_CODE_MASK              ( 0xFFu )

/** Mask of the length - 1 of an entry */
#define JPEG_HUFF_LENGTH_MASK            ( 0x0Fu )

/** Shift of the length - 1 in an entry */
#define JPEG_HUFF_LENGTH_SHIFT           ( 8u )

/** Shift of the second entry of a word of the Huffman encoder memory */
#define JPEG_HUFF_ENTRY_SHIFT            ( 16u )

/** Two empty entries: the word of the unused part of a Huffman encoder memory */
#define JPEG_HUFF_WORD_EMPTY             ( 0x0FFF0FFFu )

/** Words behind the AC entries: three empty words and the entries of the markers RST0 to RST7 (0xD0 to 0xD7) */
#define JPEG_HUFF_AC_TAIL_WORDS          { JPEG_HUFF_WORD_EMPTY, JPEG_HUFF_WORD_EMPTY, JPEG_HUFF_WORD_EMPTY, \
                                           0x0FD10FD0u, 0x0FD30FD2u, 0x0FD50FD4u, 0x0FD70FD6u }

/** Count of the words behind the AC entries */
#define JPEG_HUFF_AC_TAIL_CNT            ( 7u )

/** Count of the entries of a word of the Huffman encoder memory */
#define JPEG_HUFF_PER_WORD               ( 2u )

/** Count of the words of the AC entries */
#define JPEG_HUFF_AC_WORDS               ( JPEG_HUFF_AC_ENTRIES / JPEG_HUFF_PER_WORD )

/** Offset of the tables in the image of the DHT memory (code counts then the symbols, in the order of the DHT
 *  memory: DC0, AC0, DC1, AC1) */
#define JPEG_DHT_DC0_OFFSET              ( 0u )
#define JPEG_DHT_AC0_OFFSET              ( JPEG_DHT_DC0_OFFSET + JPEG_HUFFMAN_LENGTH_CNT + JPEG_HUFF_DC_ENTRIES )
#define JPEG_DHT_DC1_OFFSET              ( JPEG_DHT_AC0_OFFSET + JPEG_HUFFMAN_LENGTH_CNT + JPEG_HUFFMAN_AC_VALUES_MAX )
#define JPEG_DHT_AC1_OFFSET              ( JPEG_DHT_DC1_OFFSET + JPEG_HUFFMAN_LENGTH_CNT + JPEG_HUFF_DC_ENTRIES )

/** Size of the image of the DHT memory in bytes */
#define JPEG_DHT_IMAGE_SIZE              ( JPEG_REG_DHT_WORD_CNT * JPEG_WORD_SIZE )

_Static_assert( ( JPEG_DHT_AC1_OFFSET + JPEG_HUFFMAN_LENGTH_CNT + JPEG_HUFFMAN_AC_VALUES_MAX ) == JPEG_DHT_IMAGE_SIZE,
                "Jpeg: the tables do not fill the DHT memory." );

/** Count of the quantization tables of a color space is the register value of the color space + 1 */
#define JPEG_COLOR_TABLE_OFFSET          ( 1u )

/** Interrupt enable bits of the FIFOs and of the events (without the DMA requests) */
#define JPEG_CR_IRQ_MASK                 ( JPEG_CR_IFTIE   | JPEG_CR_IFNFIE | JPEG_CR_OFTIE | \
                                           JPEG_CR_OFNEIE  | JPEG_CR_EOCIE  | JPEG_CR_HPDIE    )

/** DMA request enable bits */
#define JPEG_CR_DMA_MASK                 ( JPEG_CR_IDMAEN | JPEG_CR_ODMAEN )

/** Largest block of a Gpdma channel in bytes (the block size has 16 bits), a multiple of the word size */
#define JPEG_DMA_SIZE_MAX                ( 65532u )

/** Alignment of the buffers of the DMA mode in bytes (the word of the FIFOs) */
#define JPEG_DMA_ALIGN                   ( 4u )

/** Alignment of the address and of the size of a cache range in bytes */
#define JPEG_CACHE_ALIGN                 ( 16u )

/** Count of the transfers of a Gpdma channel (one transfer list node) */
#define JPEG_DMA_TRANSFERS_CNT           ( 1u )

/** Count of the data items of a Gpdma burst (one request, one word) */
#define JPEG_DMA_BURST_LEN               ( 1u )

/** Size of the transfer configured at the initialization of a Gpdma channel (placeholder) in bytes */
#define JPEG_DMA_PLACEHOLDER_SIZE        ( 4u )

/** Gpdma errors which end the job */
#define JPEG_DMA_ERROR_MASK              ( GPDMA_ERROR_TRANSFER      | \
                                           GPDMA_ERROR_CONFIG_UPDATE | \
                                           GPDMA_ERROR_CONFIG_ERROR  | \
                                           GPDMA_ERROR_TRIG_OVERRUN    )

/** Prefix of every marker of a JPEG stream */
#define JPEG_MARKER_PREFIX               ( 0xFFu )

/** Count of the bytes of a marker */
#define JPEG_MARKER_SIZE                 ( 2u )

/** Marker SOI (start of image) */
#define JPEG_MARKER_SOI                  ( 0xD8u )

/** Marker EOI (end of image) */
#define JPEG_MARKER_EOI                  ( 0xD9u )

/** Marker SOS (start of scan) */
#define JPEG_MARKER_SOS                  ( 0xDAu )

/** Marker SOF0 (start of frame, baseline) */
#define JPEG_MARKER_SOF0                 ( 0xC0u )

/** Marker SOF1 (start of frame, extended sequential, Huffman) */
#define JPEG_MARKER_SOF1                 ( 0xC1u )

/** Marker SOF15, the last of the start of frame markers */
#define JPEG_MARKER_SOF15                ( 0xCFu )

/** Marker DHT (define Huffman table), between the start of frame markers but not one of them */
#define JPEG_MARKER_DHT                  ( 0xC4u )

/** Marker JPG (reserved for JPEG extensions), between the start of frame markers but not one of them */
#define JPEG_MARKER_JPG                  ( 0xC8u )

/** Marker DAC (define arithmetic coding conditioning), between the start of frame markers but not one of them */
#define JPEG_MARKER_DAC                  ( 0xCCu )

/** Marker TEM (temporary, no length) */
#define JPEG_MARKER_TEM                  ( 0x01u )

/** Marker RST0, the first restart marker (no length) */
#define JPEG_MARKER_RST0                 ( 0xD0u )

/** Marker RST7, the last restart marker (no length) */
#define JPEG_MARKER_RST7                 ( 0xD7u )

/** Offset of the precision (bits per sample) in a start of frame segment: the marker and the length precede it */
#define JPEG_SOF_PRECISION_OFFSET        ( 4u )

/** Precision the hardware supports */
#define JPEG_SOF_PRECISION               ( 8u )

/** Offset of the length of a marker segment behind the prefix of its marker */
#define JPEG_SEGMENT_LENGTH_OFFSET       ( 2u )

/** Size of the length field of a marker segment, which the length includes */
#define JPEG_SEGMENT_LENGTH_SIZE         ( 2u )

/** Largest count of the marker segments scanned in front of the start of frame */
#define JPEG_SCAN_SEGMENT_MAX            ( 64u )

/** Sampling factors of the first component for the subsamplings */
#define JPEG_SAMPLING_444                ( 1u )
#define JPEG_SAMPLING_DOUBLE             ( 2u )

/** Value of JPEG_CONFR1.NF for the color spaces: count of the components - 1 */
#define JPEG_NF_GRAYSCALE                ( 0u )
#define JPEG_NF_THREE                    ( 2u )
#define JPEG_NF_CMYK                     ( 3u )

/** Rows of the quantization table selected by the components of a color space (the others are not used) */
#define JPEG_QUANT_SELECT_GRAY           { 0u, 0u, 0u, 0u }
#define JPEG_QUANT_SELECT_YCBCR          { 0u, 1u, 1u, 0u }
#define JPEG_QUANT_SELECT_RGB            { 0u, 1u, 2u, 0u }
#define JPEG_QUANT_SELECT_CMYK           { 0u, 1u, 2u, 3u }

/** Rows of the Huffman tables selected by the components of a color space */
#define JPEG_HUFF_SELECT_GRAY            { 0u, 0u, 0u, 0u }
#define JPEG_HUFF_SELECT_YCBCR           { 0u, 1u, 1u, 0u }

/* =============================== MACROS =================================== */

/* ========================== EXPORTED VARIABLES ============================ */

/* =========================== LOCAL VARIABLES ============================== */

#if defined(JPEG)

/** \brief Runtime data of the module */
static jpeg_RuntimeData_t jpeg_RuntimeData =
{
    .InitState      = JPEG_FUNCTION_INACTIVE,
    .XferMode       = JPEG_XFER_MODE_POLL,
    .CpuServed      = JPEG_FUNCTION_ACTIVE,
    .DmaPeriphId    = GPDMA_PERIPH_1,
    .DmaInputChannelId  = GPDMA_CHANNEL_0,
    .DmaOutputChannelId = GPDMA_CHANNEL_1,
    .DmaPriority    = GPDMA_PRIORITY_LOW,
    .HeaderMode     = JPEG_HEADER_ON,
    .Quality        = JPEG_DEFAULT_QUALITY,
    .State          = JPEG_STATE_IDLE,
    .HeaderParsed   = JPEG_FUNCTION_INACTIVE,
    .EocWait        = JPEG_FUNCTION_INACTIVE,
    .ImageInfoValid = JPEG_FUNCTION_INACTIVE,
    .InPending      = JPEG_FUNCTION_INACTIVE,
    .InPaused       = JPEG_FUNCTION_INACTIVE,
    .OutPending     = JPEG_FUNCTION_INACTIVE,
    .OutPaused      = JPEG_FUNCTION_INACTIVE,
};

/** \brief Count of the components of the color spaces */
static const jpeg_Count_t jpeg_ComponentCntLut[ JPEG_COLOR_CNT ] = { 1u, 3u, 3u, 4u };

/** \brief Horizontal sampling factor of the first component (Y) of the subsamplings */
static const jpeg_Count_t jpeg_YFactorHLut[ JPEG_SUBSAMPLING_CNT ] = { 1u, 2u, 2u };

/** \brief Vertical sampling factor of the first component (Y) of the subsamplings */
static const jpeg_Count_t jpeg_YFactorVLut[ JPEG_SUBSAMPLING_CNT ] = { 1u, 1u, 2u };

/** \brief Quantization table of every component, indexed by the color space */
static const uint32_t jpeg_QuantSelectLut[ JPEG_COLOR_CNT ][ JPEG_REG_COMPONENT_CNT ] =
{
    [JPEG_COLOR_GRAYSCALE] = JPEG_QUANT_SELECT_GRAY,
    [JPEG_COLOR_YCBCR]     = JPEG_QUANT_SELECT_YCBCR,
    [JPEG_COLOR_RGB]       = JPEG_QUANT_SELECT_RGB,
    [JPEG_COLOR_CMYK]      = JPEG_QUANT_SELECT_CMYK,
};

/** \brief Huffman tables (AC and DC) of every component, indexed by the color space */
static const uint32_t jpeg_HuffSelectLut[ JPEG_COLOR_CNT ][ JPEG_REG_COMPONENT_CNT ] =
{
    [JPEG_COLOR_GRAYSCALE] = JPEG_HUFF_SELECT_GRAY,
    [JPEG_COLOR_YCBCR]     = JPEG_HUFF_SELECT_YCBCR,
    [JPEG_COLOR_RGB]       = JPEG_HUFF_SELECT_GRAY,
    [JPEG_COLOR_CMYK]      = JPEG_HUFF_SELECT_GRAY,
};

/** \brief Standard quantization table of every base table (the tables 2 and 3 take the chrominance and the
 *         luminance table), indexed by \ref jpeg_QuantId_t */
static const jpeg_StdQuant_t jpeg_StdQuantLut[ JPEG_QUANT_TABLE_CNT ] =
{
    [JPEG_QUANT_TABLE_0] = JPEG_STD_QUANT_LUM,
    [JPEG_QUANT_TABLE_1] = JPEG_STD_QUANT_CHROM,
    [JPEG_QUANT_TABLE_2] = JPEG_STD_QUANT_CHROM,
    [JPEG_QUANT_TABLE_3] = JPEG_STD_QUANT_LUM,
};

/** \brief Offset of the tables in the image of the DHT memory, indexed by \ref jpeg_HuffmanId_t */
static const uint32_t jpeg_DhtOffsetLut[ JPEG_HUFFMAN_CNT ] =
{
    [JPEG_HUFFMAN_DC_0] = JPEG_DHT_DC0_OFFSET,
    [JPEG_HUFFMAN_DC_1] = JPEG_DHT_DC1_OFFSET,
    [JPEG_HUFFMAN_AC_0] = JPEG_DHT_AC0_OFFSET,
    [JPEG_HUFFMAN_AC_1] = JPEG_DHT_AC1_OFFSET,
};

/** \brief Count of the symbols of the tables in the DHT memory, indexed by \ref jpeg_HuffmanId_t */
static const uint32_t jpeg_DhtValueCntLut[ JPEG_HUFFMAN_CNT ] =
{
    [JPEG_HUFFMAN_DC_0] = JPEG_HUFF_DC_ENTRIES,
    [JPEG_HUFFMAN_DC_1] = JPEG_HUFF_DC_ENTRIES,
    [JPEG_HUFFMAN_AC_0] = JPEG_HUFFMAN_AC_VALUES_MAX,
    [JPEG_HUFFMAN_AC_1] = JPEG_HUFFMAN_AC_VALUES_MAX,
};

/** \brief Gpdma request of the channels */
static const gpdma_PeriphReqId_t jpeg_DmaRequestLut[ JPEG_DMA_STREAM_CNT ] =
{
    [JPEG_DMA_STREAM_INPUT]  = GPDMA_REQ_JPEG_RX,
    [JPEG_DMA_STREAM_OUTPUT] = GPDMA_REQ_JPEG_TX,
};

/** \brief Direction of the transfer of the channels */
static const gpdma_Direction_t jpeg_DmaDirectionLut[ JPEG_DMA_STREAM_CNT ] =
{
    [JPEG_DMA_STREAM_INPUT]  = GPDMA_DIR_MEMORY_TO_PERIPH,
    [JPEG_DMA_STREAM_OUTPUT] = GPDMA_DIR_PERIPH_TO_MEMORY,
};

/** \brief Gpdma channel type (the channels 6 and 7 support the 2D addressing): layout of the transfer list node */
static const gpdma_ChannelType_t jpeg_DmaChannelTypeLut[ GPDMA_CHANNEL_CNT ] =
{
    [GPDMA_CHANNEL_0] = GPDMA_CHANNEL_LINEAR,
    [GPDMA_CHANNEL_1] = GPDMA_CHANNEL_LINEAR,
    [GPDMA_CHANNEL_2] = GPDMA_CHANNEL_LINEAR,
    [GPDMA_CHANNEL_3] = GPDMA_CHANNEL_LINEAR,
    [GPDMA_CHANNEL_4] = GPDMA_CHANNEL_LINEAR,
    [GPDMA_CHANNEL_5] = GPDMA_CHANNEL_LINEAR,
    [GPDMA_CHANNEL_6] = GPDMA_CHANNEL_LINEAR_2D,
    [GPDMA_CHANNEL_7] = GPDMA_CHANNEL_LINEAR_2D,
};

/** \brief Gpdma transfer list nodes of the channels (must be static, used by the Gpdma hardware) */
static gpdma_XferList_t jpeg_DmaXferList[ JPEG_DMA_STREAM_CNT ];

/** \brief Memory of the placeholder transfer configured at the initialization of the Gpdma channels */
static uint32_t jpeg_DmaDummy;

/** \brief Size in bytes of the block armed in the Gpdma channels */
static jpeg_Size_t jpeg_DmaArmedSize[ JPEG_DMA_STREAM_CNT ];

#endif /* JPEG */

/* ========================= EXPORTED FUNCTIONS ============================= */

/**
 * \brief Returns module SW version
 *
 * \return Module SW version
 */
jpeg_ModuleVersion_t Jpeg_Get_ModuleVersion( void )
{
    jpeg_ModuleVersion_t retVersion;

    retVersion.Major = JPEG_MAJOR_VERSION;
    retVersion.Minor = JPEG_MINOR_VERSION;
    retVersion.Patch = JPEG_PATCH_VERSION;

    return ( retVersion );
}


/**
 * \brief Fills configuration structure with default values.
 *
 * Default configuration: POLL transfer mode, Gpdma1 channel 0 for the input stream and channel 1 for the output
 * stream at low priority (DMA mode only), the sample tables, the header generation on, interrupt priority 0, no
 * callback.
 *
 * \param jpegConfig [out]: Pointer to store default configuration. Must not be NULL.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Get_DefaultConfig( jpeg_Config_t * const jpegConfig )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( JPEG_NULL_PTR != jpegConfig )
    {
        jpegConfig->XferMode                = JPEG_XFER_MODE_POLL;
        jpegConfig->DmaPeriphId             = GPDMA_PERIPH_1;
        jpegConfig->DmaInputChannelId       = GPDMA_CHANNEL_0;
        jpegConfig->DmaOutputChannelId      = GPDMA_CHANNEL_1;
        jpegConfig->DmaPriority             = GPDMA_PRIORITY_LOW;
        jpegConfig->StandardTables          = JPEG_FUNCTION_ACTIVE;
        jpegConfig->HeaderMode              = JPEG_HEADER_ON;
        jpegConfig->IrqPriority             = JPEG_DEFAULT_IRQ_PRIO;
        jpegConfig->HeaderParsedCallback    = JPEG_NULL_PTR;
        jpegConfig->InputNeededCallback     = JPEG_NULL_PTR;
        jpegConfig->OutputReadyCallback     = JPEG_NULL_PTR;
        jpegConfig->EndOfConversionCallback = JPEG_NULL_PTR;
        jpegConfig->ErrorCallback           = JPEG_NULL_PTR;

        retState = JPEG_REQUEST_OK;
    }
    else
    {
        /* Invalid pointer, nothing is filled */
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)jpegConfig;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Initializes the Jpeg module.
 *
 * Enables the peripheral clock (Rcc), enables the core of the codec and leaves it idle, loads the sample tables
 * (when \ref jpeg_Config_t::StandardTables is active) and, for \ref JPEG_XFER_MODE_ISR and
 * \ref JPEG_XFER_MODE_DMA, configures the JPEG interrupt (Nvic); in DMA mode the two Gpdma channels are initialized
 * too. The result is verified by a read-back of the registers, on a failure the module is deinitialized and
 * \ref JPEG_REQUEST_ERROR is returned. An initialized module has to be deinitialized by \ref Jpeg_Deinit before
 * the next initialization.
 *
 * \pre   \p jpegConfig shall not be \ref JPEG_NULL_PTR, the transfer mode and the header mode shall be valid values
 *        and, in DMA mode, the Gpdma controller, the two different channels and the priority shall be valid;
 *        otherwise \ref JPEG_REQUEST_ERROR and nothing changes.
 *
 * \param jpegConfig [in]: Pointer to Jpeg configuration, see \ref jpeg_Config_t. Must not be NULL.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Init( const jpeg_Config_t * const jpegConfig )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    const jpeg_RequestState_t configState = Jpeg_Check_Config( jpegConfig );

    if( ( JPEG_REQUEST_OK        == configState                ) &&
        ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.InitState )    )
    {
        const rcc_RequestState_t rccState = Rcc_Set_PeriphActive( RCC_PERIPH_JPEG );

        if( RCC_REQUEST_OK == rccState )
        {
            Jpeg_Reset_Runtime();

            jpeg_RuntimeData.XferMode                = jpegConfig->XferMode;
            jpeg_RuntimeData.DmaPeriphId             = jpegConfig->DmaPeriphId;
            jpeg_RuntimeData.DmaInputChannelId       = jpegConfig->DmaInputChannelId;
            jpeg_RuntimeData.DmaOutputChannelId      = jpegConfig->DmaOutputChannelId;
            jpeg_RuntimeData.DmaPriority             = jpegConfig->DmaPriority;
            jpeg_RuntimeData.HeaderMode              = jpegConfig->HeaderMode;
            jpeg_RuntimeData.HeaderParsedCallback    = jpegConfig->HeaderParsedCallback;
            jpeg_RuntimeData.InputNeededCallback     = jpegConfig->InputNeededCallback;
            jpeg_RuntimeData.OutputReadyCallback     = jpegConfig->OutputReadyCallback;
            jpeg_RuntimeData.EndOfConversionCallback = jpegConfig->EndOfConversionCallback;
            jpeg_RuntimeData.ErrorCallback           = jpegConfig->ErrorCallback;

            if( JPEG_XFER_MODE_DMA == jpegConfig->XferMode )
            {
                jpeg_RuntimeData.CpuServed = JPEG_FUNCTION_INACTIVE;
            }
            else
            {
                jpeg_RuntimeData.CpuServed = JPEG_FUNCTION_ACTIVE;
            }

            if( JPEG_FUNCTION_ACTIVE == jpegConfig->StandardTables )
            {
                Jpeg_Load_StandardTables();
            }
            else
            {
                /* The tables are given by the application */
            }

            retState = Jpeg_Init_Hardware( jpegConfig->IrqPriority );

            if( JPEG_REQUEST_OK == retState )
            {
                jpeg_RuntimeData.InitState = JPEG_FUNCTION_ACTIVE;
            }
            else
            {
                /* Initialization failed, the module is deinitialized */
                (void)Jpeg_Release();
                retState = JPEG_REQUEST_ERROR;
            }
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)jpegConfig;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Deinitializes module Jpeg
 *
 * Stops the running job, disables the core and the JPEG interrupt in Nvic, releases the Gpdma channels, resets the
 * peripheral and disables its clock (Rcc), removes the callbacks and marks the module as not initialized (every
 * other function returns \ref JPEG_REQUEST_ERROR afterwards).
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Deinit( void )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState )
    {
        retState = Jpeg_Release();
    }
    else
    {
        /* The module is not initialized */
        retState = JPEG_REQUEST_ERROR;
    }
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Main task of module Jpeg
 *
 * In \ref JPEG_XFER_MODE_POLL the FIFOs of the codec are served by this function: it moves the words of the input
 * buffer into the input FIFO while it has a free place, the words of the output FIFO into the output buffer while it
 * has data, and ends the job. It has to be called periodically. In the other modes the interrupts serve the codec
 * and the function does nothing.
 */
void Jpeg_Task( void )
{
#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_XFER_MODE_POLL  == jpeg_RuntimeData.XferMode  ) &&
        ( JPEG_STATE_IDLE      != jpeg_RuntimeData.State     )    )
    {
        Jpeg_Service();
    }
    else
    {
        /* Nothing to serve */
    }
#endif /* JPEG */

    return;
}


/**
 * \brief Starts the encoder.
 *
 * Checks the image, the tables of its color space and the state, programs the codec (image, quantization tables
 * scaled by the quality, Huffman tables and the DHT memory) and starts it. The buffers are given afterwards by
 * \ref Jpeg_Set_InputBuffer and \ref Jpeg_Set_OutputBuffer. The job ends when the codec consumed the MCUs of the
 * image, see \ref Jpeg_Get_RawSize for their size.
 *
 * \pre   The module is initialized, no job runs, \p imageConfig is valid (the width and the height at least 1, the
 *        color space and the subsampling listed, the subsampling 4:4:4 unless YCbCr, the quality factor 1 to 100
 *        or \ref JPEG_QUALITY_MODULE) and the tables the color space needs are available; otherwise
 *        \ref JPEG_REQUEST_ERROR and no register is written.
 *
 * \param imageConfig [in]: Image to encode. Must not be NULL.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_EncodeStart( const jpeg_ImageConfig_t * const imageConfig )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    const jpeg_RequestState_t checkState = Jpeg_Check_ImageConfig( imageConfig );

    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_STATE_IDLE      == jpeg_RuntimeData.State     ) &&
        ( JPEG_REQUEST_OK      == checkState                 )    )
    {
        retState = Jpeg_Start_Job( imageConfig );
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)imageConfig;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Starts the decoder.
 *
 * Stops and resets the codec, enables the header parsing (the parser loads the image parameters and the tables from
 * the stream) and starts the codec. The stream is given afterwards by \ref Jpeg_Set_InputBuffer, the first buffer
 * is scanned for the start of frame, a stream which is not a JPEG stream or whose frame the hardware does not
 * decode (progressive, lossless, hierarchical, arithmetic coded, samples of other than 8 bit) ends the job with
 * \ref JPEG_ERROR_STREAM or \ref JPEG_ERROR_UNSUPPORTED. When the parser finished, the header callback reports the
 * image, the application gives the output buffers (\ref Jpeg_Set_OutputBuffer) for the raw MCUs. The job ends when
 * the codec decoded the image; the header and the data are not checked any further by the hardware.
 *
 * \pre   The module is initialized and no job runs, otherwise \ref JPEG_REQUEST_ERROR and no register is written.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_DecodeStart( void )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_STATE_IDLE      == jpeg_RuntimeData.State     )    )
    {
        retState = Jpeg_Start_Decode();
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Aborts the running job.
 *
 * Stops the codec and the Gpdma channels, disables the interrupts and the DMA requests, flushes both FIFOs and
 * returns the module to the idle state. The callbacks are not called.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if a job was aborted.
 *         Otherwise (no job runs, the module is not initialized) returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_Abort( void )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_STATE_IDLE      != jpeg_RuntimeData.State     )    )
    {
        retState = Jpeg_Abort_Job();
    }
    else
    {
        /* No job runs */
        retState = JPEG_REQUEST_ERROR;
    }
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Pauses a stream of the running job: the data of the stream are not moved until \ref Jpeg_Set_Resume (the
 *        FIFO of the codec stalls, the Gpdma channel is suspended).
 *
 * \param stream [in]: Stream to pause.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_Pause( jpeg_Stream_t stream )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_STATE_IDLE      != jpeg_RuntimeData.State     ) &&
        ( JPEG_STREAM_CNT      >  stream                     )    )
    {
        gpdma_RequestState_t dmaState = GPDMA_REQUEST_OK;

        if( JPEG_STREAM_INPUT == stream )
        {
            jpeg_RuntimeData.InPaused = JPEG_FUNCTION_ACTIVE;

            if( ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.CpuServed ) &&
                ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.InPending )    )
            {
                dmaState = Gpdma_Set_PauseActive( jpeg_RuntimeData.DmaPeriphId, jpeg_RuntimeData.DmaInputChannelId );
            }
            else
            {
                /* The CPU serves the stream or no buffer is armed */
            }
        }
        else
        {
            jpeg_RuntimeData.OutPaused = JPEG_FUNCTION_ACTIVE;

            if( ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.CpuServed  ) &&
                ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.OutPending )    )
            {
                dmaState = Gpdma_Set_PauseActive( jpeg_RuntimeData.DmaPeriphId, jpeg_RuntimeData.DmaOutputChannelId );
            }
            else
            {
                /* The CPU serves the stream or no buffer is armed */
            }
        }

        Jpeg_Update_Irq();

        if( GPDMA_REQUEST_OK == dmaState )
        {
            retState = JPEG_REQUEST_OK;
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)stream;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Resumes a stream paused by \ref Jpeg_Set_Pause.
 *
 * \param stream [in]: Stream to resume.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_Resume( jpeg_Stream_t stream )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_STATE_IDLE      != jpeg_RuntimeData.State     ) &&
        ( JPEG_STREAM_CNT      >  stream                     )    )
    {
        gpdma_RequestState_t dmaState = GPDMA_REQUEST_OK;

        if( JPEG_STREAM_INPUT == stream )
        {
            jpeg_RuntimeData.InPaused = JPEG_FUNCTION_INACTIVE;

            if( ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.CpuServed ) &&
                ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.InPending )    )
            {
                dmaState = Gpdma_Set_PauseInactive( jpeg_RuntimeData.DmaPeriphId,
                                                    jpeg_RuntimeData.DmaInputChannelId );
            }
            else
            {
                /* The CPU serves the stream or no buffer is armed */
            }
        }
        else
        {
            jpeg_RuntimeData.OutPaused = JPEG_FUNCTION_INACTIVE;

            if( ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.CpuServed  ) &&
                ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.OutPending )    )
            {
                dmaState = Gpdma_Set_PauseInactive( jpeg_RuntimeData.DmaPeriphId,
                                                    jpeg_RuntimeData.DmaOutputChannelId );
            }
            else
            {
                /* The CPU serves the stream or no buffer is armed */
            }
        }

        Jpeg_Update_Irq();

        if( GPDMA_REQUEST_OK == dmaState )
        {
            retState = JPEG_REQUEST_OK;
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)stream;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Returns the state of the codec.
 *
 * \param jpegState [out]: Pointer to store the state. Must not be NULL.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Get_State( jpeg_State_t * const jpegState )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_NULL_PTR        != jpegState                  )    )
    {
        *jpegState = jpeg_RuntimeData.State;

        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)jpegState;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Gives the codec the next buffer of the input stream.
 *
 * The buffer is served in the background (see the transfer modes); when it is consumed the input needed callback
 * is called. Only one buffer is served at a time.
 *
 * \pre   A job runs, no input buffer is waiting, \p buffer is not NULL and \p size is at least 1. In DMA mode the
 *        buffer is aligned to 4 bytes and its size is a multiple of 4 bytes up to 65532 bytes (and, with the data
 *        cache enabled, aligned to 16 bytes). Otherwise \ref JPEG_REQUEST_ERROR.
 *
 * \param buffer [in]: Input data. Must not be NULL.
 * \param size   [in]: Count of the bytes of the buffer. A size which is not a multiple of 4 bytes ends the stream
 *                     (the last word is padded with zeros).
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_InputBuffer( const jpeg_Data_t * const buffer, jpeg_Size_t size )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.InitState ) &&
        ( JPEG_STATE_IDLE        != jpeg_RuntimeData.State     ) &&
        ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.InPending ) &&
        ( JPEG_NULL_PTR          != buffer                     ) &&
        ( 0u                     <  size                       )    )
    {
        retState = Jpeg_Take_Input( buffer, size );
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)buffer;
    (void)size;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Gives the codec the next buffer of the output stream.
 *
 * The buffer is filled in the background (see the transfer modes); when it is full the output ready callback is
 * called, the last buffer of a job is reported with the count of its valid bytes. Only one buffer is served at a
 * time.
 *
 * \pre   A job runs, no output buffer is waiting, \p buffer is not NULL and \p size is a multiple of 4 bytes, at
 *        least 4. In DMA mode the buffer is aligned to 4 bytes and its size is up to 65532 bytes (and, with the
 *        data cache enabled, aligned to 16 bytes and the size a multiple of 16). Otherwise \ref JPEG_REQUEST_ERROR.
 *
 * \param buffer [out]: Output data. Must not be NULL.
 * \param size   [in]:  Count of the bytes of the buffer.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_OutputBuffer( jpeg_Data_t * const buffer, jpeg_Size_t size )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.InitState ) &&
        ( JPEG_STATE_IDLE        != jpeg_RuntimeData.State     ) &&
        ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.OutPending ) &&
        ( JPEG_NULL_PTR          != buffer                     ) &&
        ( JPEG_WORD_SIZE         <= size                       ) &&
        ( 0u                     == ( size % JPEG_WORD_SIZE )  )    )
    {
        retState = Jpeg_Take_Output( buffer, size );
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)buffer;
    (void)size;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Returns the count of the bytes the current or the last job produced (the output stream), including the
 *        bytes of the buffer being filled.
 *
 * \param length [out]: Pointer to store the count. Must not be NULL.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Get_OutputLength( jpeg_Size_t * const length )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_NULL_PTR        != length                     )    )
    {
        *length = jpeg_RuntimeData.TotalOut;

        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)length;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Returns the parameters of the image of the current or of the last job.
 *
 * \param imageInfo [out]: Pointer to store the parameters. Must not be NULL.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if the parameters are known. Otherwise returns
 *         \ref JPEG_REQUEST_ERROR (no job was started, the header of the decoder is not parsed yet).
 */
jpeg_RequestState_t Jpeg_Get_ImageInfo( jpeg_ImageInfo_t * const imageInfo )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState       ) &&
        ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.ImageInfoValid  ) &&
        ( JPEG_NULL_PTR        != imageInfo                        )    )
    {
        *imageInfo = jpeg_RuntimeData.ImageInfo;

        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)imageInfo;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Calculates the count of the MCUs and the size of the raw data of an image (the input of the encoder).
 *
 * The raw data are the 8 x 8 blocks of all components of all MCUs, one byte per sample: 64 bytes per block. An MCU
 * has 1 block (grayscale), 3 blocks (YCbCr 4:4:4 and RGB), 4 blocks (YCbCr 4:2:2 and CMYK) or 6 blocks (YCbCr
 * 4:2:0); the image is padded to whole MCUs.
 *
 * \param imageConfig [in]:  Image. Must not be NULL, see \ref Jpeg_Set_EncodeStart for the valid values.
 * \param rawSize     [out]: Pointer to store the size in bytes. Must not be NULL.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR (the module is not initialized,
 *         invalid image, size above 4 GiB - 1).
 */
jpeg_RequestState_t Jpeg_Get_RawSize( const jpeg_ImageConfig_t * const imageConfig, jpeg_Size_t * const rawSize )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    const jpeg_RequestState_t checkState = Jpeg_Check_ImageConfig( imageConfig );

    if( ( JPEG_FUNCTION_ACTIVE != jpeg_RuntimeData.InitState ) ||
        ( JPEG_REQUEST_OK      != checkState                 ) ||
        ( JPEG_NULL_PTR        == rawSize                    )    )
    {
        retState = JPEG_REQUEST_ERROR;
    }
    else
    {
        jpeg_ImageInfo_t imageInfo;

        retState = Jpeg_Calc_ImageInfo( imageConfig, &imageInfo );

        if( JPEG_REQUEST_OK == retState )
        {
            *rawSize = imageInfo.RawSize;
        }
        else
        {
            /* The raw size does not fit */
        }
    }
#else
    (void)imageConfig;
    (void)rawSize;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Sets the quality factor of the module, used by the jobs whose image has \ref JPEG_QUALITY_MODULE.
 *
 * \param quality [in]: Quality factor, \ref JPEG_QUALITY_MIN to \ref JPEG_QUALITY_MAX.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_Quality( jpeg_Quality_t quality )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_QUALITY_MIN     <= quality                    ) &&
        ( JPEG_QUALITY_MAX     >= quality                    )    )
    {
        jpeg_RuntimeData.Quality = quality;

        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)quality;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Returns the quality factor of the module.
 *
 * \param quality [out]: Pointer to store the quality factor. Must not be NULL.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Get_Quality( jpeg_Quality_t * const quality )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_NULL_PTR        != quality                    )    )
    {
        *quality = jpeg_RuntimeData.Quality;

        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)quality;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Sets a quantization table used by the next encoder jobs (while no job runs).
 *
 * \param tableId [in]: Quantization table.
 * \param table   [in]: The 64 coefficients (1 to 255) in the row order of the 8 x 8 block. Must not be NULL.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_QuantizationTable( jpeg_QuantId_t tableId, const jpeg_QuantTable_t * const table )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    const jpeg_RequestState_t checkState = Jpeg_Check_QuantTable( table );

    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_STATE_IDLE      == jpeg_RuntimeData.State     ) &&
        ( JPEG_QUANT_TABLE_CNT >  tableId                    ) &&
        ( JPEG_REQUEST_OK      == checkState                 )    )
    {
        jpeg_RuntimeData.QuantTable[ tableId ]    = *table;
        jpeg_RuntimeData.QuantTableSet[ tableId ] = JPEG_FUNCTION_ACTIVE;

        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)tableId;
    (void)table;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Sets a Huffman table used by the next encoder jobs (while no job runs).
 *
 * \param tableId [in]: Huffman table.
 * \param table   [in]: The code counts and the symbols of the DHT segment. Must not be NULL, see
 *                      \ref jpeg_HuffmanTable_t for the valid values.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_HuffmanTable( jpeg_HuffmanId_t tableId, const jpeg_HuffmanTable_t * const table )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    const jpeg_RequestState_t checkState = Jpeg_Check_HuffmanTable( tableId, table );

    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_STATE_IDLE      == jpeg_RuntimeData.State     ) &&
        ( JPEG_REQUEST_OK      == checkState                 )    )
    {
        jpeg_RuntimeData.HuffmanTable[ tableId ]    = *table;
        jpeg_RuntimeData.HuffmanTableSet[ tableId ] = JPEG_FUNCTION_ACTIVE;

        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)tableId;
    (void)table;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Sets the header generation of the encoder (while no job runs).
 *
 * \param headerMode [in]: Header generation.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_HeaderMode( jpeg_HeaderMode_t headerMode )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_STATE_IDLE      == jpeg_RuntimeData.State     ) &&
        ( JPEG_HEADER_CNT      >  headerMode                 )    )
    {
        jpeg_RuntimeData.HeaderMode = headerMode;

        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)headerMode;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Sets the priority of the JPEG interrupt (interrupt and DMA mode).
 *
 * \param irqPrio [in]: Interrupt priority.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Set_IrqPriority( jpeg_IrqPrio_t irqPrio )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_XFER_MODE_POLL  != jpeg_RuntimeData.XferMode  )    )
    {
        const nvic_RequestState_t nvicState = Nvic_Set_PeriphIrq_Prio( NVIC_PERIPH_IRQ_JPEG, irqPrio );

        if( NVIC_REQUEST_OK == nvicState )
        {
            retState = JPEG_REQUEST_OK;
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }
    }
    else
    {
        /* The module is not initialized or does not use the interrupt */
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)irqPrio;
#endif /* JPEG */

    return ( retState );
}


/**
 * \brief Returns the priority of the JPEG interrupt (interrupt and DMA mode).
 *
 * \param irqPrio [out]: Pointer to store the priority. Must not be NULL.
 *
 * \return Function processing state. Returns \ref JPEG_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
jpeg_RequestState_t Jpeg_Get_IrqPriority( jpeg_IrqPrio_t * const irqPrio )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

#if defined(JPEG)
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_XFER_MODE_POLL  != jpeg_RuntimeData.XferMode  ) &&
        ( JPEG_NULL_PTR        != irqPrio                    )    )
    {
        const nvic_RequestState_t nvicState = Nvic_Get_PeriphIrq_Prio( NVIC_PERIPH_IRQ_JPEG, irqPrio );

        if( NVIC_REQUEST_OK == nvicState )
        {
            retState = JPEG_REQUEST_OK;
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }
    }
    else
    {
        /* The module is not initialized or does not use the interrupt */
        retState = JPEG_REQUEST_ERROR;
    }
#else
    (void)irqPrio;
#endif /* JPEG */

    return ( retState );
}

/* ========================== LOCAL FUNCTIONS =============================== */

#if defined(JPEG)

/**
 * \brief Checks the configuration of \ref Jpeg_Init.
 *
 * \param jpegConfig [in]: Pointer to the configuration.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the configuration is valid. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Check_Config( const jpeg_Config_t * const jpegConfig )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

    if( JPEG_NULL_PTR != jpegConfig )
    {
        if( ( JPEG_XFER_MODE_CNT >  jpegConfig->XferMode       ) &&
            ( JPEG_HEADER_CNT    >  jpegConfig->HeaderMode     ) &&
            ( JPEG_FUNCTION_ACTIVE >= jpegConfig->StandardTables )    )
        {
            retState = JPEG_REQUEST_OK;
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }

        if( ( JPEG_REQUEST_OK  == retState                   ) &&
            ( JPEG_XFER_MODE_DMA == jpegConfig->XferMode     )    )
        {
            if( ( GPDMA_PERIPH_CNT   >  jpegConfig->DmaPeriphId        ) &&
                ( GPDMA_CHANNEL_CNT  >  jpegConfig->DmaInputChannelId  ) &&
                ( GPDMA_CHANNEL_CNT  >  jpegConfig->DmaOutputChannelId ) &&
                ( GPDMA_PRIORITY_CNT >  jpegConfig->DmaPriority        ) &&
                ( jpegConfig->DmaInputChannelId != jpegConfig->DmaOutputChannelId )    )
            {
                retState = JPEG_REQUEST_OK;
            }
            else
            {
                retState = JPEG_REQUEST_ERROR;
            }
        }
        else
        {
            /* The Gpdma channels are not used or the configuration is already invalid */
        }
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Checks the image of an encoder job.
 *
 * \param imageConfig [in]: Pointer to the image.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the image is valid. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Check_ImageConfig( const jpeg_ImageConfig_t * const imageConfig )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

    if( JPEG_NULL_PTR != imageConfig )
    {
        const jpeg_Quality_t quality = imageConfig->Quality;

        if( ( JPEG_DIMENSION_MIN  <= imageConfig->Width       ) &&
            ( JPEG_DIMENSION_MIN  <= imageConfig->Height      ) &&
            ( JPEG_COLOR_CNT      >  imageConfig->ColorSpace  ) &&
            ( JPEG_SUBSAMPLING_CNT > imageConfig->Subsampling ) &&
            ( JPEG_QUALITY_MAX    >= quality                  )    )
        {
            retState = JPEG_REQUEST_OK;
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }

        if( ( JPEG_REQUEST_OK        == retState                     ) &&
            ( JPEG_COLOR_YCBCR       != imageConfig->ColorSpace      ) &&
            ( JPEG_SUBSAMPLING_444   != imageConfig->Subsampling     )    )
        {
            /* The other color spaces have one block per component */
            retState = JPEG_REQUEST_ERROR;
        }
        else
        {
            /* The subsampling is valid */
        }
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Checks a quantization table: every coefficient is 1 to 255.
 *
 * \param table [in]: Pointer to the table.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the table is valid. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Check_QuantTable( const jpeg_QuantTable_t * const table )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

    if( JPEG_NULL_PTR != table )
    {
        retState = JPEG_REQUEST_OK;

        for( uint32_t idx = 0u; JPEG_QUANT_TABLE_SIZE > idx; idx++ )
        {
            if( JPEG_QUANT_VALUE_MIN > table->Values[ idx ] )
            {
                retState = JPEG_REQUEST_ERROR;
            }
            else
            {
                /* The coefficient is valid */
            }
        }
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Checks a Huffman table: the codes fit their lengths without the code of all ones, the count of the symbols
 *        is the sum of the code counts and the symbols are valid for the kind of the table.
 *
 * \param tableId [in]: Huffman table.
 * \param table   [in]: Pointer to the table.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the table is valid. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Check_HuffmanTable( jpeg_HuffmanId_t tableId, const jpeg_HuffmanTable_t * const table )
{
    uint32_t                  wordList[ JPEG_HUFF_AC_WORDS + JPEG_HUFF_AC_TAIL_CNT ];
    uint32_t                  wordCnt  = 0u;
    jpeg_RequestState_t       retState = JPEG_REQUEST_ERROR;

    if( ( JPEG_HUFFMAN_CNT > tableId ) &&
        ( JPEG_NULL_PTR   != table   )    )
    {
        /* The conversion to the encoder memory validates the table */
        retState = Jpeg_Build_HuffEnc( tableId, table, wordList, &wordCnt );
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns the geometry of the MCU of a color space and a chroma subsampling.
 *
 * \param colorSpace  [in]:  Color space.
 * \param subsampling [in]:  Chroma subsampling (used by YCbCr only, the others have one block per component).
 * \param layout      [out]: Geometry to fill. Must not be NULL.
 */
static void Jpeg_Get_Layout( jpeg_ColorSpace_t colorSpace,
                             jpeg_Subsampling_t subsampling,
                             jpeg_McuLayout_t * const layout )
{
    jpeg_Subsampling_t usedSubsampling = JPEG_SUBSAMPLING_444;

    if( JPEG_COLOR_YCBCR == colorSpace )
    {
        usedSubsampling = subsampling;
    }
    else
    {
        /* The other color spaces have one block per component */
    }

    layout->McuWidth  = JPEG_BLOCK_SIDE * jpeg_YFactorHLut[ usedSubsampling ];
    layout->McuHeight = JPEG_BLOCK_SIDE * jpeg_YFactorVLut[ usedSubsampling ];
    layout->BlockCnt  = ( jpeg_YFactorHLut[ usedSubsampling ] * jpeg_YFactorVLut[ usedSubsampling ] ) +
                        ( jpeg_ComponentCntLut[ colorSpace ] - 1u );
}


/**
 * \brief Calculates the count of the MCUs and the size of the raw data of a valid image.
 *
 * \param imageConfig [in]:  Image.
 * \param imageInfo   [out]: Parameters of the image to fill (the quality is the one of the image or of the module).
 *
 * \return Returns \ref JPEG_REQUEST_OK if the size fits 32 bits. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Calc_ImageInfo( const jpeg_ImageConfig_t * const imageConfig,
                                                jpeg_ImageInfo_t * const imageInfo )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;
    jpeg_McuLayout_t    layout   = { 0u, 0u, 0u };

    Jpeg_Get_Layout( imageConfig->ColorSpace, imageConfig->Subsampling, &layout );

    const jpeg_Count_t mcuColumns = ( (jpeg_Count_t)imageConfig->Width  + layout.McuWidth  - 1u ) / layout.McuWidth;
    const jpeg_Count_t mcuRows    = ( (jpeg_Count_t)imageConfig->Height + layout.McuHeight - 1u ) / layout.McuHeight;
    const jpeg_Count_t mcuBytes   = layout.BlockCnt * JPEG_BLOCK_BYTES;
    const jpeg_Count_t mcuCount   = mcuColumns * mcuRows;

    imageInfo->Width       = imageConfig->Width;
    imageInfo->Height      = imageConfig->Height;
    imageInfo->ColorSpace  = imageConfig->ColorSpace;
    imageInfo->Subsampling = imageConfig->Subsampling;
    imageInfo->Quality     = imageConfig->Quality;
    imageInfo->McuCount    = mcuCount;
    imageInfo->RawSize     = 0u;

    if( ( UINT32_MAX / mcuBytes ) >= mcuCount )
    {
        imageInfo->RawSize = mcuCount * mcuBytes;

        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Calculates the scale of the quantization tables in percent from the quality factor.
 *
 * \param quality [in]: Quality factor, \ref JPEG_QUALITY_MIN to \ref JPEG_QUALITY_MAX.
 *
 * \return 5000 / quality below 50, 200 - 2 * quality from 50.
 */
static jpeg_Count_t Jpeg_Calc_Scale( jpeg_Quality_t quality )
{
    jpeg_Count_t scale = JPEG_QUALITY_LOW_REF / (jpeg_Count_t)quality;

    if( JPEG_QUALITY_SPLIT <= quality )
    {
        scale = JPEG_QUALITY_HIGH_REF - ( JPEG_QUALITY_HIGH_SLOPE * (jpeg_Count_t)quality );
    }
    else
    {
        /* Below 50 the scale falls with the quality */
    }

    return ( scale );
}


/**
 * \brief Converts a base quantization table to the 16 words of the quantization memory: the coefficients are scaled,
 *        limited to 1 to 255 and stored in the zigzag order, four per word, the first one in the lowest byte.
 *
 * \param table    [in]:  Base table (row order).
 * \param scale    [in]:  Scale in percent.
 * \param wordList [out]: The 16 words.
 */
static void Jpeg_Build_QuantWords( const jpeg_QuantTable_t * const table,
                                   jpeg_Count_t scale,
                                   uint32_t * const wordList )
{
    for( uint32_t wordIdx = 0u; JPEG_REG_QUANT_WORD_CNT > wordIdx; wordIdx++ )
    {
        uint32_t wordValue = JPEG_BITS_CLEAR;

        for( uint32_t byteIdx = 0u; JPEG_QUANT_PER_WORD > byteIdx; byteIdx++ )
        {
            const uint32_t zigzagIdx = ( wordIdx * JPEG_QUANT_PER_WORD ) + byteIdx;
            const uint32_t baseValue = table->Values[ jpeg_ZigzagOrder[ zigzagIdx ] ];
            uint32_t       scaled    = ( ( baseValue * scale ) + JPEG_QUANT_ROUND ) / JPEG_QUANT_SCALE_DIV;

            if( JPEG_QUANT_VALUE_MIN > scaled )
            {
                scaled = JPEG_QUANT_VALUE_MIN;
            }
            else
            {
                /* The value is not below the minimum */
            }

            if( JPEG_QUANT_VALUE_MAX < scaled )
            {
                scaled = JPEG_QUANT_VALUE_MAX;
            }
            else
            {
                /* The value is not above the maximum */
            }

            wordValue |= ( scaled << ( JPEG_BYTE_BITS * byteIdx ) );
        }

        wordList[ wordIdx ] = wordValue;
    }
}


/**
 * \brief Converts a Huffman table (code counts and symbols) to the words of the Huffman encoder memory: for every
 *        symbol the length - 1 (bits 11 to 8) and the low eight bits of the code (bits 7 to 0), two symbols per word
 *        (the lower symbol in the lower half), followed by the fixed words of the unused part of the memory.
 *
 * The codes are assigned in the order of the code lengths (ITU-T T.81 Annex C). A DC symbol is its category (0 to
 * 11). An AC symbol with the run r and the size s (1 to 10) is the entry r * 10 + s - 1, EOB is the entry 160 and ZRL
 * the entry 161. A symbol which is not in the table keeps the empty entry.
 *
 * \param tableId  [in]:  Huffman table.
 * \param table    [in]:  Table (code counts and symbols).
 * \param wordList [out]: The words, at least 8 (DC) or 88 (AC) places.
 * \param wordCnt  [out]: Count of the words.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the table is valid. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Build_HuffEnc( jpeg_HuffmanId_t tableId,
                                               const jpeg_HuffmanTable_t * const table,
                                               uint32_t * const wordList,
                                               uint32_t * const wordCnt )
{
    jpeg_RequestState_t retState   = JPEG_REQUEST_OK;
    uint32_t            entryList[ JPEG_HUFF_AC_ENTRIES ];
    uint32_t            entryCnt   = JPEG_HUFF_AC_ENTRIES;
    uint32_t            symbolMax  = JPEG_HUFFMAN_AC_VALUES_MAX;
    uint32_t            symbolIdx  = 0u;
    uint32_t            code       = 0u;

    if( ( JPEG_HUFFMAN_DC_0 == tableId ) ||
        ( JPEG_HUFFMAN_DC_1 == tableId )    )
    {
        entryCnt  = JPEG_HUFF_DC_ENTRIES;
        symbolMax = JPEG_HUFF_DC_ENTRIES;
    }
    else
    {
        /* AC table */
    }

    for( uint32_t idx = 0u; JPEG_HUFF_AC_ENTRIES > idx; idx++ )
    {
        entryList[ idx ] = JPEG_HUFF_ENTRY_FREE;
    }

    for( uint32_t lengthIdx = 0u; JPEG_HUFFMAN_LENGTH_CNT > lengthIdx; lengthIdx++ )
    {
        const uint32_t codeLength = lengthIdx + 1u;

        for( uint32_t codeIdx = 0u; table->Bits[ lengthIdx ] > codeIdx; codeIdx++ )
        {
            uint32_t entryIdx = entryCnt;

            if( symbolMax > symbolIdx )
            {
                const uint32_t symbol = table->Values[ symbolIdx ];

                if( JPEG_HUFF_AC_ENTRIES == entryCnt )
                {
                    const uint32_t size = ( symbol & JPEG_HUFF_SIZE_MASK );
                    const uint32_t run  = ( symbol >> JPEG_HUFF_RUN_SHIFT );

                    if( JPEG_HUFF_SYMBOL_EOB == symbol )
                    {
                        entryIdx = JPEG_HUFF_AC_EOB_INDEX;
                    }
                    else if( JPEG_HUFF_SYMBOL_ZRL == symbol )
                    {
                        entryIdx = JPEG_HUFF_AC_ZRL_INDEX;
                    }
                    else if( 0u < size )
                    {
                        if( JPEG_HUFF_SIZE_CNT >= size )
                        {
                            entryIdx = ( run * JPEG_HUFF_SIZE_CNT ) + size - 1u;
                        }
                        else
                        {
                            /* The size is not 1 to 10, not a symbol of an AC table */
                        }
                    }
                    else
                    {
                        /* The size 0 is only EOB and ZRL, not a symbol of an AC table */
                    }
                }
                else
                {
                    entryIdx = symbol;
                }
            }
            else
            {
                /* More symbols than the table holds */
            }

            if( ( entryCnt         >  entryIdx                   ) &&
                ( JPEG_HUFF_ENTRY_FREE  == entryList[ entryIdx ] )    )
            {
                entryList[ entryIdx ] = ( ( ( codeLength - 1u ) & JPEG_HUFF_LENGTH_MASK ) << JPEG_HUFF_LENGTH_SHIFT ) |
                                        ( code & JPEG_HUFF_CODE_MASK );
            }
            else
            {
                /* Invalid or repeated symbol, or too many symbols */
                retState = JPEG_REQUEST_ERROR;
            }

            symbolIdx++;
            code++;
        }

        /* The codes of this length must fit the length and none is allowed to be all ones */
        if( ( 1u << codeLength ) <= code )
        {
            retState = JPEG_REQUEST_ERROR;
        }
        else
        {
            /* The codes fit */
        }

        code <<= 1u;
    }

    if( 0u == symbolIdx )
    {
        retState = JPEG_REQUEST_ERROR;
    }
    else
    {
        /* The table has symbols */
    }

    if( symbolMax < symbolIdx )
    {
        retState = JPEG_REQUEST_ERROR;
    }
    else
    {
        /* The count of the symbols fits the table */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        const uint32_t entryWords = entryCnt / JPEG_HUFF_PER_WORD;
        uint32_t       wordIdx    = 0u;

        for( wordIdx = 0u; entryWords > wordIdx; wordIdx++ )
        {
            uint32_t lowEntry  = entryList[ JPEG_HUFF_PER_WORD * wordIdx ];
            uint32_t highEntry = entryList[ ( JPEG_HUFF_PER_WORD * wordIdx ) + 1u ];

            if( JPEG_HUFF_ENTRY_FREE == lowEntry )
            {
                lowEntry = JPEG_HUFF_ENTRY_EMPTY;
            }
            else
            {
                /* The symbol is in the table */
            }

            if( JPEG_HUFF_ENTRY_FREE == highEntry )
            {
                highEntry = JPEG_HUFF_ENTRY_EMPTY;
            }
            else
            {
                /* The symbol is in the table */
            }

            wordList[ wordIdx ] = lowEntry | ( highEntry << JPEG_HUFF_ENTRY_SHIFT );
        }

        if( JPEG_HUFF_AC_ENTRIES == entryCnt )
        {
            const uint32_t tailList[ JPEG_HUFF_AC_TAIL_CNT ] = JPEG_HUFF_AC_TAIL_WORDS;

            for( uint32_t tailIdx = 0u; JPEG_HUFF_AC_TAIL_CNT > tailIdx; tailIdx++ )
            {
                wordList[ wordIdx ] = tailList[ tailIdx ];
                wordIdx++;
            }
        }
        else
        {
            wordList[ wordIdx ] = JPEG_HUFF_WORD_EMPTY;
            wordIdx++;
            wordList[ wordIdx ] = JPEG_HUFF_WORD_EMPTY;
            wordIdx++;
        }

        *wordCnt = wordIdx;
    }
    else
    {
        *wordCnt = 0u;
    }

    return ( retState );
}


/**
 * \brief Builds the image of the DHT memory: the code counts and the symbols of the four Huffman tables in the order
 *        DC 0, AC 0, DC 1, AC 1 (the order of the DHT segments the header generator writes), as 103 words. A table
 *        which is not set is replaced by the sample table.
 *
 * \param wordList [out]: The 103 words.
 */
static void Jpeg_Build_DhtWords( uint32_t * const wordList )
{
    jpeg_Data_t image[ JPEG_DHT_IMAGE_SIZE ] = { 0u };

    for( uint32_t tableIdx = 0u; JPEG_HUFFMAN_CNT > tableIdx; tableIdx++ )
    {
        const jpeg_HuffmanTable_t * table  = &jpeg_StdHuffmanTable[ tableIdx ];
        const uint32_t              offset = jpeg_DhtOffsetLut[ tableIdx ];

        if( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.HuffmanTableSet[ tableIdx ] )
        {
            table = &jpeg_RuntimeData.HuffmanTable[ tableIdx ];
        }
        else
        {
            /* The sample table is used */
        }

        for( uint32_t lengthIdx = 0u; JPEG_HUFFMAN_LENGTH_CNT > lengthIdx; lengthIdx++ )
        {
            image[ offset + lengthIdx ] = table->Bits[ lengthIdx ];
        }

        for( uint32_t valueIdx = 0u; jpeg_DhtValueCntLut[ tableIdx ] > valueIdx; valueIdx++ )
        {
            image[ offset + JPEG_HUFFMAN_LENGTH_CNT + valueIdx ] = table->Values[ valueIdx ];
        }
    }

    for( uint32_t wordIdx = 0u; JPEG_REG_DHT_WORD_CNT > wordIdx; wordIdx++ )
    {
        uint32_t wordValue = JPEG_BITS_CLEAR;

        for( uint32_t byteIdx = 0u; JPEG_WORD_SIZE > byteIdx; byteIdx++ )
        {
            const uint32_t imageByte = image[ ( wordIdx * JPEG_WORD_SIZE ) + byteIdx ];

            wordValue |= ( imageByte << ( JPEG_BYTE_BITS * byteIdx ) );
        }

        wordList[ wordIdx ] = wordValue;
    }
}


/**
 * \brief Builds the register image of an encoder job: the mode, the header, the color space, the size, the count of
 *        the MCUs and the components (sampling factors, blocks, quantization and Huffman tables).
 *
 * \param imageConfig [in]:  Image (the quality is not used here).
 * \param imageInfo   [in]:  Parameters of the image calculated by \ref Jpeg_Calc_ImageInfo.
 * \param regConfig   [out]: Register image to fill.
 */
static void Jpeg_Build_RegConfig( const jpeg_ImageConfig_t * const imageConfig,
                                  const jpeg_ImageInfo_t * const imageInfo,
                                  jpeg_RegImageConfig_t * const regConfig )
{
    const jpeg_RegImageConfig_t emptyConfig    = { 0u };
    const jpeg_Count_t          componentCnt   = jpeg_ComponentCntLut[ imageConfig->ColorSpace ];
    jpeg_Subsampling_t          usedSubsampling = JPEG_SUBSAMPLING_444;

    *regConfig = emptyConfig;

    if( JPEG_COLOR_YCBCR == imageConfig->ColorSpace )
    {
        usedSubsampling = imageConfig->Subsampling;
    }
    else
    {
        /* The other color spaces have one block per component */
    }

    if( JPEG_HEADER_ON == jpeg_RuntimeData.HeaderMode )
    {
        regConfig->Header = 1u;
    }
    else
    {
        regConfig->Header = 0u;
    }

    regConfig->Decode     = 0u;
    regConfig->ColorSpace = (uint32_t)imageConfig->ColorSpace;
    regConfig->Components = componentCnt - 1u;
    regConfig->Scan       = componentCnt - 1u;
    regConfig->Height     = imageInfo->Height;
    regConfig->Width      = imageInfo->Width;
    regConfig->McuCnt     = imageInfo->McuCount - 1u;

    for( uint32_t idx = 0u; componentCnt > idx; idx++ )
    {
        jpeg_RegComponent_t * const component = &regConfig->Component[ idx ];

        component->QuantTable = jpeg_QuantSelectLut[ imageConfig->ColorSpace ][ idx ];
        component->HuffmanAc  = jpeg_HuffSelectLut[ imageConfig->ColorSpace ][ idx ];
        component->HuffmanDc  = jpeg_HuffSelectLut[ imageConfig->ColorSpace ][ idx ];

        if( 0u == idx )
        {
            component->HorizontalFactor = jpeg_YFactorHLut[ usedSubsampling ];
            component->VerticalFactor   = jpeg_YFactorVLut[ usedSubsampling ];
            component->BlockCnt         = ( jpeg_YFactorHLut[ usedSubsampling ] *
                                            jpeg_YFactorVLut[ usedSubsampling ] ) - 1u;
        }
        else
        {
            component->HorizontalFactor = 1u;
            component->VerticalFactor   = 1u;
            component->BlockCnt         = 0u;
        }
    }
}


/**
 * \brief Checks that the tables the color space needs are available: the quantization tables 0 to N - 1 (N is 1, 2, 3
 *        or 4 for grayscale, YCbCr, RGB or CMYK) and the Huffman tables DC 0 and AC 0 (YCbCr also DC 1 and AC 1).
 *
 * \param colorSpace [in]: Color space.
 *
 * \return \ref JPEG_FUNCTION_ACTIVE if all the tables are available. Otherwise \ref JPEG_FUNCTION_INACTIVE.
 */
static jpeg_FunctionState_t Jpeg_Is_TableReady( jpeg_ColorSpace_t colorSpace )
{
    jpeg_FunctionState_t retState  = JPEG_FUNCTION_ACTIVE;
    const uint32_t       quantCnt  = (uint32_t)colorSpace + JPEG_COLOR_TABLE_OFFSET;

    for( uint32_t idx = 0u; quantCnt > idx; idx++ )
    {
        if( JPEG_FUNCTION_ACTIVE != jpeg_RuntimeData.QuantTableSet[ idx ] )
        {
            retState = JPEG_FUNCTION_INACTIVE;
        }
        else
        {
            /* The quantization table is available */
        }
    }

    if( JPEG_FUNCTION_ACTIVE != jpeg_RuntimeData.HuffmanTableSet[ JPEG_HUFFMAN_DC_0 ] )
    {
        retState = JPEG_FUNCTION_INACTIVE;
    }
    else
    {
        /* The DC table 0 is available */
    }

    if( JPEG_FUNCTION_ACTIVE != jpeg_RuntimeData.HuffmanTableSet[ JPEG_HUFFMAN_AC_0 ] )
    {
        retState = JPEG_FUNCTION_INACTIVE;
    }
    else
    {
        /* The AC table 0 is available */
    }

    if( JPEG_COLOR_YCBCR == colorSpace )
    {
        if( JPEG_FUNCTION_ACTIVE != jpeg_RuntimeData.HuffmanTableSet[ JPEG_HUFFMAN_DC_1 ] )
        {
            retState = JPEG_FUNCTION_INACTIVE;
        }
        else
        {
            /* The DC table 1 is available */
        }

        if( JPEG_FUNCTION_ACTIVE != jpeg_RuntimeData.HuffmanTableSet[ JPEG_HUFFMAN_AC_1 ] )
        {
            retState = JPEG_FUNCTION_INACTIVE;
        }
        else
        {
            /* The AC table 1 is available */
        }
    }
    else
    {
        /* The other color spaces use the tables 0 only */
    }

    return ( retState );
}


/**
 * \brief Writes the quantization tables of the color space (scaled by the quality), the Huffman encoder memories and
 *        the DHT memory.
 *
 * \param colorSpace [in]: Color space.
 * \param quality    [in]: Quality factor of the job.
 *
 * \return Returns \ref JPEG_REQUEST_OK if all tables were converted. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Load_Tables( jpeg_ColorSpace_t colorSpace, jpeg_Quality_t quality )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_OK;
    const jpeg_Count_t  scale    = Jpeg_Calc_Scale( quality );
    const uint32_t      quantCnt = (uint32_t)colorSpace + JPEG_COLOR_TABLE_OFFSET;
    uint32_t            wordList[ JPEG_HUFF_AC_WORDS + JPEG_HUFF_AC_TAIL_CNT ];
    uint32_t            dhtList[ JPEG_REG_DHT_WORD_CNT ];

    for( uint32_t idx = 0u; quantCnt > idx; idx++ )
    {
        Jpeg_Build_QuantWords( &jpeg_RuntimeData.QuantTable[ idx ], scale, wordList );
        Jpeg_Reg_Set_QuantWords( (jpeg_QuantId_t)idx, wordList );
    }

    for( uint32_t idx = 0u; JPEG_HUFFMAN_CNT > idx; idx++ )
    {
        const jpeg_HuffmanTable_t * table   = &jpeg_StdHuffmanTable[ idx ];
        uint32_t                    wordCnt = 0u;

        if( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.HuffmanTableSet[ idx ] )
        {
            table = &jpeg_RuntimeData.HuffmanTable[ idx ];
        }
        else
        {
            /* The sample table is used */
        }

        const jpeg_RequestState_t buildState = Jpeg_Build_HuffEnc( (jpeg_HuffmanId_t)idx, table, wordList, &wordCnt );

        if( JPEG_REQUEST_OK == buildState )
        {
            Jpeg_Reg_Set_HuffEncWords( (jpeg_HuffmanId_t)idx, wordList, wordCnt );
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }
    }

    Jpeg_Build_DhtWords( dhtList );
    Jpeg_Reg_Set_DhtWords( dhtList );

    return ( retState );
}


/**
 * \brief Starts an encoder job: stops and resets the codec, writes the tables and the image registers, enables the
 *        events and starts the codec.
 *
 * \param imageConfig [in]: Valid image, the module is initialized and idle and the tables are available.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the codec was started. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Start_Job( const jpeg_ImageConfig_t * const imageConfig )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;
    jpeg_ImageConfig_t  usedConfig = *imageConfig;
    jpeg_ImageInfo_t    imageInfo;

    if( JPEG_QUALITY_MODULE == usedConfig.Quality )
    {
        usedConfig.Quality = jpeg_RuntimeData.Quality;
    }
    else
    {
        /* The image has its own quality */
    }

    const jpeg_RequestState_t      calcState = Jpeg_Calc_ImageInfo( &usedConfig, &imageInfo );
    const jpeg_FunctionState_t     ready     = Jpeg_Is_TableReady( usedConfig.ColorSpace );

    if( ( JPEG_REQUEST_OK     == calcState ) &&
        ( JPEG_FUNCTION_ACTIVE == ready    )    )
    {
        jpeg_RegImageConfig_t regConfig;

        retState = Jpeg_Reg_Reset_Codec();

        if( JPEG_REQUEST_OK == retState )
        {
            retState = Jpeg_Load_Tables( usedConfig.ColorSpace, usedConfig.Quality );
        }
        else
        {
            /* The codec did not stop */
        }

        if( JPEG_REQUEST_OK == retState )
        {
            Jpeg_Build_RegConfig( &usedConfig, &imageInfo, &regConfig );
            Jpeg_Reg_Set_ImageConfig( &regConfig );

            Jpeg_Reset_Stream();

            jpeg_RuntimeData.State          = JPEG_STATE_ENCODING;
            jpeg_RuntimeData.HeaderParsed   = JPEG_FUNCTION_ACTIVE;
            jpeg_RuntimeData.ImageInfo      = imageInfo;
            jpeg_RuntimeData.ImageInfoValid = JPEG_FUNCTION_ACTIVE;

            if( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.CpuServed )
            {
                Jpeg_Set_RegBit( JPEG_REG_CR, JPEG_CR_DMA_MASK );
            }
            else
            {
                /* The CPU serves the FIFOs */
            }

            Jpeg_Update_Irq();
            Jpeg_Reg_Start_Codec();
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Clears the data of the streams and of the progress of a job (before the start of a job).
 */
static void Jpeg_Reset_Stream( void )
{
    jpeg_RuntimeData.InData     = JPEG_NULL_PTR;
    jpeg_RuntimeData.InSize     = 0u;
    jpeg_RuntimeData.InIdx      = 0u;
    jpeg_RuntimeData.InPending  = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.InPaused   = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.OutData    = JPEG_NULL_PTR;
    jpeg_RuntimeData.OutSize    = 0u;
    jpeg_RuntimeData.OutIdx     = 0u;
    jpeg_RuntimeData.OutPending = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.OutPaused  = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.TotalOut   = 0u;
    jpeg_RuntimeData.EocWait    = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.HeaderParsed = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.HeaderScanned = JPEG_FUNCTION_INACTIVE;

    for( jpeg_DmaStream_t stream = JPEG_DMA_STREAM_INPUT; JPEG_DMA_STREAM_CNT > stream; stream++ )
    {
        jpeg_DmaArmedSize[ stream ] = 0u;
    }
}


/**
 * \brief Sets the interrupt enable bits of the codec for the state of the job: the end of the conversion, the parsed
 *        header (decoder), and, when the CPU serves the FIFOs, the not full input FIFO (while an input buffer waits)
 *        and the not empty output FIFO (while an output buffer waits). No interrupt in the poll mode.
 */
static void Jpeg_Update_Irq( void )
{
    uint32_t irqBits = JPEG_BITS_CLEAR;

    if( ( JPEG_XFER_MODE_POLL != jpeg_RuntimeData.XferMode ) &&
        ( JPEG_STATE_IDLE     != jpeg_RuntimeData.State    )    )
    {
        if( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.EocWait )
        {
            irqBits |= JPEG_CR_EOCIE;
        }
        else
        {
            /* The end was seen, the output FIFO has to be emptied first */
        }

        if( ( JPEG_STATE_DECODING        == jpeg_RuntimeData.State        ) &&
            ( JPEG_FUNCTION_INACTIVE     == jpeg_RuntimeData.HeaderParsed )    )
        {
            irqBits |= JPEG_CR_HPDIE;
        }
        else
        {
            /* No header is expected */
        }

        if( ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.CpuServed ) &&
            ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.InPending ) &&
            ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.InPaused  )    )
        {
            irqBits |= JPEG_CR_IFNFIE;
        }
        else
        {
            /* The input FIFO is not served by the interrupt */
        }

        if( ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.CpuServed  ) &&
            ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.OutPending ) &&
            ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.OutPaused  )    )
        {
            irqBits |= JPEG_CR_OFNEIE;
        }
        else
        {
            /* The output FIFO is not served by the interrupt */
        }
    }
    else
    {
        /* Poll mode or no job: no interrupt */
    }

    Jpeg_Set_RegVal( JPEG_REG_CR, JPEG_CR_IRQ_MASK, irqBits );
}


/**
 * \brief Enables the core, leaves the codec idle, configures the interrupt (interrupt and DMA mode) and the Gpdma
 *        channels (DMA mode) and verifies the result by a read-back.
 *
 * \param irqPrio [in]: Priority of the JPEG interrupt.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the codec is enabled and idle. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Init_Hardware( jpeg_IrqPrio_t irqPrio )
{
    jpeg_RequestState_t retState = Jpeg_Reg_Enable_Core();

    if( JPEG_REQUEST_OK == retState )
    {
        retState = Jpeg_Reg_Reset_Codec();
    }
    else
    {
        /* The core did not start */
    }

    if( ( JPEG_REQUEST_OK     == retState                   ) &&
        ( JPEG_XFER_MODE_POLL != jpeg_RuntimeData.XferMode )    )
    {
        nvic_RequestState_t nvicState = Nvic_Set_PeriphIrq_Handler( NVIC_PERIPH_IRQ_JPEG, Jpeg_IsrHandler );

        if( NVIC_REQUEST_OK == nvicState )
        {
            nvicState = Nvic_Set_PeriphIrq_Prio( NVIC_PERIPH_IRQ_JPEG, irqPrio );
        }
        else
        {
            /* Handler registration already failed */
        }

        if( NVIC_REQUEST_OK == nvicState )
        {
            nvicState = Nvic_Set_PeriphIrq_Active( NVIC_PERIPH_IRQ_JPEG );
        }
        else
        {
            /* Priority configuration already failed */
        }

        if( NVIC_REQUEST_OK == nvicState )
        {
            retState = JPEG_REQUEST_OK;
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }
    }
    else
    {
        /* POLL mode does not use the interrupt or the codec did not stop */
    }

    if( ( JPEG_REQUEST_OK    == retState                   ) &&
        ( JPEG_XFER_MODE_DMA == jpeg_RuntimeData.XferMode )    )
    {
        retState = Jpeg_Dma_Init();
    }
    else
    {
        /* The Gpdma channels are not used or a previous step failed */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        /* Read-back: core enabled, no request enabled, codec stopped, no flag of the conversion */
        const uint32_t crMask     = ( JPEG_CR_JCEN | JPEG_CR_IRQ_MASK | JPEG_CR_DMA_MASK );
        const uint32_t crValue    = Jpeg_Get_RegVal( JPEG_REG_CR, crMask );
        const uint32_t startValue = Jpeg_Get_RegVal( JPEG_REG_CONFR0, JPEG_CONFR0_START );
        const uint32_t flagValue  = Jpeg_Get_RegVal( JPEG_REG_SR, ( JPEG_SR_EOCF | JPEG_SR_HPDF | JPEG_SR_COF ) );

        if( ( JPEG_CR_JCEN    == crValue    ) &&
            ( JPEG_BITS_CLEAR == startValue ) &&
            ( JPEG_BITS_CLEAR == flagValue  )    )
        {
            retState = JPEG_REQUEST_OK;
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }
    }
    else
    {
        /* A previous step failed */
    }

    return ( retState );
}


/**
 * \brief Copies the sample tables of the specification to the tables of the module and marks them as available.
 */
static void Jpeg_Load_StandardTables( void )
{
    for( uint32_t idx = 0u; JPEG_QUANT_TABLE_CNT > idx; idx++ )
    {
        jpeg_RuntimeData.QuantTable[ idx ]    = jpeg_StdQuantTable[ jpeg_StdQuantLut[ idx ] ];
        jpeg_RuntimeData.QuantTableSet[ idx ] = JPEG_FUNCTION_ACTIVE;
    }

    for( uint32_t idx = 0u; JPEG_HUFFMAN_CNT > idx; idx++ )
    {
        jpeg_RuntimeData.HuffmanTable[ idx ]    = jpeg_StdHuffmanTable[ idx ];
        jpeg_RuntimeData.HuffmanTableSet[ idx ] = JPEG_FUNCTION_ACTIVE;
    }
}


/**
 * \brief Takes an input buffer: stores it, arms the Gpdma channel (DMA mode) and enables the interrupt.
 *
 * \param buffer [in]: Input data (not NULL).
 * \param size   [in]: Count of the bytes (at least 1).
 *
 * \return Returns \ref JPEG_REQUEST_OK if the buffer is served. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Take_Input( const jpeg_Data_t * const buffer, jpeg_Size_t size )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_OK;
    const uint32_t      address  = (uint32_t)(uintptr_t)buffer;

    if( ( JPEG_STATE_DECODING     == jpeg_RuntimeData.State         ) &&
        ( JPEG_FUNCTION_INACTIVE  == jpeg_RuntimeData.HeaderScanned )    )
    {
        const jpeg_ScanResult_t scanResult = Jpeg_Scan_Header( buffer, size );

        jpeg_RuntimeData.HeaderScanned = JPEG_FUNCTION_ACTIVE;

        if( JPEG_SCAN_UNSUPPORTED == scanResult )
        {
            retState = JPEG_REQUEST_ERROR;

            Jpeg_Fail_Job( JPEG_ERROR_UNSUPPORTED );
        }
        else if( JPEG_SCAN_CORRUPT == scanResult )
        {
            retState = JPEG_REQUEST_ERROR;

            Jpeg_Fail_Job( JPEG_ERROR_STREAM );
        }
        else
        {
            /* The stream may be decoded */
        }
    }
    else
    {
        /* Not the first buffer of a decoder job */
    }

    if( ( JPEG_REQUEST_OK        == retState                  ) &&
        ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.CpuServed )    )
    {
        if( ( JPEG_DMA_SIZE_MAX >= size                      ) &&
            ( 0u                == ( size % JPEG_DMA_ALIGN ) ) &&
            ( 0u                == ( address % JPEG_DMA_ALIGN ) )    )
        {
            retState = Jpeg_Cache_Maintain( JPEG_CACHE_CLEAN, address, size );
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }
    }
    else
    {
        /* The CPU moves the bytes, any address and size are valid */
    }

    if( ( JPEG_REQUEST_OK    == retState                  ) &&
        ( JPEG_STATE_IDLE    != jpeg_RuntimeData.State    )    )
    {
        jpeg_RuntimeData.InData    = buffer;
        jpeg_RuntimeData.InSize    = size;
        jpeg_RuntimeData.InIdx     = 0u;
        jpeg_RuntimeData.InPending = JPEG_FUNCTION_ACTIVE;

        if( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.CpuServed )
        {
            jpeg_DmaArmedSize[ JPEG_DMA_STREAM_INPUT ] = size;

            retState = Jpeg_Dma_Arm( JPEG_DMA_STREAM_INPUT, address, size );

            if( JPEG_REQUEST_OK != retState )
            {
                jpeg_RuntimeData.InPending = JPEG_FUNCTION_INACTIVE;
            }
            else if( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InPaused )
            {
                /* The stream was paused before the buffer: the new transfer waits for the resume */
                const gpdma_RequestState_t pauseState = Gpdma_Set_PauseActive( jpeg_RuntimeData.DmaPeriphId,
                                                                               jpeg_RuntimeData.DmaInputChannelId );

                if( GPDMA_REQUEST_OK != pauseState )
                {
                    retState = JPEG_REQUEST_ERROR;
                }
                else
                {
                    /* The channel is suspended */
                }
            }
            else
            {
                /* The channel is armed */
            }
        }
        else
        {
            /* The CPU serves the buffer */
        }

        Jpeg_Update_Irq();
    }
    else
    {
        /* The buffer is not valid for the DMA mode */
    }

    return ( retState );
}


/**
 * \brief Takes an output buffer: stores it, arms the Gpdma channel (DMA mode) and enables the interrupt.
 *
 * \param buffer [out]: Output data (not NULL).
 * \param size   [in]:  Count of the bytes (a multiple of 4, at least 4).
 *
 * \return Returns \ref JPEG_REQUEST_OK if the buffer is served. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Take_Output( jpeg_Data_t * const buffer, jpeg_Size_t size )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_OK;
    const uint32_t      address  = (uint32_t)(uintptr_t)buffer;

    if( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.CpuServed )
    {
        if( ( JPEG_DMA_SIZE_MAX >= size                         ) &&
            ( 0u                == ( address % JPEG_DMA_ALIGN ) )    )
        {
            retState = Jpeg_Cache_Maintain( JPEG_CACHE_FLUSH, address, size );
        }
        else
        {
            retState = JPEG_REQUEST_ERROR;
        }
    }
    else
    {
        /* The CPU moves the bytes, any address is valid */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        jpeg_RuntimeData.OutData    = buffer;
        jpeg_RuntimeData.OutSize    = size;
        jpeg_RuntimeData.OutIdx     = 0u;
        jpeg_RuntimeData.OutPending = JPEG_FUNCTION_ACTIVE;
        jpeg_RuntimeData.EocWait    = JPEG_FUNCTION_INACTIVE;

        if( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.CpuServed )
        {
            jpeg_DmaArmedSize[ JPEG_DMA_STREAM_OUTPUT ] = size;

            retState = Jpeg_Dma_Arm( JPEG_DMA_STREAM_OUTPUT, address, size );

            if( JPEG_REQUEST_OK != retState )
            {
                jpeg_RuntimeData.OutPending = JPEG_FUNCTION_INACTIVE;
            }
            else if( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.OutPaused )
            {
                /* The stream was paused before the buffer: the new transfer waits for the resume */
                const gpdma_RequestState_t pauseState = Gpdma_Set_PauseActive( jpeg_RuntimeData.DmaPeriphId,
                                                                               jpeg_RuntimeData.DmaOutputChannelId );

                if( GPDMA_REQUEST_OK != pauseState )
                {
                    retState = JPEG_REQUEST_ERROR;
                }
                else
                {
                    /* The channel is suspended */
                }
            }
            else
            {
                /* The channel is armed */
            }
        }
        else
        {
            /* The CPU serves the buffer */
        }

        Jpeg_Update_Irq();
    }
    else
    {
        /* The buffer is not valid for the DMA mode */
    }

    return ( retState );
}


/**
 * \brief Takes the next word (up to four bytes, the first one in the lowest byte) from the input buffer. The last
 *        word of a buffer whose size is not a multiple of 4 is padded with zeros.
 *
 * \return The word for the input FIFO.
 */
static uint32_t Jpeg_Get_InputWord( void )
{
    uint32_t          dataWord  = JPEG_BITS_CLEAR;
    const jpeg_Size_t remaining = jpeg_RuntimeData.InSize - jpeg_RuntimeData.InIdx;
    jpeg_Size_t       byteCnt   = JPEG_WORD_SIZE;

    if( remaining < byteCnt )
    {
        byteCnt = remaining;
    }
    else
    {
        /* A whole word is left */
    }

    for( jpeg_Size_t byteIdx = 0u; byteCnt > byteIdx; byteIdx++ )
    {
        const uint32_t dataByte = jpeg_RuntimeData.InData[ jpeg_RuntimeData.InIdx + byteIdx ];

        dataWord |= ( dataByte << ( JPEG_BYTE_BITS * byteIdx ) );
    }

    jpeg_RuntimeData.InIdx += byteCnt;

    return ( dataWord );
}


/**
 * \brief Stores a word of the output FIFO (four bytes, the first one from the lowest byte) in the output buffer.
 *
 * \param dataWord [in]: Word read from the output FIFO.
 */
static void Jpeg_Put_OutputWord( uint32_t dataWord )
{
    for( jpeg_Size_t byteIdx = 0u; JPEG_WORD_SIZE > byteIdx; byteIdx++ )
    {
        const uint32_t dataByte = ( ( dataWord >> ( JPEG_BYTE_BITS * byteIdx ) ) & JPEG_BYTE_MASK );

        jpeg_RuntimeData.OutData[ jpeg_RuntimeData.OutIdx + byteIdx ] = (jpeg_Data_t)dataByte;
    }

    jpeg_RuntimeData.OutIdx   += JPEG_WORD_SIZE;
    jpeg_RuntimeData.TotalOut += JPEG_WORD_SIZE;
}


/**
 * \brief Serves the FIFOs of the codec by the CPU (poll and interrupt mode): moves the input words, the output words
 *        and ends the job.
 */
static void Jpeg_Service( void )
{
    Jpeg_Check_Header();
    Jpeg_Feed();
    Jpeg_Drain();
    Jpeg_Check_End();
}


/**
 * \brief Serves the events of the DMA mode: the Gpdma channels move the data, the interrupt reports the parsed
 *        header and the end of the conversion.
 */
static void Jpeg_Service_Dma( void )
{
    Jpeg_Check_Header();

    const uint32_t eocFlag = Jpeg_Get_RegBit( JPEG_REG_SR, JPEG_SR_EOCF );

    if( ( JPEG_STATE_IDLE != jpeg_RuntimeData.State ) &&
        ( JPEG_BITS_CLEAR != eocFlag                )    )
    {
        Jpeg_Dma_End();
    }
    else
    {
        /* The conversion goes on */
    }
}


/**
 * \brief Moves the words of the input buffer into the input FIFO while it is not full (at most the size of the FIFO
 *        per call). When the buffer is consumed the input needed callback is called.
 */
static void Jpeg_Feed( void )
{
    if( ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.InPending ) &&
        ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.InPaused  )    )
    {
        jpeg_FunctionState_t moreData = JPEG_FUNCTION_ACTIVE;

        for( uint32_t idx = 0u;
             ( JPEG_FIFO_WORDS > idx ) &&
             ( JPEG_FUNCTION_ACTIVE == moreData );
             idx++ )
        {
            const uint32_t notFull = Jpeg_Get_RegBit( JPEG_REG_SR, JPEG_SR_IFNFF );

            if( JPEG_BITS_CLEAR != notFull )
            {
                Jpeg_Reg_Write_Word( Jpeg_Get_InputWord() );

                if( jpeg_RuntimeData.InSize <= jpeg_RuntimeData.InIdx )
                {
                    moreData = JPEG_FUNCTION_INACTIVE;
                }
                else
                {
                    /* More words in the buffer */
                }
            }
            else
            {
                /* The input FIFO is full */
                moreData = JPEG_FUNCTION_INACTIVE;
            }
        }

        if( jpeg_RuntimeData.InSize <= jpeg_RuntimeData.InIdx )
        {
            jpeg_RuntimeData.InPending = JPEG_FUNCTION_INACTIVE;

            Jpeg_Update_Irq();
            Jpeg_Notify_InputNeeded();
        }
        else
        {
            /* The buffer is not consumed yet */
        }
    }
    else
    {
        /* No input buffer is waiting or the stream is paused */
    }
}


/**
 * \brief Moves the words of the output FIFO into the output buffer while it holds data (at most the size of the FIFO
 *        per call). When the buffer is full the output ready callback is called.
 */
static void Jpeg_Drain( void )
{
    if( ( JPEG_FUNCTION_ACTIVE   == jpeg_RuntimeData.OutPending ) &&
        ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.OutPaused  )    )
    {
        jpeg_FunctionState_t moreData   = JPEG_FUNCTION_ACTIVE;
        jpeg_FunctionState_t bufferFull = JPEG_FUNCTION_INACTIVE;

        for( uint32_t idx = 0u;
             ( JPEG_FIFO_WORDS > idx ) &&
             ( JPEG_FUNCTION_ACTIVE == moreData );
             idx++ )
        {
            const uint32_t notEmpty = Jpeg_Get_RegBit( JPEG_REG_SR, JPEG_SR_OFNEF );

            if( JPEG_BITS_CLEAR != notEmpty )
            {
                Jpeg_Put_OutputWord( Jpeg_Reg_Read_Word() );

                if( jpeg_RuntimeData.OutSize <= jpeg_RuntimeData.OutIdx )
                {
                    moreData   = JPEG_FUNCTION_INACTIVE;
                    bufferFull = JPEG_FUNCTION_ACTIVE;
                }
                else
                {
                    /* More room in the buffer */
                }
            }
            else
            {
                /* The output FIFO is empty */
                moreData = JPEG_FUNCTION_INACTIVE;
            }
        }

        if( JPEG_FUNCTION_ACTIVE == bufferFull )
        {
            jpeg_RuntimeData.OutPending = JPEG_FUNCTION_INACTIVE;

            Jpeg_Update_Irq();
            Jpeg_Notify_OutputReady( jpeg_RuntimeData.OutSize );
        }
        else
        {
            /* The buffer is not full yet */
        }
    }
    else
    {
        /* No output buffer is waiting or the stream is paused */
    }
}


/**
 * \brief Ends the job when the codec reported the end of the conversion and the output FIFO is empty. While the FIFO
 *        holds data and no output buffer is served, the end interrupt is disabled until the next output buffer.
 */
static void Jpeg_Check_End( void )
{
    const uint32_t eocFlag  = Jpeg_Get_RegBit( JPEG_REG_SR, JPEG_SR_EOCF );
    const uint32_t notEmpty = Jpeg_Get_RegBit( JPEG_REG_SR, JPEG_SR_OFNEF );

    if( ( JPEG_STATE_IDLE != jpeg_RuntimeData.State ) &&
        ( JPEG_BITS_CLEAR != eocFlag                )    )
    {
        if( JPEG_BITS_CLEAR == notEmpty )
        {
            Jpeg_Complete_Job();
        }
        else
        {
            jpeg_RuntimeData.EocWait = JPEG_FUNCTION_ACTIVE;

            Jpeg_Update_Irq();
        }
    }
    else
    {
        /* The job runs on or ended already */
    }
}


/**
 * \brief Returns the module to the idle state after a job: no buffer is served, no stream is paused and the FIFOs
 *        are served the way the mode says. The total count of the output bytes and the image parameters are kept.
 */
static void Jpeg_Enter_Idle( void )
{
    jpeg_RuntimeData.State      = JPEG_STATE_IDLE;
    jpeg_RuntimeData.InPending  = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.InPaused   = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.OutPending = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.OutPaused  = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.EocWait    = JPEG_FUNCTION_INACTIVE;

    if( JPEG_XFER_MODE_DMA == jpeg_RuntimeData.XferMode )
    {
        jpeg_RuntimeData.CpuServed = JPEG_FUNCTION_INACTIVE;
    }
    else
    {
        jpeg_RuntimeData.CpuServed = JPEG_FUNCTION_ACTIVE;
    }
}


/**
 * \brief Ends a job normally: stops the codec, reports the last output buffer (when it holds data) and the end of the
 *        conversion. A decoder job which ended before its header was parsed ends with \ref JPEG_ERROR_STREAM.
 */
static void Jpeg_Complete_Job( void )
{
    jpeg_Size_t                partial       = 0u;
    const jpeg_Size_t          totalLength   = jpeg_RuntimeData.TotalOut;
    jpeg_FunctionState_t       headerMissing = JPEG_FUNCTION_INACTIVE;

    if( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.OutPending )
    {
        partial = jpeg_RuntimeData.OutIdx;
    }
    else
    {
        /* No partial output buffer */
    }

    if( ( JPEG_STATE_DECODING    == jpeg_RuntimeData.State        ) &&
        ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.HeaderParsed )    )
    {
        headerMissing = JPEG_FUNCTION_ACTIVE;
    }
    else
    {
        /* The header is not expected or parsed */
    }

    (void)Jpeg_Reg_Reset_Codec();
    Jpeg_Enter_Idle();

    if( JPEG_FUNCTION_ACTIVE == headerMissing )
    {
        Jpeg_Notify_Error( JPEG_ERROR_STREAM );
    }
    else
    {
        if( 0u < partial )
        {
            Jpeg_Notify_OutputReady( partial );
        }
        else
        {
            /* No partial output buffer to report */
        }

        Jpeg_Notify_EndOfConversion( totalLength );
    }
}


/**
 * \brief Ends a job with an error: stops the codec and the Gpdma channels and reports the error.
 *
 * \param errorMask [in]: Errors, bit mask of \ref jpeg_Error_t.
 */
static void Jpeg_Fail_Job( jpeg_Error_t errorMask )
{
    (void)Jpeg_Abort_Job();

    Jpeg_Notify_Error( errorMask );
}


/**
 * \brief Aborts the job: stops the Gpdma channels (DMA mode), resets the codec (stop, no request, FIFOs flushed,
 *        flags cleared) and returns the module to the idle state.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the codec and the channels stopped.
 *         Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Abort_Job( void )
{
    jpeg_RequestState_t dmaState = JPEG_REQUEST_OK;

    if( JPEG_XFER_MODE_DMA == jpeg_RuntimeData.XferMode )
    {
        dmaState = Jpeg_Dma_Stop();
    }
    else
    {
        /* No Gpdma channel is used */
    }

    const jpeg_RequestState_t codecState = Jpeg_Reg_Reset_Codec();

    Jpeg_Enter_Idle();

    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

    if( ( JPEG_REQUEST_OK == dmaState   ) &&
        ( JPEG_REQUEST_OK == codecState )    )
    {
        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Calls the input needed callback if registered.
 */
static void Jpeg_Notify_InputNeeded( void )
{
    if( JPEG_NULL_PTR != jpeg_RuntimeData.InputNeededCallback )
    {
        jpeg_RuntimeData.InputNeededCallback();
    }
    else
    {
        /* No callback registered */
    }
}


/**
 * \brief Calls the output ready callback if registered.
 *
 * \param length [in]: Count of the valid bytes of the output buffer.
 */
static void Jpeg_Notify_OutputReady( jpeg_Size_t length )
{
    if( JPEG_NULL_PTR != jpeg_RuntimeData.OutputReadyCallback )
    {
        jpeg_RuntimeData.OutputReadyCallback( length );
    }
    else
    {
        /* No callback registered */
    }
}


/**
 * \brief Calls the end of conversion callback if registered.
 *
 * \param totalLength [in]: Count of the bytes the job produced.
 */
static void Jpeg_Notify_EndOfConversion( jpeg_Size_t totalLength )
{
    if( JPEG_NULL_PTR != jpeg_RuntimeData.EndOfConversionCallback )
    {
        jpeg_RuntimeData.EndOfConversionCallback( totalLength );
    }
    else
    {
        /* No callback registered */
    }
}


/**
 * \brief Calls the error callback if registered.
 *
 * \param errorMask [in]: Errors, bit mask of \ref jpeg_Error_t.
 */
static void Jpeg_Notify_Error( jpeg_Error_t errorMask )
{
    if( JPEG_NULL_PTR != jpeg_RuntimeData.ErrorCallback )
    {
        jpeg_RuntimeData.ErrorCallback( errorMask );
    }
    else
    {
        /* No callback registered */
    }
}


/**
 * \brief Maintains the data cache around a transfer of the Gpdma (DMA mode). Nothing is done when the data cache is
 *        not enabled; with the enabled data cache the range has to be aligned to the cache line (16 bytes).
 *
 * \param operation [in]: Clean (input), flush (output before the transfer) or invalidate (output after it).
 * \param address   [in]: Address of the buffer.
 * \param size      [in]: Size of the buffer in bytes.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the maintenance is done or not needed. Otherwise returns
 *         \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Cache_Maintain( jpeg_CacheOp_t operation, uint32_t address, jpeg_Size_t size )
{
    jpeg_RequestState_t        retState   = JPEG_REQUEST_OK;
    cache_State_t              cacheState = { 0u };
    const cache_RequestState_t stateReq   = Cache_Get_State( CACHE_DATA, &cacheState );

    if( ( CACHE_REQUEST_OK == stateReq                ) &&
        ( CACHE_FLAG_ACTIVE == cacheState.Enabled     )    )
    {
        cache_RequestState_t cacheReq = CACHE_REQUEST_ERROR;

        if( ( 0u == ( address % JPEG_CACHE_ALIGN ) ) &&
            ( 0u == ( size % JPEG_CACHE_ALIGN    ) )    )
        {
            if( JPEG_CACHE_CLEAN == operation )
            {
                cacheReq = Cache_Set_CleanRange( CACHE_DATA, address, size );
            }
            else if( JPEG_CACHE_FLUSH == operation )
            {
                cacheReq = Cache_Set_CleanInvalidateRange( CACHE_DATA, address, size );
            }
            else
            {
                cacheReq = Cache_Set_InvalidateRange( CACHE_DATA, address, size );
            }
        }
        else
        {
            /* The range is not aligned to the cache line */
        }

        if( CACHE_REQUEST_OK != cacheReq )
        {
            retState = JPEG_REQUEST_ERROR;
        }
        else
        {
            /* The cache is maintained */
        }
    }
    else
    {
        /* No data cache or the data cache is disabled: the Gpdma sees the memory */
    }

    return ( retState );
}


/**
 * \brief Initializes the two Gpdma channels of the DMA mode (the transfers are configured with the buffers) and
 *        enables their interrupts in Nvic.
 *
 * \return Returns \ref JPEG_REQUEST_OK if both channels are initialized. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Dma_Init( void )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_OK;

    for( jpeg_DmaStream_t stream = JPEG_DMA_STREAM_INPUT; JPEG_DMA_STREAM_CNT > stream; stream++ )
    {
        if( JPEG_REQUEST_OK == retState )
        {
            const gpdma_ChannelId_t channelId  = Jpeg_Dma_Get_ChannelId( stream );
            gpdma_TransferConfig_t  xferConfig = { 0u };
            gpdma_ConfigStruct_t    dmaConfig  = { 0u };

            Jpeg_Dma_Get_XferConfig( stream,
                                     (uint32_t)(uintptr_t)&jpeg_DmaDummy,
                                     JPEG_DMA_PLACEHOLDER_SIZE,
                                     &xferConfig );

            dmaConfig.PeriphId            = jpeg_RuntimeData.DmaPeriphId;
            dmaConfig.ChannelId           = channelId;
            dmaConfig.ChannelPrio         = jpeg_RuntimeData.DmaPriority;
            dmaConfig.TransferExecMode    = GPDMA_XFER_EXEC_CONTINUOUS;
            dmaConfig.TransferConfig      = &xferConfig;
            dmaConfig.TransfersCount      = JPEG_DMA_TRANSFERS_CNT;
            dmaConfig.XferListAccessMode  = GPDMA_TRANSFER_LIST_ACCESS_SINGLE;
            dmaConfig.XferList            = &jpeg_DmaXferList[ stream ];
            dmaConfig.TransferLockState   = GPDMA_TRANSFER_LIST_LOCKED;
            dmaConfig.TransferCompleteIsr = Jpeg_Dma_OutputComplete;
            dmaConfig.HalfTransferIsr     = GPDMA_NULL_PTR;
            dmaConfig.ErrorIsr            = Jpeg_Dma_Error;
            dmaConfig.ErrorMask           = (gpdma_ErrorMaskId_t)JPEG_DMA_ERROR_MASK;

            if( JPEG_DMA_STREAM_INPUT == stream )
            {
                dmaConfig.TransferCompleteIsr = Jpeg_Dma_InputComplete;
            }
            else
            {
                /* The output channel reports the full buffer */
            }

            gpdma_RequestState_t dmaState = Gpdma_Init( &dmaConfig );

            if( GPDMA_REQUEST_OK == dmaState )
            {
                jpeg_RuntimeData.DmaChannelInit[ stream ] = JPEG_FUNCTION_ACTIVE;

                dmaState = Gpdma_Set_InterruptActive( jpeg_RuntimeData.DmaPeriphId, channelId );
            }
            else
            {
                /* The channel is not initialized */
            }

            if( GPDMA_REQUEST_OK != dmaState )
            {
                retState = JPEG_REQUEST_ERROR;
            }
            else
            {
                /* The channel is ready */
            }
        }
        else
        {
            /* A previous channel failed */
        }
    }

    return ( retState );
}


/**
 * \brief Releases the Gpdma channels which were initialized by the module.
 *
 * \return Returns \ref JPEG_REQUEST_OK if every initialized channel was released (also if there is none).
 *         Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Dma_Deinit( void )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_OK;

    for( jpeg_DmaStream_t stream = JPEG_DMA_STREAM_INPUT; JPEG_DMA_STREAM_CNT > stream; stream++ )
    {
        if( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.DmaChannelInit[ stream ] )
        {
            const gpdma_RequestState_t dmaState = Gpdma_Deinit( jpeg_RuntimeData.DmaPeriphId,
                                                                Jpeg_Dma_Get_ChannelId( stream ) );

            if( GPDMA_REQUEST_OK != dmaState )
            {
                retState = JPEG_REQUEST_ERROR;
            }
            else
            {
                /* The channel is released */
            }
        }
        else
        {
            /* The channel was not initialized by the module */
        }
    }

    return ( retState );
}


/**
 * \brief Stops the Gpdma channels which were initialized by the module.
 *
 * \return Returns \ref JPEG_REQUEST_OK if every initialized channel was stopped (also if there is none).
 *         Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Dma_Stop( void )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_OK;

    for( jpeg_DmaStream_t stream = JPEG_DMA_STREAM_INPUT; JPEG_DMA_STREAM_CNT > stream; stream++ )
    {
        if( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.DmaChannelInit[ stream ] )
        {
            const gpdma_RequestState_t dmaState = Gpdma_Set_ChannelInactive( jpeg_RuntimeData.DmaPeriphId,
                                                                             Jpeg_Dma_Get_ChannelId( stream ) );

            if( GPDMA_REQUEST_OK != dmaState )
            {
                retState = JPEG_REQUEST_ERROR;
            }
            else
            {
                /* The channel is stopped */
            }
        }
        else
        {
            /* The channel was not initialized by the module */
        }
    }

    return ( retState );
}


/**
 * \brief Arms a Gpdma channel for a buffer: generates the transfer list node, loads it into the channel and enables
 *        the channel.
 *
 * \param stream     [in]: Channel of the input stream or of the output stream.
 * \param memoryAddr [in]: Address of the memory buffer.
 * \param byteCnt    [in]: Count of the bytes of the buffer.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the channel is enabled. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Dma_Arm( jpeg_DmaStream_t stream, uint32_t memoryAddr, jpeg_Size_t byteCnt )
{
    jpeg_RequestState_t       retState    = JPEG_REQUEST_ERROR;
    const gpdma_ChannelId_t   channelId   = Jpeg_Dma_Get_ChannelId( stream );
    const gpdma_PeriphId_t    periphId    = jpeg_RuntimeData.DmaPeriphId;
    const gpdma_ChannelType_t channelType = jpeg_DmaChannelTypeLut[ channelId ];
    volatile gpdma_XferList_t *xferNode   = &jpeg_DmaXferList[ stream ];
    gpdma_TransferConfig_t    xferConfig  = { 0u };
    gpdma_XferLinkReg_t       linkReg     = 0u;
    gpdma_RequestState_t      dmaState    = GPDMA_REQUEST_ERROR;

    Jpeg_Dma_Get_XferConfig( stream, memoryAddr, byteCnt, &xferConfig );

    dmaState = Gpdma_Get_XferListConfig( &xferConfig, JPEG_DMA_TRANSFERS_CNT, channelType, xferNode );

    if( GPDMA_REQUEST_OK == dmaState )
    {
        dmaState = Gpdma_Get_XferList_LinkReg( xferNode, channelType, &linkReg );
    }
    else
    {
        /* The transfer list node was not generated */
    }

    if( GPDMA_REQUEST_OK == dmaState )
    {
        dmaState = Gpdma_Set_XferListLink( periphId, channelId, linkReg );
    }
    else
    {
        /* The link register is not available */
    }

    if( GPDMA_REQUEST_OK == dmaState )
    {
        dmaState = Gpdma_Set_BlockSize( periphId, channelId, xferConfig.BlockSize );
    }
    else
    {
        /* The link register was not configured */
    }

    if( GPDMA_REQUEST_OK == dmaState )
    {
        dmaState = Gpdma_Set_SourceAddr( periphId, channelId, xferConfig.SourceAddr );
    }
    else
    {
        /* The block size was not configured */
    }

    if( GPDMA_REQUEST_OK == dmaState )
    {
        dmaState = Gpdma_Set_DestinationAddr( periphId, channelId, xferConfig.DestinationAddr );
    }
    else
    {
        /* The source address was not configured */
    }

    if( GPDMA_REQUEST_OK == dmaState )
    {
        dmaState = Gpdma_Set_ChannelActive( periphId, channelId );
    }
    else
    {
        /* The destination address was not configured */
    }

    if( GPDMA_REQUEST_OK == dmaState )
    {
        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Fills the Gpdma transfer configuration of a channel: the input stream goes from the memory to JPEG_DIR, the
 *        output stream from JPEG_DOR to the memory, 32 bit data, one request per word, one block of the buffer.
 *
 * \param stream     [in]:  Channel of the input stream or of the output stream.
 * \param memoryAddr [in]:  Address of the memory buffer.
 * \param byteCnt    [in]:  Count of the bytes of the buffer.
 * \param xferConfig [out]: Pointer to the transfer configuration to fill. Must not be NULL.
 */
static void Jpeg_Dma_Get_XferConfig( jpeg_DmaStream_t stream,
                                     uint32_t memoryAddr,
                                     jpeg_Size_t byteCnt,
                                     gpdma_TransferConfig_t * const xferConfig )
{
    xferConfig->XferListExecMode       = GPDMA_XFER_LIST_EXEC_ONCE;
    xferConfig->Direction              = jpeg_DmaDirectionLut[ stream ];
    xferConfig->EventMode              = GPDMA_TRANSFER_EVENT_BLOCK;
    xferConfig->TriggerType            = GPDMA_TRG_NOT_USED;
    xferConfig->TriggerMode            = GPDMA_TRIGGER_BLOCK;
    xferConfig->RequestSource          = jpeg_DmaRequestLut[ stream ];
    xferConfig->RequestMode            = GPDMA_PERIPH_REQ_SINGLE;
    xferConfig->BlockSize              = (gpdma_BlockSize_t)byteCnt;
    xferConfig->BlockRepetitionCount   = 0u;

    xferConfig->SourceDataSize         = GPDMA_DATA_SIZE_32BITS;
    xferConfig->SourceBurstLength      = JPEG_DMA_BURST_LEN;
    xferConfig->SourcePortId           = GPDMA_PORT_DEFAULT;
    xferConfig->SourceDataOp           = GPDMA_SRC_DATA_PRESERVE;

    xferConfig->DestinationDataSize    = GPDMA_DATA_SIZE_32BITS;
    xferConfig->DestinationBurstLength = JPEG_DMA_BURST_LEN;
    xferConfig->DestinationPortId      = GPDMA_PORT_DEFAULT;
    xferConfig->DestinationDataOp      = GPDMA_DEST_DATA_PRESERVE;

    if( JPEG_DMA_STREAM_INPUT == stream )
    {
        xferConfig->SourceAddr          = (gpdma_SrcAddr_t)memoryAddr;
        xferConfig->SourceAddrMode      = GPDMA_ADDR_INCREMENT;
        xferConfig->DestinationAddr     = (gpdma_DstAddr_t)Jpeg_Reg_Get_DirAddr();
        xferConfig->DestinationAddrMode = GPDMA_ADDR_STATIC;
    }
    else
    {
        xferConfig->SourceAddr          = (gpdma_SrcAddr_t)Jpeg_Reg_Get_DorAddr();
        xferConfig->SourceAddrMode      = GPDMA_ADDR_STATIC;
        xferConfig->DestinationAddr     = (gpdma_DstAddr_t)memoryAddr;
        xferConfig->DestinationAddrMode = GPDMA_ADDR_INCREMENT;
    }
}


/**
 * \brief Returns the Gpdma channel of the input stream or of the output stream.
 *
 * \param stream [in]: Channel of the input stream or of the output stream.
 *
 * \return Gpdma channel of the configuration of the module.
 */
static gpdma_ChannelId_t Jpeg_Dma_Get_ChannelId( jpeg_DmaStream_t stream )
{
    gpdma_ChannelId_t retChannelId = jpeg_RuntimeData.DmaOutputChannelId;

    if( JPEG_DMA_STREAM_INPUT == stream )
    {
        retChannelId = jpeg_RuntimeData.DmaInputChannelId;
    }
    else
    {
        retChannelId = jpeg_RuntimeData.DmaOutputChannelId;
    }

    return ( retChannelId );
}


/**
 * \brief Ends the DMA transfers at the end of the conversion: disables the DMA requests, stops both channels,
 *        calculates the count of the bytes the output channel moved from its remaining block size and hands the rest
 *        of the output FIFO to the CPU (the job is then ended by \ref Jpeg_Service like in the interrupt mode).
 */
static void Jpeg_Dma_End( void )
{
    Jpeg_Reset_RegBit( JPEG_REG_CR, JPEG_CR_DMA_MASK );

    (void)Jpeg_Dma_Stop();

    if( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.OutPending )
    {
        gpdma_BlockSize_t    remaining = 0u;
        jpeg_Size_t          moved     = 0u;
        const jpeg_Size_t    armed     = jpeg_DmaArmedSize[ JPEG_DMA_STREAM_OUTPUT ];
        const gpdma_RequestState_t dmaState = Gpdma_Get_BlockSize( jpeg_RuntimeData.DmaPeriphId,
                                                                   jpeg_RuntimeData.DmaOutputChannelId,
                                                                   &remaining );

        if( ( GPDMA_REQUEST_OK == dmaState ) &&
            ( armed            >= remaining )    )
        {
            moved = armed - remaining;
        }
        else
        {
            /* The remaining block size is not available, the CPU reads the rest */
        }

        (void)Jpeg_Cache_Maintain( JPEG_CACHE_INVALIDATE,
                                   (uint32_t)(uintptr_t)jpeg_RuntimeData.OutData,
                                   jpeg_RuntimeData.OutSize );

        jpeg_RuntimeData.OutIdx    = moved;
        jpeg_RuntimeData.TotalOut += moved;
    }
    else
    {
        /* No output buffer was armed */
    }

    jpeg_RuntimeData.InPending = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.CpuServed = JPEG_FUNCTION_ACTIVE;

    Jpeg_Service();
}


/**
 * \brief Gpdma transfer complete callback of the input channel: the input buffer is consumed.
 */
static void Jpeg_Dma_InputComplete( void )
{
    if( ( JPEG_STATE_IDLE      != jpeg_RuntimeData.State     ) &&
        ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InPending )    )
    {
        jpeg_RuntimeData.InPending = JPEG_FUNCTION_INACTIVE;
        jpeg_RuntimeData.InIdx     = jpeg_RuntimeData.InSize;

        Jpeg_Notify_InputNeeded();
    }
    else
    {
        /* No input buffer is armed */
    }
}


/**
 * \brief Gpdma transfer complete callback of the output channel: the output buffer is full.
 */
static void Jpeg_Dma_OutputComplete( void )
{
    if( ( JPEG_STATE_IDLE      != jpeg_RuntimeData.State      ) &&
        ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.OutPending )    )
    {
        (void)Jpeg_Cache_Maintain( JPEG_CACHE_INVALIDATE,
                                   (uint32_t)(uintptr_t)jpeg_RuntimeData.OutData,
                                   jpeg_RuntimeData.OutSize );

        jpeg_RuntimeData.OutPending = JPEG_FUNCTION_INACTIVE;
        jpeg_RuntimeData.OutIdx     = jpeg_RuntimeData.OutSize;
        jpeg_RuntimeData.TotalOut  += jpeg_RuntimeData.OutSize;

        Jpeg_Notify_OutputReady( jpeg_RuntimeData.OutSize );
    }
    else
    {
        /* No output buffer is armed */
    }
}


/**
 * \brief Gpdma error callback of both channels: the job ends with \ref JPEG_ERROR_DMA.
 *
 * \param errorMask [in]: Gpdma errors, not used (every Gpdma error is a transfer error of the job).
 */
static void Jpeg_Dma_Error( gpdma_ErrorMaskId_t errorMask )
{
    (void)errorMask;

    if( JPEG_STATE_IDLE != jpeg_RuntimeData.State )
    {
        Jpeg_Fail_Job( JPEG_ERROR_DMA );
    }
    else
    {
        /* No job runs */
    }
}


/**
 * \brief Releases the peripheral: aborts the job, disables the interrupt, releases the Gpdma channels, disables the
 *        core, resets the peripheral, disables the clock and clears the runtime data.
 *
 * \return Returns \ref JPEG_REQUEST_OK if every step succeeded. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Release( void )
{
    jpeg_RequestState_t retState = JPEG_REQUEST_ERROR;

    const jpeg_RequestState_t dmaStopState = Jpeg_Dma_Stop();
    const jpeg_RequestState_t codecState   = Jpeg_Reg_Reset_Codec();

    Jpeg_Reg_Disable_Core();

    nvic_RequestState_t nvicState = NVIC_REQUEST_OK;

    if( JPEG_XFER_MODE_POLL != jpeg_RuntimeData.XferMode )
    {
        nvicState = Nvic_Set_PeriphIrq_Inactive( NVIC_PERIPH_IRQ_JPEG );
    }
    else
    {
        /* The interrupt is not used */
    }

    const jpeg_RequestState_t dmaDeinitState = Jpeg_Dma_Deinit();
    const rcc_RequestState_t  rstActState    = Rcc_Set_ResetActive( RCC_PERIPH_JPEG );
    const rcc_RequestState_t  rstInactState  = Rcc_Set_ResetInactive( RCC_PERIPH_JPEG );
    const rcc_RequestState_t  clkState       = Rcc_Set_PeriphInactive( RCC_PERIPH_JPEG );

    Jpeg_Reset_Runtime();

    if( ( JPEG_REQUEST_OK == dmaStopState   ) &&
        ( JPEG_REQUEST_OK == codecState     ) &&
        ( NVIC_REQUEST_OK == nvicState      ) &&
        ( JPEG_REQUEST_OK == dmaDeinitState ) &&
        ( RCC_REQUEST_OK  == rstActState    ) &&
        ( RCC_REQUEST_OK  == rstInactState  ) &&
        ( RCC_REQUEST_OK  == clkState       )    )
    {
        retState = JPEG_REQUEST_OK;
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Clears the runtime data: the module is not initialized, no tables are available, no job runs and no
 *        callback is registered.
 */
static void Jpeg_Reset_Runtime( void )
{
    jpeg_RuntimeData.InitState               = JPEG_FUNCTION_INACTIVE;
    jpeg_RuntimeData.XferMode                = JPEG_XFER_MODE_POLL;
    jpeg_RuntimeData.CpuServed               = JPEG_FUNCTION_ACTIVE;
    jpeg_RuntimeData.HeaderMode              = JPEG_HEADER_ON;
    jpeg_RuntimeData.Quality                 = JPEG_DEFAULT_QUALITY;
    jpeg_RuntimeData.HeaderParsedCallback    = JPEG_NULL_PTR;
    jpeg_RuntimeData.InputNeededCallback     = JPEG_NULL_PTR;
    jpeg_RuntimeData.OutputReadyCallback     = JPEG_NULL_PTR;
    jpeg_RuntimeData.EndOfConversionCallback = JPEG_NULL_PTR;
    jpeg_RuntimeData.ErrorCallback           = JPEG_NULL_PTR;
    jpeg_RuntimeData.State                   = JPEG_STATE_IDLE;
    jpeg_RuntimeData.ImageInfoValid          = JPEG_FUNCTION_INACTIVE;

    for( uint32_t idx = 0u; JPEG_QUANT_TABLE_CNT > idx; idx++ )
    {
        jpeg_RuntimeData.QuantTableSet[ idx ] = JPEG_FUNCTION_INACTIVE;
    }

    for( uint32_t idx = 0u; JPEG_HUFFMAN_CNT > idx; idx++ )
    {
        jpeg_RuntimeData.HuffmanTableSet[ idx ] = JPEG_FUNCTION_INACTIVE;
    }

    for( jpeg_DmaStream_t stream = JPEG_DMA_STREAM_INPUT; JPEG_DMA_STREAM_CNT > stream; stream++ )
    {
        jpeg_RuntimeData.DmaChannelInit[ stream ] = JPEG_FUNCTION_INACTIVE;
    }

    Jpeg_Reset_Stream();
}

/**
 * \brief Starts a decoder job: stops and resets the codec, enables the decoder with the header parsing, clears the
 *        state of the job and starts the codec.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the codec was started. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Start_Decode( void )
{
    jpeg_RequestState_t         retState  = Jpeg_Reg_Reset_Codec();
    const jpeg_RegImageConfig_t emptyConfig = { 0u };

    if( JPEG_REQUEST_OK == retState )
    {
        jpeg_RegImageConfig_t regConfig = emptyConfig;

        regConfig.Decode = 1u;
        regConfig.Header = 1u;

        Jpeg_Reg_Set_ImageConfig( &regConfig );

        Jpeg_Reset_Stream();

        jpeg_RuntimeData.State          = JPEG_STATE_DECODING;
        jpeg_RuntimeData.ImageInfoValid = JPEG_FUNCTION_INACTIVE;

        if( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.CpuServed )
        {
            Jpeg_Set_RegBit( JPEG_REG_CR, JPEG_CR_DMA_MASK );
        }
        else
        {
            /* The CPU serves the FIFOs */
        }

        Jpeg_Update_Irq();
        Jpeg_Reg_Start_Codec();
    }
    else
    {
        retState = JPEG_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Tells whether a marker is a start of frame marker (SOF0 to SOF15 without DHT, JPG and DAC).
 *
 * \param marker [in]: Second byte of the marker.
 *
 * \return \ref JPEG_FUNCTION_ACTIVE for a start of frame marker. Otherwise \ref JPEG_FUNCTION_INACTIVE.
 */
static jpeg_FunctionState_t Jpeg_Is_SofMarker( jpeg_Size_t marker )
{
    jpeg_FunctionState_t retState = JPEG_FUNCTION_INACTIVE;

    if( ( JPEG_MARKER_SOF0  <= marker ) &&
        ( JPEG_MARKER_SOF15 >= marker )    )
    {
        retState = JPEG_FUNCTION_ACTIVE;

        if( ( JPEG_MARKER_DHT == marker ) ||
            ( JPEG_MARKER_JPG == marker ) ||
            ( JPEG_MARKER_DAC == marker )    )
        {
            retState = JPEG_FUNCTION_INACTIVE;
        }
        else
        {
            /* A start of frame marker */
        }
    }
    else
    {
        /* Not a start of frame marker */
    }

    return ( retState );
}


/**
 * \brief Tells whether a marker has no length field (TEM and the restart markers RST0 to RST7).
 *
 * \param marker [in]: Second byte of the marker.
 *
 * \return \ref JPEG_FUNCTION_ACTIVE for a marker without a segment. Otherwise \ref JPEG_FUNCTION_INACTIVE.
 */
static jpeg_FunctionState_t Jpeg_Is_StandaloneMarker( jpeg_Size_t marker )
{
    jpeg_FunctionState_t retState = JPEG_FUNCTION_INACTIVE;

    if( JPEG_MARKER_TEM == marker )
    {
        retState = JPEG_FUNCTION_ACTIVE;
    }
    else if( JPEG_MARKER_RST0 <= marker )
    {
        if( JPEG_MARKER_RST7 >= marker )
        {
            retState = JPEG_FUNCTION_ACTIVE;
        }
        else
        {
            /* Above the restart markers */
        }
    }
    else
    {
        /* A marker with a segment */
    }

    return ( retState );
}


/**
 * \brief Checks a start of frame segment: the hardware decodes the baseline frame (SOF0) and the extended sequential
 *        Huffman frame (SOF1) of 8-bit samples.
 *
 * \param data [in]: Start of the stream.
 * \param size [in]: Count of the bytes of the buffer.
 * \param pos  [in]: Position of the prefix of the marker.
 *
 * \return The result of the scan.
 */
static jpeg_ScanResult_t Jpeg_Check_Sof( const jpeg_Data_t * const data, jpeg_Size_t size, jpeg_Size_t pos )
{
    jpeg_ScanResult_t retResult = JPEG_SCAN_UNSUPPORTED;
    const jpeg_Size_t marker    = data[ pos + 1u ];

    if( ( JPEG_MARKER_SOF0 == marker ) ||
        ( JPEG_MARKER_SOF1 == marker )    )
    {
        if( size > ( pos + JPEG_SOF_PRECISION_OFFSET ) )
        {
            if( JPEG_SOF_PRECISION == data[ pos + JPEG_SOF_PRECISION_OFFSET ] )
            {
                retResult = JPEG_SCAN_SUPPORTED;
            }
            else
            {
                /* Samples of other than 8 bit */
                retResult = JPEG_SCAN_UNSUPPORTED;
            }
        }
        else
        {
            retResult = JPEG_SCAN_INCOMPLETE;
        }
    }
    else
    {
        /* Progressive, lossless, hierarchical or arithmetic coded frame */
        retResult = JPEG_SCAN_UNSUPPORTED;
    }

    return ( retResult );
}


/**
 * \brief Walks the marker segments behind the SOI marker up to the start of frame.
 *
 * \param data [in]: Start of the stream.
 * \param size [in]: Count of the bytes of the buffer (at least the SOI marker).
 *
 * \return The result of the scan.
 */
static jpeg_ScanResult_t Jpeg_Scan_Segments( const jpeg_Data_t * const data, jpeg_Size_t size )
{
    jpeg_ScanResult_t    retResult = JPEG_SCAN_INCOMPLETE;
    jpeg_FunctionState_t scanning  = JPEG_FUNCTION_ACTIVE;
    jpeg_Size_t          pos       = JPEG_MARKER_SIZE;

    for( uint32_t idx = 0u;
         ( JPEG_SCAN_SEGMENT_MAX > idx ) &&
         ( JPEG_FUNCTION_ACTIVE == scanning );
         idx++ )
    {
        if( size > ( pos + 1u ) )
        {
            const jpeg_Size_t          marker     = data[ pos + 1u ];
            const jpeg_FunctionState_t isSof      = Jpeg_Is_SofMarker( marker );
            const jpeg_FunctionState_t standalone = Jpeg_Is_StandaloneMarker( marker );

            if( JPEG_MARKER_PREFIX != data[ pos ] )
            {
                retResult = JPEG_SCAN_CORRUPT;
                scanning  = JPEG_FUNCTION_INACTIVE;
            }
            else if( JPEG_MARKER_PREFIX == marker )
            {
                /* A fill byte in front of a marker */
                pos++;
            }
            else if( JPEG_FUNCTION_ACTIVE == isSof )
            {
                retResult = Jpeg_Check_Sof( data, size, pos );
                scanning  = JPEG_FUNCTION_INACTIVE;
            }
            else if( ( JPEG_MARKER_SOS == marker ) ||
                     ( JPEG_MARKER_EOI == marker )    )
            {
                /* The scan or the end of the image without a frame */
                retResult = JPEG_SCAN_CORRUPT;
                scanning  = JPEG_FUNCTION_INACTIVE;
            }
            else if( JPEG_FUNCTION_ACTIVE == standalone )
            {
                /* A marker without a length */
                pos += JPEG_MARKER_SIZE;
            }
            else if( size > ( pos + JPEG_SEGMENT_LENGTH_OFFSET + 1u ) )
            {
                const jpeg_Size_t lengthHigh = data[ pos + JPEG_SEGMENT_LENGTH_OFFSET ];
                const jpeg_Size_t lengthLow  = data[ pos + JPEG_SEGMENT_LENGTH_OFFSET + 1u ];
                const jpeg_Size_t length     = ( lengthHigh << JPEG_BYTE_BITS ) | lengthLow;

                if( JPEG_SEGMENT_LENGTH_SIZE > length )
                {
                    /* The length includes its own field */
                    retResult = JPEG_SCAN_CORRUPT;
                    scanning  = JPEG_FUNCTION_INACTIVE;
                }
                else
                {
                    pos += JPEG_MARKER_SIZE + length;
                }
            }
            else
            {
                /* The length of the segment is behind the buffer */
                scanning = JPEG_FUNCTION_INACTIVE;
            }
        }
        else
        {
            /* The buffer ends before the next marker */
            scanning = JPEG_FUNCTION_INACTIVE;
        }
    }

    return ( retResult );
}


/**
 * \brief Scans the first bytes of a stream: the SOI marker and the marker segments up to the start of frame.
 *
 * \param data [in]: Start of the stream.
 * \param size [in]: Count of the bytes of the buffer.
 *
 * \return The result of the scan.
 */
static jpeg_ScanResult_t Jpeg_Scan_Header( const jpeg_Data_t * const data, jpeg_Size_t size )
{
    jpeg_ScanResult_t retResult = JPEG_SCAN_INCOMPLETE;

    if( JPEG_MARKER_SIZE <= size )
    {
        if( ( JPEG_MARKER_PREFIX == data[ 0u ] ) &&
            ( JPEG_MARKER_SOI    == data[ 1u ] )    )
        {
            retResult = Jpeg_Scan_Segments( data, size );
        }
        else
        {
            retResult = JPEG_SCAN_CORRUPT;
        }
    }
    else
    {
        /* Too short to tell */
    }

    return ( retResult );
}


/**
 * \brief Estimates the quality factor of the parsed quantization table 0 from the sample luminance table: for every
 *        coefficient the scale in percent is the ratio of the two, the quality follows from the scale the way the
 *        scale follows from the quality (\ref Jpeg_Calc_Scale); the estimate is the mean of the 64 coefficients.
 *
 * \return Estimated quality factor, \ref JPEG_QUALITY_MIN to \ref JPEG_QUALITY_MAX.
 */
static jpeg_Quality_t Jpeg_Calc_Quality( void )
{
    jpeg_Count_t sum = 0u;

    for( uint32_t wordIdx = 0u; JPEG_REG_QUANT_WORD_CNT > wordIdx; wordIdx++ )
    {
        const uint32_t quantWord = Jpeg_Reg_Get_QuantWord( JPEG_QUANT_TABLE_0, wordIdx );

        for( uint32_t byteIdx = 0u; JPEG_QUANT_PER_WORD > byteIdx; byteIdx++ )
        {
            const uint32_t zigzagIdx = ( wordIdx * JPEG_QUANT_PER_WORD ) + byteIdx;
            const uint32_t quantVal  = ( quantWord >> ( JPEG_BYTE_BITS * byteIdx ) ) & JPEG_BYTE_MASK;
            const uint32_t baseVal   = jpeg_StdQuantTable[ JPEG_STD_QUANT_LUM ].Values[ jpeg_ZigzagOrder[ zigzagIdx ] ];
            const uint32_t scale     = ( quantVal * JPEG_QUANT_SCALE_DIV ) / baseVal;

            if( JPEG_QUANT_VALUE_MIN == quantVal )
            {
                sum += JPEG_QUALITY_MAX;
            }
            else if( JPEG_QUANT_SCALE_DIV >= scale )
            {
                sum += ( JPEG_QUALITY_HIGH_REF - scale ) / JPEG_QUALITY_HIGH_SLOPE;
            }
            else
            {
                sum += JPEG_QUALITY_LOW_REF / scale;
            }
        }
    }

    jpeg_Count_t quality = sum / JPEG_QUANT_TABLE_SIZE;

    if( JPEG_QUALITY_MIN > quality )
    {
        quality = JPEG_QUALITY_MIN;
    }
    else
    {
        /* Not below the minimum */
    }

    if( JPEG_QUALITY_MAX < quality )
    {
        quality = JPEG_QUALITY_MAX;
    }
    else
    {
        /* Not above the maximum */
    }

    return ( (jpeg_Quality_t)quality );
}


/**
 * \brief Reads the image parameters the header parser stored in the registers.
 *
 * The color space follows from the count of the components (1: grayscale, 3: YCbCr, 4: CMYK, the hardware does not
 * tell YCbCr from RGB), the subsampling from the sampling factors of the first component (1 x 1: 4:4:4, 2 x 1: 4:2:2,
 * 2 x 2: 4:2:0). Another count of the components or other sampling factors are not supported.
 *
 * \param imageInfo [out]: Parameters to fill.
 *
 * \return Returns \ref JPEG_REQUEST_OK if the image is supported. Otherwise returns \ref JPEG_REQUEST_ERROR.
 */
static jpeg_RequestState_t Jpeg_Read_ImageInfo( jpeg_ImageInfo_t * const imageInfo )
{
    jpeg_RequestState_t   retState = JPEG_REQUEST_OK;
    jpeg_RegImageConfig_t regConfig;
    jpeg_ImageConfig_t    imageConfig = { 0u };
    jpeg_ImageInfo_t      calcInfo;

    Jpeg_Reg_Get_ImageConfig( &regConfig );

    imageConfig.ColorSpace  = JPEG_COLOR_GRAYSCALE;
    imageConfig.Subsampling = JPEG_SUBSAMPLING_444;
    imageConfig.Width       = (jpeg_Dimension_t)regConfig.Width;
    imageConfig.Height      = (jpeg_Dimension_t)regConfig.Height;

    if( JPEG_NF_THREE == regConfig.Components )
    {
        imageConfig.ColorSpace = JPEG_COLOR_YCBCR;
    }
    else if( JPEG_NF_CMYK == regConfig.Components )
    {
        imageConfig.ColorSpace = JPEG_COLOR_CMYK;
    }
    else if( JPEG_NF_GRAYSCALE == regConfig.Components )
    {
        imageConfig.ColorSpace = JPEG_COLOR_GRAYSCALE;
    }
    else
    {
        /* Two components are not supported */
        retState = JPEG_REQUEST_ERROR;
    }

    if( JPEG_COLOR_YCBCR == imageConfig.ColorSpace )
    {
        const uint32_t horizontal = regConfig.Component[ 0u ].HorizontalFactor;
        const uint32_t vertical   = regConfig.Component[ 0u ].VerticalFactor;

        if( ( JPEG_SAMPLING_444 == horizontal ) &&
            ( JPEG_SAMPLING_444 == vertical   )    )
        {
            imageConfig.Subsampling = JPEG_SUBSAMPLING_444;
        }
        else if( ( JPEG_SAMPLING_DOUBLE == horizontal ) &&
                 ( JPEG_SAMPLING_444    == vertical   )    )
        {
            imageConfig.Subsampling = JPEG_SUBSAMPLING_422;
        }
        else if( ( JPEG_SAMPLING_DOUBLE == horizontal ) &&
                 ( JPEG_SAMPLING_DOUBLE == vertical   )    )
        {
            imageConfig.Subsampling = JPEG_SUBSAMPLING_420;
        }
        else
        {
            /* Other sampling factors cannot be described */
            retState = JPEG_REQUEST_ERROR;
        }
    }
    else
    {
        /* The other color spaces have one block per component */
    }

    if( ( JPEG_REQUEST_OK  == retState                  ) &&
        ( JPEG_DIMENSION_MIN > imageConfig.Width        )    )
    {
        retState = JPEG_REQUEST_ERROR;
    }
    else
    {
        /* The width is valid or an error is set already */
    }

    if( ( JPEG_REQUEST_OK  == retState                  ) &&
        ( JPEG_DIMENSION_MIN > imageConfig.Height       )    )
    {
        retState = JPEG_REQUEST_ERROR;
    }
    else
    {
        /* The height is valid or an error is set already */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        imageConfig.Quality = JPEG_QUALITY_MIN;

        retState = Jpeg_Calc_ImageInfo( &imageConfig, &calcInfo );
    }
    else
    {
        /* The image is not supported */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        calcInfo.Quality = Jpeg_Calc_Quality();
        *imageInfo       = calcInfo;
    }
    else
    {
        /* The image is not supported */
    }

    return ( retState );
}


/**
 * \brief Handles the end of the header parsing of a decoder job: reads the image parameters, clears the flag and
 *        calls the header callback. An image the module cannot describe ends the job with
 *        \ref JPEG_ERROR_UNSUPPORTED.
 */
static void Jpeg_Check_Header( void )
{
    const uint32_t headerFlag = Jpeg_Get_RegBit( JPEG_REG_SR, JPEG_SR_HPDF );

    if( ( JPEG_STATE_DECODING    == jpeg_RuntimeData.State        ) &&
        ( JPEG_FUNCTION_INACTIVE == jpeg_RuntimeData.HeaderParsed ) &&
        ( JPEG_BITS_CLEAR        != headerFlag                    )    )
    {
        jpeg_ImageInfo_t imageInfo;

        const jpeg_RequestState_t infoState = Jpeg_Read_ImageInfo( &imageInfo );

        Jpeg_Reg_Clear_Flags( JPEG_REG_CFR_CHPDF );

        jpeg_RuntimeData.HeaderParsed = JPEG_FUNCTION_ACTIVE;

        if( JPEG_REQUEST_OK == infoState )
        {
            jpeg_RuntimeData.ImageInfo      = imageInfo;
            jpeg_RuntimeData.ImageInfoValid = JPEG_FUNCTION_ACTIVE;

            Jpeg_Update_Irq();
            Jpeg_Notify_HeaderParsed( &imageInfo );
        }
        else
        {
            Jpeg_Fail_Job( JPEG_ERROR_UNSUPPORTED );
        }
    }
    else
    {
        /* No header was parsed */
    }
}


/**
 * \brief Calls the header parsed callback if registered.
 *
 * \param imageInfo [in]: Parameters of the image.
 */
static void Jpeg_Notify_HeaderParsed( const jpeg_ImageInfo_t * const imageInfo )
{
    if( JPEG_NULL_PTR != jpeg_RuntimeData.HeaderParsedCallback )
    {
        jpeg_RuntimeData.HeaderParsedCallback( imageInfo );
    }
    else
    {
        /* No callback registered */
    }
}


/* =========================== INTERRUPT HANDLERS =========================== */

/**
 * \brief JPEG interrupt handler: serves the FIFOs in interrupt mode, reports the end of the conversion in DMA mode
 *        (the data are moved by the Gpdma channels).
 */
static void Jpeg_IsrHandler( void )
{
    if( ( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.InitState ) &&
        ( JPEG_STATE_IDLE      != jpeg_RuntimeData.State     )    )
    {
        if( JPEG_FUNCTION_ACTIVE == jpeg_RuntimeData.CpuServed )
        {
            Jpeg_Service();
        }
        else
        {
            Jpeg_Service_Dma();
        }
    }
    else
    {
        /* No job runs */
    }
}

#endif /* JPEG */

/* ================================ TASKS =================================== */
