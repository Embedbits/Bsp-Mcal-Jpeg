/**
 * \author Mr.Nobody
 * \file Jpeg.h
 * \ingroup Jpeg
 * \brief Jpeg module common functionality header file.
 *
 * This file contains the common functionality used internally by the module,
 * and shall provide interface between the module and the application.
 *
 */

#ifndef JPEG_JPEG_H
#define JPEG_JPEG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================= INCLUDES =================================== */
#include "Jpeg_Types.h"                     /* Module types definitions       */
/* ============================= TYPEDEFS =================================== */

/** \brief Geometry of the MCU (minimum coded unit) of a color space and a chroma subsampling. The MCU is a group of
 *         8 x 8 blocks: the raw data of the codec are the MCUs one after another, in the line order of the MCUs. */
typedef struct
{
    jpeg_Count_t McuWidth;  /**< Width of the MCU in pixels                                         */
    jpeg_Count_t McuHeight; /**< Height of the MCU in pixels                                        */
    jpeg_Count_t BlockCnt;  /**< Count of the 8 x 8 blocks of the MCU (all components)              */
}   jpeg_McuLayout_t;

/* ========================= SYMBOLIC CONSTANTS ============================= */

/* ========================= EXPORTED MACROS ================================ */

/* ========================= EXPORTED VARIABLES ============================= */

/* ======================== EXPORTED FUNCTIONS ============================== */

#ifdef __cplusplus
}
#endif

#endif /* JPEG_JPEG_H */
