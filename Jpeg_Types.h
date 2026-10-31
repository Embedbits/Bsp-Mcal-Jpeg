/**
 * \author Mr.Nobody
 * \file Jpeg_Types.h
 * \ingroup Jpeg
 * \brief Jpeg module global types definition
 *
 * This file contains the types definitions used across the module and are
 * available for other modules through Port file.
 *
 */

/**
 * \defgroup Jpeg Jpeg
 * \brief JPEG codec (JPEG) module
 */

#ifndef JPEG_JPEG_TYPES_H
#define JPEG_JPEG_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== INCLUDES ================================== */
#include "stdint.h"                         /* Module types definition        */
#include "Gpdma_Port.h"                     /* DMA types used by jpeg_Config_t */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Null pointer definition */
#define JPEG_NULL_PTR                       ( ( void* ) 0u )

/** Smallest quality factor of the quantization tables (the coarsest quantization) */
#define JPEG_QUALITY_MIN                    ( 1u )

/** Largest quality factor of the quantization tables (all quantization values 1) */
#define JPEG_QUALITY_MAX                    ( 100u )

/** Quality factor of \ref jpeg_ImageConfig_t::Quality which selects the quality set by \ref Jpeg_Set_Quality */
#define JPEG_QUALITY_MODULE                 ( 0u )

/** Smallest width and height of an image in pixels */
#define JPEG_DIMENSION_MIN                  ( 1u )

/** Count of the coefficients of a quantization table (an 8 x 8 block) */
#define JPEG_QUANT_TABLE_SIZE               ( 64u )

/** Count of the code lengths of a Huffman table (the counts of the codes of 1 to 16 bits) */
#define JPEG_HUFFMAN_LENGTH_CNT             ( 16u )

/** Maximal count of the values of a DC Huffman table (the difference categories 0 to 11) */
#define JPEG_HUFFMAN_DC_VALUES_MAX          ( 12u )

/** Maximal count of the values of an AC Huffman table (the run / size symbols and the markers EOB and ZRL) */
#define JPEG_HUFFMAN_AC_VALUES_MAX          ( 162u )

/* ========================== EXPORTED MACROS =============================== */

/* ============================== TYPEDEFS ================================== */

/** \brief Type signaling major version of SW module */
typedef uint8_t jpeg_MajorVersion_t;


/** \brief Type signaling minor version of SW module */
typedef uint8_t jpeg_MinorVersion_t;


/** \brief Type signaling patch version of SW module */
typedef uint8_t jpeg_PatchVersion_t;


/** \brief Type signaling actual version of SW module */
typedef struct
{
    jpeg_MajorVersion_t Major; /**< Major version */
    jpeg_MinorVersion_t Minor; /**< Minor version */
    jpeg_PatchVersion_t Patch; /**< Patch version */
}   jpeg_ModuleVersion_t;


/** \brief Function status enumeration */
typedef enum
{
    JPEG_FUNCTION_INACTIVE = 0u, /**< Function status is inactive */
    JPEG_FUNCTION_ACTIVE         /**< Function status is active   */
}   jpeg_FunctionState_t;


/** \brief Flag states enumeration */
typedef enum
{
    JPEG_FLAG_INACTIVE = 0u, /**< Inactive flag state */
    JPEG_FLAG_ACTIVE         /**< Active flag state   */
}   jpeg_FlagState_t;


/** \brief Enumeration used to signal request processing state */
typedef enum
{
    JPEG_REQUEST_ERROR = 0u, /**< Processing request failed  */
    JPEG_REQUEST_OK          /**< Processing request succeed */
}   jpeg_RequestState_t;


/** \brief Interrupt priority type definition */
typedef uint32_t jpeg_IrqPrio_t;


/** \brief One byte of the raw image data (the MCU blocks) or of the JPEG stream */
typedef uint8_t jpeg_Data_t;


/** \brief Size of a buffer and count of the bytes of a stream or of the raw image data */
typedef uint32_t jpeg_Size_t;


/** \brief Width or height of an image in pixels */
typedef uint16_t jpeg_Dimension_t;


/** \brief Quality factor of the quantization tables (\ref JPEG_QUALITY_MIN to \ref JPEG_QUALITY_MAX) */
typedef uint8_t jpeg_Quality_t;


/** \brief Count of a kind of the items of an image (the MCU blocks, the codes of a Huffman table) */
typedef uint32_t jpeg_Count_t;


/** \brief Data transfer handling style enumeration */
typedef enum
{
    JPEG_XFER_MODE_POLL = 0u, /**< The FIFOs are served by \ref Jpeg_Task                                     */
    JPEG_XFER_MODE_ISR,       /**< The FIFOs are served by the JPEG interrupt                                 */
    JPEG_XFER_MODE_DMA,       /**< The FIFOs are served by two Gpdma channels, the JPEG interrupt reports the
                                   header and the end of the conversion                                       */
    JPEG_XFER_MODE_CNT        /**< Count of available options                                                 */
}   jpeg_XferMode_t;


/** \brief State of the codec (one job at a time) */
typedef enum
{
    JPEG_STATE_IDLE = 0u, /**< No job runs, a job can be started              */
    JPEG_STATE_ENCODING,  /**< The encoder runs (\ref Jpeg_Set_EncodeStart)   */
    JPEG_STATE_DECODING,  /**< The decoder runs (\ref Jpeg_Set_DecodeStart)   */
    JPEG_STATE_CNT        /**< Count of available options                     */
}   jpeg_State_t;


/** \brief Color space of the raw image data (the components of a pixel in the MCU blocks) */
typedef enum
{
    JPEG_COLOR_GRAYSCALE = 0u, /**< One component (Y), one quantization table                      */
    JPEG_COLOR_YCBCR,          /**< Three components (Y, Cb, Cr), two quantization tables          */
    JPEG_COLOR_RGB,            /**< Three components (R, G, B), three quantization tables, 4:4:4   */
    JPEG_COLOR_CMYK,           /**< Four components (C, M, Y, K), four quantization tables, 4:4:4  */
    JPEG_COLOR_CNT             /**< Count of available options                                     */
}   jpeg_ColorSpace_t;


/** \brief Chroma subsampling of the YCbCr color space (grayscale, RGB and CMYK use \ref JPEG_SUBSAMPLING_444) */
typedef enum
{
    JPEG_SUBSAMPLING_444 = 0u, /**< Every pixel has all components, MCU of 8 x 8 pixels (1 block per component)   */
    JPEG_SUBSAMPLING_422,      /**< Chroma halved horizontally, MCU of 16 x 8 pixels (2 Y blocks + Cb + Cr)       */
    JPEG_SUBSAMPLING_420,      /**< Chroma halved in both directions, MCU of 16 x 16 pixels (4 Y blocks + Cb + Cr) */
    JPEG_SUBSAMPLING_CNT       /**< Count of available options                                                    */
}   jpeg_Subsampling_t;


/** \brief Generation of the JPEG header by the encoder (the markers SOI, DQT, SOF, DHT, SOS in front of the data)
 *
 * The decoder always parses the header: its tables and the image parameters are loaded from it.
 */
typedef enum
{
    JPEG_HEADER_OFF = 0u, /**< The encoder writes only the entropy coded data (no markers)       */
    JPEG_HEADER_ON,       /**< The encoder writes the markers in front of the entropy coded data  */
    JPEG_HEADER_CNT       /**< Count of available options                                         */
}   jpeg_HeaderMode_t;


/** \brief Data streams of the codec */
typedef enum
{
    JPEG_STREAM_INPUT = 0u, /**< Input stream (the raw image data of the encoder, the JPEG stream of the decoder)  */
    JPEG_STREAM_OUTPUT,     /**< Output stream (the JPEG stream of the encoder, the raw image data of the decoder) */
    JPEG_STREAM_CNT         /**< Count of available options                                                       */
}   jpeg_Stream_t;


/** \brief Quantization tables of the codec (the encoder selects them by the color space) */
typedef enum
{
    JPEG_QUANT_TABLE_0 = 0u, /**< Table 0 (luminance, grayscale, red, cyan)   */
    JPEG_QUANT_TABLE_1,      /**< Table 1 (chrominance, green, magenta)       */
    JPEG_QUANT_TABLE_2,      /**< Table 2 (blue, yellow)                      */
    JPEG_QUANT_TABLE_3,      /**< Table 3 (black)                             */
    JPEG_QUANT_TABLE_CNT     /**< Count of available options                  */
}   jpeg_QuantId_t;


/** \brief Huffman tables of the encoder (the tables 0 code the luminance, the tables 1 the other components) */
typedef enum
{
    JPEG_HUFFMAN_DC_0 = 0u, /**< DC table 0 (up to \ref JPEG_HUFFMAN_DC_VALUES_MAX values)  */
    JPEG_HUFFMAN_DC_1,      /**< DC table 1 (up to \ref JPEG_HUFFMAN_DC_VALUES_MAX values)  */
    JPEG_HUFFMAN_AC_0,      /**< AC table 0 (up to \ref JPEG_HUFFMAN_AC_VALUES_MAX values)  */
    JPEG_HUFFMAN_AC_1,      /**< AC table 1 (up to \ref JPEG_HUFFMAN_AC_VALUES_MAX values)  */
    JPEG_HUFFMAN_CNT        /**< Count of available options                                 */
}   jpeg_HuffmanId_t;


/** \brief List of the errors of a job (bit mask) */
typedef enum
{
    JPEG_ERROR_NONE        = 0x00u, /**< No error                                                              */
    JPEG_ERROR_UNSUPPORTED = 0x01u, /**< The stream uses a feature the hardware does not support (progressive,
                                         12-bit samples, arithmetic coding, lossless, hierarchical)           */
    JPEG_ERROR_STREAM      = 0x02u, /**< The stream is corrupt (no SOI marker, a damaged marker segment, the
                                         conversion ended before the header was parsed)                       */
    JPEG_ERROR_DMA         = 0x04u, /**< A Gpdma transfer error                                                */
    JPEG_ERROR_ALL         = 0x07u  /**< All possible errors                                                   */
}   jpeg_Error_t;


/** \brief Quantization table: the 64 coefficients of an 8 x 8 block in the row order of the block (the standard
 *         tables of the JPEG specification are given in this order). The module applies the zigzag order. */
typedef struct
{
    jpeg_Data_t Values[ JPEG_QUANT_TABLE_SIZE ]; /**< Coefficients 1 to 255, row by row */
}   jpeg_QuantTable_t;


/** \brief Huffman table in the form of the JPEG specification (the DHT marker segment): the count of the codes of
 *         every length and the symbols ordered by the code length.
 *
 * The count of the symbols is the sum of \ref Bits. A DC table has at most \ref JPEG_HUFFMAN_DC_VALUES_MAX symbols
 * (the categories 0 to 11), an AC table at most \ref JPEG_HUFFMAN_AC_VALUES_MAX (the run / size symbols 0x01 to 0xFA
 * of the sizes 1 to 10 and the symbols 0x00 = EOB and 0xF0 = ZRL).
 */
typedef struct
{
    jpeg_Data_t Bits[ JPEG_HUFFMAN_LENGTH_CNT ];     /**< Count of the codes of the length 1 to 16 bits          */
    jpeg_Data_t Values[ JPEG_HUFFMAN_AC_VALUES_MAX ]; /**< Symbols in the order of the code lengths               */
}   jpeg_HuffmanTable_t;


/** \brief Image to encode (see \ref Jpeg_Set_EncodeStart) */
typedef struct
{
    jpeg_Dimension_t   Width;       /**< Width in pixels (1 to 65535)                                              */
    jpeg_Dimension_t   Height;      /**< Height in pixels (1 to 65535)                                             */
    jpeg_ColorSpace_t  ColorSpace;  /**< Color space of the raw data                                               */
    jpeg_Subsampling_t Subsampling; /**< Chroma subsampling (\ref JPEG_SUBSAMPLING_444 unless YCbCr)               */
    jpeg_Quality_t     Quality;     /**< Quality factor 1 to 100, \ref JPEG_QUALITY_MODULE for the module quality  */
}   jpeg_ImageConfig_t;


/** \brief Parameters of the image of a job (see \ref Jpeg_Get_ImageInfo): of the encoder job from its
 *         configuration, of the decoder job from the parsed header */
typedef struct
{
    jpeg_Dimension_t   Width;       /**< Width in pixels                                                       */
    jpeg_Dimension_t   Height;      /**< Height in pixels                                                      */
    jpeg_ColorSpace_t  ColorSpace;  /**< Color space of the raw data                                           */
    jpeg_Subsampling_t Subsampling; /**< Chroma subsampling                                                    */
    jpeg_Quality_t     Quality;     /**< Quality factor, estimated from the quantization table 0 by the decoder */
    jpeg_Count_t       McuCount;    /**< Count of the MCU blocks of the image                                  */
    jpeg_Size_t        RawSize;     /**< Size of the raw data in bytes (the MCU blocks of 8 bit components)    */
}   jpeg_ImageInfo_t;


/** \brief Callback of the parsed header of the decoder, called from the interrupt context or from \ref Jpeg_Task
 *
 * \param imageInfo [in]: The image parameters of the stream (the quality is estimated from the quantization table 0).
 */
typedef void ( jpeg_HeaderParsedCallback_t )( const jpeg_ImageInfo_t * const imageInfo );


/** \brief Callback of an input stream which needs the next buffer (\ref Jpeg_Set_InputBuffer), called from the
 *         interrupt context or from \ref Jpeg_Task */
typedef void ( jpeg_InputNeededCallback_t )( void );


/** \brief Callback of an output buffer with the data (it is full or the job ended), called from the interrupt
 *         context or from \ref Jpeg_Task. The next buffer is given by \ref Jpeg_Set_OutputBuffer.
 *
 * \param length [in]: Count of the valid bytes in the buffer.
 */
typedef void ( jpeg_OutputReadyCallback_t )( jpeg_Size_t length );


/** \brief Callback of the end of a job (all the data are in the output buffers), called from the interrupt context
 *         or from \ref Jpeg_Task
 *
 * \param totalLength [in]: Count of the bytes the job produced (the encoder: the size of the JPEG stream in whole
 *                          words, the last bytes may be the padding after the EOI marker).
 */
typedef void ( jpeg_EndOfConversionCallback_t )( jpeg_Size_t totalLength );


/** \brief Callback of an error, called from the interrupt context or from \ref Jpeg_Task. The job is ended.
 *
 * \param errorMask [in]: The errors, bit mask of \ref jpeg_Error_t.
 */
typedef void ( jpeg_ErrorCallback_t )( jpeg_Error_t errorMask );


/** \brief Type used to store Jpeg module configuration (see \ref Jpeg_Init) */
typedef struct
{
    jpeg_XferMode_t                  XferMode;                /**< Data transfer mode (poll / interrupt / DMA)       */
    gpdma_PeriphId_t                 DmaPeriphId;             /**< Gpdma controller (DMA mode only)                  */
    gpdma_ChannelId_t                DmaInputChannelId;       /**< Gpdma channel of the input stream (DMA mode only) */
    gpdma_ChannelId_t                DmaOutputChannelId;      /**< Gpdma channel of the output stream (DMA mode only) */
    gpdma_Priority_t                 DmaPriority;             /**< Priority of both Gpdma channels (DMA mode only)   */
    jpeg_FunctionState_t             StandardTables;          /**< Load the standard quantization and Huffman tables */
    jpeg_HeaderMode_t                HeaderMode;              /**< Header generation of the encoder                  */
    jpeg_IrqPrio_t                   IrqPriority;             /**< JPEG interrupt priority (interrupt and DMA mode)  */
    jpeg_HeaderParsedCallback_t *    HeaderParsedCallback;    /**< Header of the decoder parsed, NULL disables       */
    jpeg_InputNeededCallback_t *     InputNeededCallback;     /**< Input buffer consumed, NULL disables              */
    jpeg_OutputReadyCallback_t *     OutputReadyCallback;     /**< Output buffer with data, NULL disables            */
    jpeg_EndOfConversionCallback_t * EndOfConversionCallback; /**< End of the job, NULL disables                     */
    jpeg_ErrorCallback_t *           ErrorCallback;           /**< Error of the job, NULL disables                   */
}   jpeg_Config_t;

/* ========================== EXPORTED VARIABLES ============================ */

/* ========================= EXPORTED FUNCTIONS ============================= */

#ifdef __cplusplus
}
#endif

#endif /* JPEG_JPEG_TYPES_H */
