/**
 * \author Mr.Nobody
 * \file ItTest_Jpeg.c
 * \ingroup Jpeg
 * \brief Integration tests of the Jpeg module on target.
 *
 * The Jpeg module runs on the MCU together with the real JPEG codec, GPDMA, RCC, NVIC and DCACHE hardware. The codec
 * needs no wiring and no fixture: every test runs on a bare board. The tests encode small images (a smooth gradient of
 * 16 x 16 pixels in grayscale and of 32 x 16 pixels in YCbCr 4:2:0) in the three transfer modes (POLL, ISR, DMA with
 * two Gpdma channels), check the structure of the JPEG stream (the SOI marker, the start of frame with the size and the
 * components, the EOI marker), decode the stream again and compare the raw data with the original within a tolerance
 * (the sample tables with the quality 100 quantize with the step 1), compare the parsed image parameters with the image
 * and the estimated quality with the quality of the encoder. Streams with a progressive frame and without the SOI
 * marker are rejected, a paused stream continues after the resume.
 *
 * Used resources:
 * - JPEG (the clock is the bus clock)
 * - GPDMA1 channel 0 (input stream) and channel 1 (output stream) in the DMA tests
 *
 * \note No fixture feature is needed (no wiring, no external part).
 * \note On the lines without the JPEG codec (every line except STM32H5E4, H5E5, H5F4 and H5F5) every test is ignored
 *       (`TEST_IGNORE_MESSAGE`), see the module README availability table.
 * \note The buffers of the tests are in the RAM of the program and aligned to 16 bytes, so the data cache maintenance
 *       of the DMA mode (the Cache module, not initialized by the tests) is not needed.
 * \note The throughput of the codec in pixels per second is not measured (the module has no time base).
 * \note Compiled for the target but not run on hardware, see the module report.
 */

/* ============================= INCLUDES =================================== */
#include "unity.h"                          /* Unity testing framework        */
#include "IntegrationTesting.h"             /* Integration testing on target  */
#include "Jpeg_Port.h"                      /* Module under test              */
#include "stdint.h"                         /* Standard integer types         */
/* ============================= TYPEDEFS =================================== */

/* ======================= FORWARD DECLARATIONS ============================= */

#if defined(JPEG)
static void                ItJpeg_InputNeededCallback ( void );
static void                ItJpeg_OutputReadyCallback ( jpeg_Size_t length );
static void                ItJpeg_EndOfConversionCallback( jpeg_Size_t totalLength );
static void                ItJpeg_ErrorCallback       ( jpeg_Error_t errorMask );
static void                ItJpeg_HeaderParsedCallback( const jpeg_ImageInfo_t * const imageInfo );
static void                ItJpeg_Reset_Results       ( void );
static jpeg_RequestState_t ItJpeg_Init                ( jpeg_XferMode_t xferMode );
static void                ItJpeg_Wait_Job            ( jpeg_XferMode_t xferMode );
static jpeg_Data_t         ItJpeg_Get_Gradient        ( uint32_t gradient );
static uint32_t            ItJpeg_Get_Field           ( jpeg_Size_t pos );
static void                ItJpeg_Fill_Gray           ( void );
static void                ItJpeg_Fill_YCbCr          ( void );
static jpeg_ImageConfig_t  ItJpeg_Get_Image           ( jpeg_ColorSpace_t colorSpace,
                                                        jpeg_Subsampling_t subsampling,
                                                        jpeg_Dimension_t width );
static jpeg_RequestState_t ItJpeg_Encode              ( jpeg_XferMode_t xferMode,
                                                        const jpeg_ImageConfig_t * const image,
                                                        jpeg_Size_t rawSize );
static jpeg_RequestState_t ItJpeg_Decode              ( jpeg_XferMode_t xferMode, jpeg_Size_t streamSize );
static void                ItJpeg_Assert_Stream       ( jpeg_Dimension_t width,
                                                        jpeg_Dimension_t height,
                                                        jpeg_Size_t componentCnt );
static void                ItJpeg_Assert_Decoded      ( jpeg_Size_t rawSize );
static void                ItJpeg_Run_RoundTrip       ( jpeg_XferMode_t xferMode,
                                                        const jpeg_ImageConfig_t * const image,
                                                        jpeg_Size_t componentCnt );
#endif /* JPEG */

/* ========================= SYMBOLIC CONSTANTS ============================= */

/** Message of a test on a line without the JPEG codec */
#define IT_JPEG_NO_JPEG_MSG                 "No JPEG codec on this line (only STM32H5E4 / H5E5 / H5F4 / H5F5)."

/** Interrupt priority used by the tests */
#define IT_JPEG_IRQ_PRIO                    ( 5u )

/** Quality factor of the round trip tests (the sample tables with the step 1) */
#define IT_JPEG_QUALITY                     ( 100u )

/** Tolerance of the decoded samples against the original ones */
#define IT_JPEG_TOLERANCE                   ( 3 )

/** Tolerance of the estimated quality against the quality of the encoder */
#define IT_JPEG_QUALITY_TOLERANCE           ( 2 )

/** Size of the grayscale image and of the YCbCr image in pixels */
#define IT_JPEG_SIDE                        ( 16u )
#define IT_JPEG_WIDE                        ( 32u )

/** Count of the MCUs and the raw size: grayscale 16 x 16 (4 MCUs of 64 bytes), YCbCr 4:2:0 32 x 16 (2 MCUs of
 *  384 bytes) */
#define IT_JPEG_GRAY_MCUS                   ( 4u )
#define IT_JPEG_GRAY_RAW_SIZE               ( 256u )
#define IT_JPEG_YCBCR_MCUS                  ( 2u )
#define IT_JPEG_YCBCR_RAW_SIZE              ( 768u )

/** Count of the components of the test images */
#define IT_JPEG_COMPONENTS_GRAY             ( 1u )
#define IT_JPEG_COMPONENTS_YCBCR            ( 3u )

/** Count of the luminance blocks side by side in an MCU of the YCbCr 4:2:0 and the width of the MCU in blocks */
#define IT_JPEG_MCU_BLOCKS_WIDE             ( 2u )

/** Size of an 8 x 8 block, the blocks of an MCU of the YCbCr 4:2:0 and the neutral value of a chroma sample */
#define IT_JPEG_BLOCK_SIZE                  ( 64u )
#define IT_JPEG_BLOCKS_YCBCR                ( 6u )
#define IT_JPEG_BLOCKS_Y                    ( 4u )
#define IT_JPEG_BLOCK_SIDE                  ( 8u )
#define IT_JPEG_CHROMA_NEUTRAL              ( 128u )

/** Gradient of the test images: the value of a pixel is the base plus the step times the sum of its coordinates */
#define IT_JPEG_GRADIENT_BASE               ( 40u )
#define IT_JPEG_GRADIENT_STEP               ( 4u )

/** Size of the buffers of the tests in bytes (a multiple of 16, at most 65532) */
#define IT_JPEG_STREAM_SIZE                 ( 4096u )
#define IT_JPEG_DECODED_SIZE                ( 1024u )
#define IT_JPEG_SMALL_SIZE                  ( 32u )

/** Alignment of the buffers of the tests */
#define IT_JPEG_ALIGN                       ( 16u )

/** Limit of the loops waiting for the end of a job */
#define IT_JPEG_WAIT_LIMIT                  ( 20000000u )

/** Count of the calls of Jpeg_Task() while a stream is paused */
#define IT_JPEG_PAUSE_CALLS                 ( 1000u )

/** Bytes of the markers of the JPEG stream */
#define IT_JPEG_MARKER_PREFIX               ( 0xFFu )
#define IT_JPEG_MARKER_SOI                  ( 0xD8u )
#define IT_JPEG_MARKER_EOI                  ( 0xD9u )
#define IT_JPEG_MARKER_SOF0                 ( 0xC0u )
#define IT_JPEG_MARKER_SOF2                 ( 0xC2u )

/** Count of the bytes of a marker and the offsets of the fields of the start of frame segment behind its marker */
#define IT_JPEG_MARKER_SIZE                 ( 2u )
#define IT_JPEG_SOF_PRECISION               ( 4u )
#define IT_JPEG_SOF_HEIGHT                  ( 5u )
#define IT_JPEG_SOF_WIDTH                   ( 7u )
#define IT_JPEG_SOF_COMPONENTS              ( 9u )
#define IT_JPEG_PRECISION                   ( 8u )

/** Bytes behind the EOI marker which the stream may hold (the stream is padded to whole words) */
#define IT_JPEG_PADDING_MAX                 ( 3u )

/** Bits of a byte */
#define IT_JPEG_BYTE_BITS                   ( 8u )

/** Smallest size of the stream of an encoded image: the markers and the data */
#define IT_JPEG_STREAM_MIN                  ( 64u )

/* ========================== LOCAL VARIABLES =============================== */

#if defined(JPEG)

/** Raw image (the MCUs) and the buffers of the stream and of the decoded image */
static jpeg_Data_t itJpeg_Raw[ IT_JPEG_YCBCR_RAW_SIZE ] __attribute__((aligned(IT_JPEG_ALIGN)));
static jpeg_Data_t itJpeg_Stream[ IT_JPEG_STREAM_SIZE ] __attribute__((aligned(IT_JPEG_ALIGN)));
static jpeg_Data_t itJpeg_Decoded[ IT_JPEG_DECODED_SIZE ] __attribute__((aligned(IT_JPEG_ALIGN)));
static jpeg_Data_t itJpeg_Small[ IT_JPEG_SMALL_SIZE ] __attribute__((aligned(IT_JPEG_ALIGN)));

/** Counters of the callbacks and the results of the last job */
static volatile uint32_t        itJpeg_InputCnt;
static volatile uint32_t        itJpeg_OutputCnt;
static volatile jpeg_Size_t     itJpeg_OutputLast;
static volatile uint32_t        itJpeg_EndCnt;
static volatile jpeg_Size_t     itJpeg_EndLength;
static volatile uint32_t        itJpeg_ErrorCnt;
static volatile jpeg_Error_t    itJpeg_LastError;
static volatile uint32_t        itJpeg_HeaderCnt;
static jpeg_ImageInfo_t         itJpeg_HeaderInfo;

#endif /* JPEG */

/* ============================= TEST SETUP ================================= */

void setUp( void )
{
    /* No per-test state */
}


void tearDown( void )
{
    /* Peripheral state is cleared by system reset after the test case */
}

/* ===================== MODULE VERSION AND INITIALIZATION ================== */

/**
 * \brief   Jpeg_Get_ModuleVersion() returns the version of the module.
 *
 * \par Expected results
 * - The version is not 0.0.0.
 */
void It_Jpeg_Get_ModuleVersion_ReturnsVersion( void )
{
    const jpeg_ModuleVersion_t version = Jpeg_Get_ModuleVersion();

    TEST_ASSERT_GREATER_THAN_UINT32( 0u, (uint32_t)version.Major + version.Minor + version.Patch );
}


/**
 * \brief   Jpeg_Init() and Jpeg_Deinit() work in the three transfer modes.
 *
 * \par Expected results
 * - JPEG_REQUEST_OK from the initialization and the release, the state is idle, a second initialization without the
 *   release is rejected, the module can be initialized again after the release.
 */
void It_Jpeg_Init_AllModes_InitDeinitOk( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    for( jpeg_XferMode_t xferMode = JPEG_XFER_MODE_POLL; JPEG_XFER_MODE_CNT > xferMode; xferMode++ )
    {
        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, ItJpeg_Init( xferMode ) );
        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
        TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
        TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, ItJpeg_Init( xferMode ) );
        TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Deinit() );
    }
#else
    TEST_IGNORE_MESSAGE( IT_JPEG_NO_JPEG_MSG );
#endif /* JPEG */
}


/**
 * \brief   Jpeg_Get_RawSize() returns the size of the raw data of the test images.
 *
 * \par Expected results
 * - 256 bytes for the grayscale image of 16 x 16 pixels, 768 bytes for the YCbCr 4:2:0 image of 32 x 16 pixels.
 */
void It_Jpeg_Get_RawSize_TestImages_ReturnsMcuBytes( void )
{
#if defined(JPEG)
    const jpeg_ImageConfig_t gray  = ItJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, IT_JPEG_SIDE );
    const jpeg_ImageConfig_t ycbcr = ItJpeg_Get_Image( JPEG_COLOR_YCBCR, JPEG_SUBSAMPLING_420, IT_JPEG_WIDE );
    jpeg_Size_t              rawSize = 0u;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, ItJpeg_Init( JPEG_XFER_MODE_POLL ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_RawSize( &gray, &rawSize ) );
    TEST_ASSERT_EQUAL_UINT32( IT_JPEG_GRAY_RAW_SIZE, rawSize );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_RawSize( &ycbcr, &rawSize ) );
    TEST_ASSERT_EQUAL_UINT32( IT_JPEG_YCBCR_RAW_SIZE, rawSize );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Deinit() );
#else
    TEST_IGNORE_MESSAGE( IT_JPEG_NO_JPEG_MSG );
#endif /* JPEG */
}

/* ================================== ENCODER =============================== */

/**
 * \brief   The grayscale image is encoded in the poll mode and decoded again.
 *
 * \par Expected results
 * - The stream starts with the SOI marker, has a baseline frame of 16 x 16 pixels with one component and ends with the
 *   EOI marker (followed by at most the padding to a whole word); the decoded raw data differ from the original by at
 *   most the tolerance, the parsed header and the estimated quality match.
 */
void It_Jpeg_Set_EncodeStart_GrayscalePoll_RoundTripMatches( void )
{
#if defined(JPEG)
    const jpeg_ImageConfig_t image = ItJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, IT_JPEG_SIDE );

    ItJpeg_Run_RoundTrip( JPEG_XFER_MODE_POLL, &image, IT_JPEG_COMPONENTS_GRAY );
#else
    TEST_IGNORE_MESSAGE( IT_JPEG_NO_JPEG_MSG );
#endif /* JPEG */
}


/**
 * \brief   The grayscale image is encoded in the interrupt mode and decoded again.
 *
 * \par Expected results
 * - The same as in the poll mode.
 */
void It_Jpeg_Set_EncodeStart_GrayscaleIsr_RoundTripMatches( void )
{
#if defined(JPEG)
    const jpeg_ImageConfig_t image = ItJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, IT_JPEG_SIDE );

    ItJpeg_Run_RoundTrip( JPEG_XFER_MODE_ISR, &image, IT_JPEG_COMPONENTS_GRAY );
#else
    TEST_IGNORE_MESSAGE( IT_JPEG_NO_JPEG_MSG );
#endif /* JPEG */
}


/**
 * \brief   The grayscale image is encoded in the DMA mode and decoded again.
 *
 * \par Expected results
 * - The same as in the poll mode, the streams are moved by two Gpdma channels.
 */
void It_Jpeg_Set_EncodeStart_GrayscaleDma_RoundTripMatches( void )
{
#if defined(JPEG)
    const jpeg_ImageConfig_t image = ItJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, IT_JPEG_SIDE );

    ItJpeg_Run_RoundTrip( JPEG_XFER_MODE_DMA, &image, IT_JPEG_COMPONENTS_GRAY );
#else
    TEST_IGNORE_MESSAGE( IT_JPEG_NO_JPEG_MSG );
#endif /* JPEG */
}


/**
 * \brief   The YCbCr 4:2:0 image is encoded in the poll mode and decoded again.
 *
 * \par Expected results
 * - A baseline frame of 32 x 16 pixels with three components; the decoded raw data, the color space (YCbCr), the
 *   subsampling (4:2:0), the count of the MCUs and the raw size of the parsed header match.
 */
void It_Jpeg_Set_EncodeStart_YCbCrPoll_RoundTripMatches( void )
{
#if defined(JPEG)
    const jpeg_ImageConfig_t image = ItJpeg_Get_Image( JPEG_COLOR_YCBCR, JPEG_SUBSAMPLING_420, IT_JPEG_WIDE );

    ItJpeg_Run_RoundTrip( JPEG_XFER_MODE_POLL, &image, IT_JPEG_COMPONENTS_YCBCR );
#else
    TEST_IGNORE_MESSAGE( IT_JPEG_NO_JPEG_MSG );
#endif /* JPEG */
}


/**
 * \brief   The YCbCr 4:2:0 image is encoded in the DMA mode and decoded again.
 *
 * \par Expected results
 * - The same as in the poll mode.
 */
void It_Jpeg_Set_EncodeStart_YCbCrDma_RoundTripMatches( void )
{
#if defined(JPEG)
    const jpeg_ImageConfig_t image = ItJpeg_Get_Image( JPEG_COLOR_YCBCR, JPEG_SUBSAMPLING_420, IT_JPEG_WIDE );

    ItJpeg_Run_RoundTrip( JPEG_XFER_MODE_DMA, &image, IT_JPEG_COMPONENTS_YCBCR );
#else
    TEST_IGNORE_MESSAGE( IT_JPEG_NO_JPEG_MSG );
#endif /* JPEG */
}


/**
 * \brief   A paused input stream does not move and continues after the resume.
 *
 * \par Expected results
 * - While the input stream is paused the job does not end (Jpeg_Task() is called many times), after the resume the job
 *   ends with the end of the conversion and the stream has the SOI marker.
 */
void It_Jpeg_Set_Pause_InputStream_ContinuesAfterResume( void )
{
#if defined(JPEG)
    const jpeg_ImageConfig_t image = ItJpeg_Get_Image( JPEG_COLOR_GRAYSCALE, JPEG_SUBSAMPLING_444, IT_JPEG_SIDE );

    ItJpeg_Reset_Results();
    ItJpeg_Fill_Gray();

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, ItJpeg_Init( JPEG_XFER_MODE_POLL ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_EncodeStart( &image ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Pause( JPEG_STREAM_INPUT ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_InputBuffer( itJpeg_Raw, IT_JPEG_GRAY_RAW_SIZE ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_OutputBuffer( itJpeg_Stream, IT_JPEG_STREAM_SIZE ) );

    for( uint32_t callIdx = 0u; IT_JPEG_PAUSE_CALLS > callIdx; callIdx++ )
    {
        Jpeg_Task();
    }

    TEST_ASSERT_EQUAL_UINT32( 0u, itJpeg_EndCnt );
    TEST_ASSERT_EQUAL_UINT32( 0u, itJpeg_InputCnt );

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_Resume( JPEG_STREAM_INPUT ) );
    ItJpeg_Wait_Job( JPEG_XFER_MODE_POLL );

    TEST_ASSERT_EQUAL_UINT32( 0u, itJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL_UINT32( 1u, itJpeg_EndCnt );
    TEST_ASSERT_EQUAL_UINT8( IT_JPEG_MARKER_PREFIX, itJpeg_Stream[ 0u ] );
    TEST_ASSERT_EQUAL_UINT8( IT_JPEG_MARKER_SOI, itJpeg_Stream[ 1u ] );
#else
    TEST_IGNORE_MESSAGE( IT_JPEG_NO_JPEG_MSG );
#endif /* JPEG */
}

/* ================================== DECODER =============================== */

/**
 * \brief   A stream with a progressive frame is rejected by the decoder.
 *
 * \par Expected results
 * - Jpeg_Set_InputBuffer() returns JPEG_REQUEST_ERROR, the error callback reports JPEG_ERROR_UNSUPPORTED, the codec is
 *   idle again.
 */
void It_Jpeg_Set_DecodeStart_ProgressiveFrame_ReportsUnsupported( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    ItJpeg_Reset_Results();

    itJpeg_Small[ 0u ] = IT_JPEG_MARKER_PREFIX;
    itJpeg_Small[ 1u ] = IT_JPEG_MARKER_SOI;
    itJpeg_Small[ IT_JPEG_MARKER_SIZE ]      = IT_JPEG_MARKER_PREFIX;
    itJpeg_Small[ IT_JPEG_MARKER_SIZE + 1u ] = IT_JPEG_MARKER_SOF2;
    itJpeg_Small[ IT_JPEG_MARKER_SIZE + IT_JPEG_SOF_PRECISION ] = IT_JPEG_PRECISION;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, ItJpeg_Init( JPEG_XFER_MODE_POLL ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( itJpeg_Small, IT_JPEG_SMALL_SIZE ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, itJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL( JPEG_ERROR_UNSUPPORTED, itJpeg_LastError );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
#else
    TEST_IGNORE_MESSAGE( IT_JPEG_NO_JPEG_MSG );
#endif /* JPEG */
}


/**
 * \brief   A buffer which does not start with the SOI marker is rejected by the decoder.
 *
 * \par Expected results
 * - Jpeg_Set_InputBuffer() returns JPEG_REQUEST_ERROR, the error callback reports JPEG_ERROR_STREAM, the codec is idle
 *   again.
 */
void It_Jpeg_Set_DecodeStart_NotJpeg_ReportsStreamError( void )
{
#if defined(JPEG)
    jpeg_State_t state = JPEG_STATE_CNT;

    ItJpeg_Reset_Results();

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, ItJpeg_Init( JPEG_XFER_MODE_POLL ) );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Set_DecodeStart() );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_ERROR, Jpeg_Set_InputBuffer( itJpeg_Small, IT_JPEG_SMALL_SIZE ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, itJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL( JPEG_ERROR_STREAM, itJpeg_LastError );
    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_State( &state ) );
    TEST_ASSERT_EQUAL( JPEG_STATE_IDLE, state );
#else
    TEST_IGNORE_MESSAGE( IT_JPEG_NO_JPEG_MSG );
#endif /* JPEG */
}

/* ========================== HELPER FUNCTIONS ============================== */

#if defined(JPEG)

/** Input needed callback of the tests: counts the calls */
static void ItJpeg_InputNeededCallback( void )
{
    itJpeg_InputCnt ++;
}


/** Output ready callback of the tests: counts the calls and keeps the last length */
static void ItJpeg_OutputReadyCallback( jpeg_Size_t length )
{
    itJpeg_OutputLast = length;
    itJpeg_OutputCnt ++;
}


/** End of conversion callback of the tests: keeps the total length and counts the calls */
static void ItJpeg_EndOfConversionCallback( jpeg_Size_t totalLength )
{
    itJpeg_EndLength = totalLength;
    itJpeg_EndCnt ++;
}


/** Error callback of the tests: keeps the error mask and counts the calls */
static void ItJpeg_ErrorCallback( jpeg_Error_t errorMask )
{
    itJpeg_LastError = errorMask;
    itJpeg_ErrorCnt ++;
}


/** Header parsed callback of the tests: keeps a copy of the image parameters */
static void ItJpeg_HeaderParsedCallback( const jpeg_ImageInfo_t * const imageInfo )
{
    itJpeg_HeaderInfo = *imageInfo;
    itJpeg_HeaderCnt ++;
}


/** Clears the counters and the results of the callbacks */
static void ItJpeg_Reset_Results( void )
{
    itJpeg_InputCnt   = 0u;
    itJpeg_OutputCnt  = 0u;
    itJpeg_OutputLast = 0u;
    itJpeg_EndCnt     = 0u;
    itJpeg_EndLength  = 0u;
    itJpeg_ErrorCnt   = 0u;
    itJpeg_LastError  = JPEG_ERROR_NONE;
    itJpeg_HeaderCnt  = 0u;
}


/**
 * \brief Initializes the module in a transfer mode with the callbacks of the tests.
 *
 * \param xferMode [in]: Data transfer mode.
 *
 * \return Result of Jpeg_Init().
 */
static jpeg_RequestState_t ItJpeg_Init( jpeg_XferMode_t xferMode )
{
    jpeg_Config_t       config;
    jpeg_RequestState_t retState = Jpeg_Get_DefaultConfig( &config );

    if( JPEG_REQUEST_OK == retState )
    {
        config.XferMode                = xferMode;
        config.IrqPriority             = IT_JPEG_IRQ_PRIO;
        config.InputNeededCallback     = ItJpeg_InputNeededCallback;
        config.OutputReadyCallback     = ItJpeg_OutputReadyCallback;
        config.EndOfConversionCallback = ItJpeg_EndOfConversionCallback;
        config.ErrorCallback           = ItJpeg_ErrorCallback;
        config.HeaderParsedCallback    = ItJpeg_HeaderParsedCallback;

        retState = Jpeg_Init( &config );
    }
    else
    {
        /* The default configuration is not available */
    }

    return ( retState );
}


/**
 * \brief Waits until the job ends with the end of the conversion or with an error (bounded loop); in the poll mode
 *        the task of the module is called.
 *
 * \param xferMode [in]: Data transfer mode.
 */
static void ItJpeg_Wait_Job( jpeg_XferMode_t xferMode )
{
    for( uint32_t idx = 0u;
         IT_JPEG_WAIT_LIMIT > idx;
         idx++ )
    {
        if( JPEG_XFER_MODE_POLL == xferMode )
        {
            Jpeg_Task();
        }
        else
        {
            /* The interrupts serve the codec */
        }

        const uint32_t finished = itJpeg_EndCnt + itJpeg_ErrorCnt;

        if( 0u != finished )
        {
            break;
        }
        else
        {
            /* The job runs */
        }
    }
}


/**
 * \brief Returns the sample of the gradient of the test images.
 *
 * \param gradient [in]: Sum of the coordinates of the pixel.
 *
 * \return The sample.
 */
static jpeg_Data_t ItJpeg_Get_Gradient( uint32_t gradient )
{
    return ( (jpeg_Data_t)( IT_JPEG_GRADIENT_BASE + ( IT_JPEG_GRADIENT_STEP * gradient ) ) );
}


/**
 * \brief Returns a 16-bit big endian field of the stream.
 *
 * \param pos [in]: Position of the first (high) byte.
 *
 * \return The field.
 */
static uint32_t ItJpeg_Get_Field( jpeg_Size_t pos )
{
    return ( ( (uint32_t)itJpeg_Stream[ pos ] << IT_JPEG_BYTE_BITS ) | itJpeg_Stream[ pos + 1u ] );
}


/** Fills the raw data of the grayscale image of 16 x 16 pixels (4 MCUs of one block, the line order of the MCUs) */
static void ItJpeg_Fill_Gray( void )
{
    for( uint32_t mcuIdx = 0u; IT_JPEG_GRAY_MCUS > mcuIdx; mcuIdx++ )
    {
        const uint32_t mcuX = ( mcuIdx % ( IT_JPEG_SIDE / IT_JPEG_BLOCK_SIDE ) ) * IT_JPEG_BLOCK_SIDE;
        const uint32_t mcuY = ( mcuIdx / ( IT_JPEG_SIDE / IT_JPEG_BLOCK_SIDE ) ) * IT_JPEG_BLOCK_SIDE;

        for( uint32_t pixel = 0u; IT_JPEG_BLOCK_SIZE > pixel; pixel++ )
        {
            const uint32_t gradient = mcuX + ( pixel % IT_JPEG_BLOCK_SIDE ) + mcuY + ( pixel / IT_JPEG_BLOCK_SIDE );

            itJpeg_Raw[ ( mcuIdx * IT_JPEG_BLOCK_SIZE ) + pixel ] = ItJpeg_Get_Gradient( gradient );
        }
    }
}


/**
 * \brief Fills the raw data of the YCbCr 4:2:0 image of 32 x 16 pixels: 2 MCUs of the four luminance blocks (the
 *        gradient) followed by the two chrominance blocks (neutral).
 */
static void ItJpeg_Fill_YCbCr( void )
{
    for( uint32_t mcuIdx = 0u; IT_JPEG_YCBCR_MCUS > mcuIdx; mcuIdx++ )
    {
        for( uint32_t blockIdx = 0u; IT_JPEG_BLOCKS_YCBCR > blockIdx; blockIdx++ )
        {
            const uint32_t blockX = ( ( mcuIdx * IT_JPEG_MCU_BLOCKS_WIDE ) + ( blockIdx % IT_JPEG_MCU_BLOCKS_WIDE ) ) *
                                    IT_JPEG_BLOCK_SIDE;
            const uint32_t blockY = ( blockIdx / IT_JPEG_MCU_BLOCKS_WIDE ) * IT_JPEG_BLOCK_SIDE;

            for( uint32_t pixel = 0u; IT_JPEG_BLOCK_SIZE > pixel; pixel++ )
            {
                const uint32_t column   = blockX + ( pixel % IT_JPEG_BLOCK_SIDE );
                const uint32_t row      = blockY + ( pixel / IT_JPEG_BLOCK_SIDE );
                const uint32_t gradient = column + row;
                const uint32_t blockNo  = ( mcuIdx * IT_JPEG_BLOCKS_YCBCR ) + blockIdx;
                const uint32_t offset   = ( blockNo * IT_JPEG_BLOCK_SIZE ) + pixel;

                if( IT_JPEG_BLOCKS_Y > blockIdx )
                {
                    itJpeg_Raw[ offset ] = ItJpeg_Get_Gradient( gradient );
                }
                else
                {
                    itJpeg_Raw[ offset ] = (jpeg_Data_t)IT_JPEG_CHROMA_NEUTRAL;
                }
            }
        }
    }
}


/**
 * \brief Returns an image of the tests (16 pixels high).
 *
 * \param colorSpace  [in]: Color space.
 * \param subsampling [in]: Chroma subsampling.
 * \param width       [in]: Width in pixels.
 *
 * \return The image with the quality of the round trip tests.
 */
static jpeg_ImageConfig_t ItJpeg_Get_Image( jpeg_ColorSpace_t colorSpace,
                                            jpeg_Subsampling_t subsampling,
                                            jpeg_Dimension_t width )
{
    jpeg_ImageConfig_t image;

    image.Width       = width;
    image.Height      = IT_JPEG_SIDE;
    image.ColorSpace  = colorSpace;
    image.Subsampling = subsampling;
    image.Quality     = IT_JPEG_QUALITY;

    return ( image );
}


/**
 * \brief Encodes the raw image into the stream buffer and waits for the end of the job.
 *
 * \param xferMode [in]: Data transfer mode.
 * \param image    [in]: Image to encode.
 * \param rawSize  [in]: Size of the raw data in bytes.
 *
 * \return JPEG_REQUEST_OK if the requests were accepted.
 */
static jpeg_RequestState_t ItJpeg_Encode( jpeg_XferMode_t xferMode,
                                          const jpeg_ImageConfig_t * const image,
                                          jpeg_Size_t rawSize )
{
    jpeg_RequestState_t retState = ItJpeg_Init( xferMode );

    ItJpeg_Reset_Results();

    if( JPEG_REQUEST_OK == retState )
    {
        retState = Jpeg_Set_EncodeStart( image );
    }
    else
    {
        /* The initialization failed */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        retState = Jpeg_Set_InputBuffer( itJpeg_Raw, rawSize );
    }
    else
    {
        /* The start failed */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        retState = Jpeg_Set_OutputBuffer( itJpeg_Stream, IT_JPEG_STREAM_SIZE );
    }
    else
    {
        /* The input buffer was not accepted */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        ItJpeg_Wait_Job( xferMode );
    }
    else
    {
        /* The output buffer was not accepted */
    }

    return ( retState );
}


/**
 * \brief Decodes the stream buffer into the decoded buffer and waits for the end of the job.
 *
 * The codec is released and initialized again, the results of the encoder job are kept in the stream buffer.
 *
 * \param xferMode   [in]: Data transfer mode.
 * \param streamSize [in]: Size of the stream in bytes.
 *
 * \return JPEG_REQUEST_OK if the requests were accepted.
 */
static jpeg_RequestState_t ItJpeg_Decode( jpeg_XferMode_t xferMode, jpeg_Size_t streamSize )
{
    jpeg_RequestState_t retState = Jpeg_Deinit();

    ItJpeg_Reset_Results();

    if( JPEG_REQUEST_OK == retState )
    {
        retState = ItJpeg_Init( xferMode );
    }
    else
    {
        /* The release failed */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        retState = Jpeg_Set_DecodeStart();
    }
    else
    {
        /* The initialization failed */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        retState = Jpeg_Set_OutputBuffer( itJpeg_Decoded, IT_JPEG_DECODED_SIZE );
    }
    else
    {
        /* The start failed */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        retState = Jpeg_Set_InputBuffer( itJpeg_Stream, streamSize );
    }
    else
    {
        /* The output buffer was not accepted */
    }

    if( JPEG_REQUEST_OK == retState )
    {
        ItJpeg_Wait_Job( xferMode );
    }
    else
    {
        /* The input buffer was not accepted */
    }

    return ( retState );
}


/**
 * \brief Checks the structure of the encoded stream: the SOI marker at the start, the start of frame segment of a
 *        baseline frame (8-bit samples, the size and the count of the components of the image) behind the other
 *        segments, the EOI marker at the end (at most the padding to a whole word behind it).
 *
 * \param width        [in]: Width of the image.
 * \param height       [in]: Height of the image.
 * \param componentCnt [in]: Count of the components of the image.
 */
static void ItJpeg_Assert_Stream( jpeg_Dimension_t width, jpeg_Dimension_t height, jpeg_Size_t componentCnt )
{
    const jpeg_Size_t length  = itJpeg_EndLength;
    jpeg_Size_t       sofPos  = 0u;
    jpeg_Size_t       eoiPos  = 0u;

    TEST_ASSERT_GREATER_THAN_UINT32( IT_JPEG_STREAM_MIN, length );
    TEST_ASSERT_TRUE( IT_JPEG_STREAM_SIZE >= length );
    TEST_ASSERT_EQUAL_UINT8( IT_JPEG_MARKER_PREFIX, itJpeg_Stream[ 0u ] );
    TEST_ASSERT_EQUAL_UINT8( IT_JPEG_MARKER_SOI, itJpeg_Stream[ 1u ] );

    for( jpeg_Size_t idx = 0u; length > ( idx + 1u ); idx++ )
    {
        if( ( IT_JPEG_MARKER_PREFIX == itJpeg_Stream[ idx ] ) &&
            ( IT_JPEG_MARKER_SOF0   == itJpeg_Stream[ idx + 1u ] ) &&
            ( 0u                    == sofPos ) )
        {
            sofPos = idx;
        }
        else
        {
            /* Not the start of frame marker */
        }

        if( ( IT_JPEG_MARKER_PREFIX == itJpeg_Stream[ idx ] ) &&
            ( IT_JPEG_MARKER_EOI    == itJpeg_Stream[ idx + 1u ] ) )
        {
            eoiPos = idx;
        }
        else
        {
            /* Not the end of image marker */
        }
    }

    TEST_ASSERT_GREATER_THAN_UINT32( 0u, sofPos );
    TEST_ASSERT_EQUAL_UINT8( IT_JPEG_PRECISION, itJpeg_Stream[ sofPos + IT_JPEG_SOF_PRECISION ] );
    TEST_ASSERT_EQUAL_UINT32( height, ItJpeg_Get_Field( sofPos + IT_JPEG_SOF_HEIGHT ) );
    TEST_ASSERT_EQUAL_UINT32( width, ItJpeg_Get_Field( sofPos + IT_JPEG_SOF_WIDTH ) );
    TEST_ASSERT_EQUAL_UINT32( componentCnt, itJpeg_Stream[ sofPos + IT_JPEG_SOF_COMPONENTS ] );
    TEST_ASSERT_GREATER_THAN_UINT32( sofPos, eoiPos );
    TEST_ASSERT_TRUE( ( length - ( eoiPos + IT_JPEG_MARKER_SIZE ) ) <= IT_JPEG_PADDING_MAX );
}


/**
 * \brief Compares the decoded raw data with the original ones within the tolerance.
 *
 * \param rawSize [in]: Size of the raw data in bytes.
 */
static void ItJpeg_Assert_Decoded( jpeg_Size_t rawSize )
{
    TEST_ASSERT_EQUAL_UINT32( rawSize, itJpeg_EndLength );

    for( jpeg_Size_t idx = 0u; rawSize > idx; idx++ )
    {
        TEST_ASSERT_INT_WITHIN( IT_JPEG_TOLERANCE, itJpeg_Raw[ idx ], itJpeg_Decoded[ idx ] );
    }
}


/**
 * \brief Encodes the raw image in a transfer mode, checks the stream, decodes it in the same mode and compares the
 *        decoded raw data and the parsed header with the image.
 *
 * \param xferMode     [in]: Data transfer mode.
 * \param image        [in]: Image (the raw data are prepared for the grayscale and the YCbCr 4:2:0 image).
 * \param componentCnt [in]: Count of the components of the image.
 */
static void ItJpeg_Run_RoundTrip( jpeg_XferMode_t xferMode,
                                  const jpeg_ImageConfig_t * const image,
                                  jpeg_Size_t componentCnt )
{
    jpeg_Size_t rawSize = 0u;
    jpeg_Size_t length  = 0u;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, Jpeg_Get_RawSize( image, &rawSize ) );

    if( JPEG_COLOR_GRAYSCALE == image->ColorSpace )
    {
        ItJpeg_Fill_Gray();
    }
    else
    {
        ItJpeg_Fill_YCbCr();
    }

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, ItJpeg_Encode( xferMode, image, rawSize ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, itJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL_UINT32( 1u, itJpeg_EndCnt );
    ItJpeg_Assert_Stream( image->Width, image->Height, componentCnt );
    length = itJpeg_EndLength;

    TEST_ASSERT_EQUAL( JPEG_REQUEST_OK, ItJpeg_Decode( xferMode, length ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, itJpeg_ErrorCnt );
    TEST_ASSERT_EQUAL_UINT32( 1u, itJpeg_EndCnt );
    TEST_ASSERT_EQUAL_UINT32( 1u, itJpeg_HeaderCnt );
    ItJpeg_Assert_Decoded( rawSize );

    TEST_ASSERT_EQUAL_UINT16( image->Width, itJpeg_HeaderInfo.Width );
    TEST_ASSERT_EQUAL_UINT16( image->Height, itJpeg_HeaderInfo.Height );
    TEST_ASSERT_EQUAL( image->ColorSpace, itJpeg_HeaderInfo.ColorSpace );
    TEST_ASSERT_EQUAL( image->Subsampling, itJpeg_HeaderInfo.Subsampling );
    TEST_ASSERT_EQUAL_UINT32( rawSize, itJpeg_HeaderInfo.RawSize );
    TEST_ASSERT_INT_WITHIN( IT_JPEG_QUALITY_TOLERANCE, image->Quality, itJpeg_HeaderInfo.Quality );
}

#endif /* JPEG */

/* =============================== TASKS ==================================== */
