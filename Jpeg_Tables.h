/**
 * \author Mr.Nobody
 * \file Jpeg_Tables.h
 * \ingroup Jpeg
 * \brief Jpeg module standard tables header file.
 *
 * The standard tables of the JPEG specification used by the module. The header is private to the module.
 *
 */

#ifndef JPEG_JPEG_TABLES_H
#define JPEG_JPEG_TABLES_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================= INCLUDES =================================== */
#include "Jpeg_Types.h"                     /* Module types definitions       */
/* ============================= TYPEDEFS =================================== */

/** \brief Index of the standard quantization tables */
typedef enum
{
    JPEG_STD_QUANT_LUM = 0u, /**< Sample luminance table                  */
    JPEG_STD_QUANT_CHROM,    /**< Sample chrominance table                */
    JPEG_STD_QUANT_CNT       /**< Count of the standard tables            */
}   jpeg_StdQuant_t;

/* ========================= SYMBOLIC CONSTANTS ============================= */

/* ========================= EXPORTED MACROS ================================ */

/* ========================= EXPORTED VARIABLES ============================= */

/** Zigzag order of the coefficients of an 8 x 8 block: the natural index of the coefficient at the zigzag position */
extern const jpeg_Data_t jpeg_ZigzagOrder[ JPEG_QUANT_TABLE_SIZE ];

/** Sample quantization tables of the specification (luminance, chrominance) */
extern const jpeg_QuantTable_t jpeg_StdQuantTable[ JPEG_STD_QUANT_CNT ];

/** Sample Huffman tables of the specification, indexed by \ref jpeg_HuffmanId_t */
extern const jpeg_HuffmanTable_t jpeg_StdHuffmanTable[ JPEG_HUFFMAN_CNT ];

/* ======================== EXPORTED FUNCTIONS ============================== */

#ifdef __cplusplus
}
#endif

#endif /* JPEG_JPEG_TABLES_H */
