# JPEG Peripheral Driver

This module provides an abstraction layer for the **JPEG codec** (`JPEG`) of STM32 MCUs: the hardware **encoding of the raw image data
into a JPEG stream** and the **decoding of a JPEG stream into the raw minimum coded units (MCU)**, with the generation and the parsing
of the JPEG header, the programmable quantization and Huffman tables, the color spaces (grayscale, YCbCr, RGB, CMYK) and the chroma
subsampling, one clock per pixel and the concurrent input and output streams moved by the DMA. It is a low-level MCAL module: it owns the JPEG hardware, the
application and the other modules never touch the registers.

The public interface (`Jpeg_Port.h`, `Jpeg_Types.h`) is **identical for all supported families**. The implementation
for a family lives in its own branch (`Dev/<Family>` for development, `Releases/<Family>` for the released versions), so
a module or an application written against the interface does not change when the MCU family changes.

This README is the landing page and the **specification of the interface** (master). The family branches contain their
own README with everything that is specific to the family: the instances of the device, their settings and the status of
the implementation.

> **Status: STM32H5 implemented** (branch `Dev/STM32H5`): encoder and decoder in the poll, interrupt and DMA mode, unit tests
> on the 12 STM32H5 device lines, integration tests compiled but not run on hardware. The interface below is the specification,
> the additions and deviations of the STM32H5 implementation are listed in the chapter "STM32H5 implementation".

---

## Intent and purpose

- The codec turns a **camera frame into a compact file** (the capture of a photo, the video as a sequence of the JPEG images) and a **JPEG file into pixels** (the pictures of the user
  interface, the album, the web content) at a few megapixels per second without the CPU. A software decoder on a Cortex-M needs the CPU for hundreds of milliseconds per picture.
- The codec works on the **MCU blocks** (8 x 8 or 16 x 16 pixels in the YCbCr components), not on the scan lines of an ordinary bitmap: the encoder expects the image in
  MCU order, the decoder delivers it in MCU order. The conversion between the MCU order and the line order, and between YCbCr and RGB, is done by a graphics accelerator
  (the Dma2D module) or by the software helper of the middleware. The module owns the codec side: the configuration, the header, the tables, the two streams, the events.
- The user describes the **picture and the quality** - "320 x 240, YCbCr 4:2:0, quality 80" for the encoder; the decoder discovers the image from the header
  (`Jpeg_Get_ImageInfo`: the width, the height, the color space, the subsampling, the quality / tables) and the module configures the codec from it. The quality factor (1 ... 100) is converted
  to the quantization tables (the standard tables scaled as the common libraries do).
- The input and the output are **streams of the unknown length** (the compressed side) and of the fixed length (the raw side): the module hands the DMA the buffers one by one with the
  callbacks "input needed", "output available", "end of the image", "header parsed", and supports the pause and the resume.
- The module hides the differences between the families: the supported color spaces, the FIFOs and the DMA structure differ, the interface stays the same.

What the module is **not**: it does not parse a JPEG file container beyond the header of the stream (JFIF/EXIF are parsed by the middleware), it does not convert the color space or
the MCU order and it does not scale the picture.

---

## Concept

| Item | Meaning |
|---|---|
| **Mode** | `JPEG_MODE_ENCODE`, `JPEG_MODE_DECODE`. |
| **Image** (`jpeg_ImageConfig_t`) | The width, the height, the color space (`JPEG_COLOR_GRAYSCALE`, `_YCBCR`, `_RGB`, `_CMYK`), the chroma subsampling (`JPEG_SUBSAMPLING_444`, `_422`, `_420`), the quality factor. |
| **Tables** | The quantization tables (up to 4) and the Huffman tables (2 DC and 2 AC): the standard tables are loaded by default; custom tables can be set. |
| **Header** | The generation (the encoder writes the SOI, DQT, SOF, DHT, SOS markers in front of the data) and the parsing (the decoder reads the markers of the stream) can be switched off for the headerless streams. |
| **Streams** | The input (`Jpeg_Set_InputBuffer`) and the output (`Jpeg_Set_OutputBuffer`) with the DMA; the buffers are given when the callbacks ask for them. |

Rules:

1. `Jpeg_Init` configures the clock, the DMA and the interrupt and leaves the codec idle. `Jpeg_Set_EncodeStart( imageConfig )` or `Jpeg_Set_DecodeStart()` starts a job.
2. The **encoder** needs the image in the MCU order in the input buffers and produces the JPEG stream into the output buffers; the end of the image is signalled when
   the input size computed from the image configuration was consumed and the output was flushed (`EndOfConversionCallback`, with the length of the compressed data).
3. The **decoder** first parses the header: the `HeaderParsedCallback` reports the image information, then the decoder produces the MCUs into the output buffers; a header
   with a feature the hardware does not support (progressive coding, 12-bit samples, arithmetic coding) is rejected with `JPEG_ERROR_UNSUPPORTED`.
4. When the codec needs more input (`InputNeededCallback`) or has the output ready (`OutputReadyCallback( length )`) the application gives the next buffer or consumes the
   data; `Jpeg_Set_Pause` / `Jpeg_Set_Resume` stop and continue the DMA of the stream.
5. A job is ended by the end of the image, by `Jpeg_Set_Abort` (the codec and the DMA are reset) or by an error (a corrupt stream, the DMA error); the module returns to idle.
6. All parameters are checked before a register is written; a job request while one runs returns an error.

---

## Supported families

Every family gets its own story, its own tasks and its own branches `Dev/<Family>` and `Releases/<Family>` cut from this
master.

| Family | Branch | Codec | Status |
|---|---|---|---|
| STM32H5 | `Dev/STM32H5` | `JPEG` on the STM32H5E / H5F lines: encoder and decoder, one clock per pixel, grayscale / YCbCr / RGB / CMYK, 8-bit components, header generator and parser, four quantization tables, two AC and two DC Huffman tables, concurrent input and output streams | implemented, unit tests on 12 device lines, integration tests compiled (not run on hardware) |
| STM32H7 | `Dev/STM32H7` | `JPEG` on the lines that have it | planned |
| STM32U5 | `Dev/STM32U5` | `JPEG` on the lines with the graphics | planned |
| STM32U3 | `Dev/STM32U3` | device dependent | planned |
| STM32F4, STM32F7, STM32G4, STM32L4 | `Dev/STM32F4` ... | no JPEG codec on the device - the module is a stub that returns `JPEG_REQUEST_ERROR` | planned |

A function without a meaning in a family stays in the interface, returns `JPEG_REQUEST_ERROR` and suppresses the unused
parameters (for example a color space the hardware does not support).

---

## Public interface in short (proposal)

| Group | Functions |
|---|---|
| Module management | `Jpeg_Get_ModuleVersion`, `Jpeg_Init`, `Jpeg_Deinit`, `Jpeg_Task`, `Jpeg_Get_DefaultConfig` |
| Jobs | `Jpeg_Set_EncodeStart`, `Jpeg_Set_DecodeStart`, `Jpeg_Set_Abort`, `Jpeg_Set_Pause`, `Jpeg_Set_Resume`, `Jpeg_Get_State` |
| Streams | `Jpeg_Set_InputBuffer`, `Jpeg_Set_OutputBuffer`, `Jpeg_Get_OutputLength` |
| Image and tables | `Jpeg_Get_ImageInfo`, `Jpeg_Set_Quality`, `Jpeg_Set_QuantizationTable`, `Jpeg_Set_HuffmanTable`, `Jpeg_Set_HeaderMode` |
| Interrupts | `Jpeg_Set_IrqPriority` / `Get`, the callbacks are a part of the configuration |

```c
jpeg_ModuleVersion_t Jpeg_Get_ModuleVersion     ( void );

jpeg_RequestState_t  Jpeg_Init                  ( const jpeg_Config_t * const jpegConfig );
jpeg_RequestState_t  Jpeg_Deinit                ( void );
void                 Jpeg_Task                  ( void );
jpeg_RequestState_t  Jpeg_Get_DefaultConfig     ( jpeg_Config_t * const jpegConfig );

jpeg_RequestState_t  Jpeg_Set_EncodeStart       ( const jpeg_ImageConfig_t * const imageConfig );
jpeg_RequestState_t  Jpeg_Set_DecodeStart       ( void );
jpeg_RequestState_t  Jpeg_Set_Abort             ( void );
jpeg_RequestState_t  Jpeg_Set_Pause             ( jpeg_Stream_t stream );
jpeg_RequestState_t  Jpeg_Set_Resume            ( jpeg_Stream_t stream );

jpeg_RequestState_t  Jpeg_Set_InputBuffer       ( const jpeg_Data_t * const buffer, jpeg_Size_t size );
jpeg_RequestState_t  Jpeg_Set_OutputBuffer      ( jpeg_Data_t * const buffer, jpeg_Size_t size );
jpeg_RequestState_t  Jpeg_Get_ImageInfo         ( jpeg_ImageInfo_t * const imageInfo );
jpeg_RequestState_t  Jpeg_Set_Quality           ( jpeg_Quality_t quality );
```

Every function returns `JPEG_REQUEST_OK` / `JPEG_REQUEST_ERROR` and checks its parameters and the state of the module
before it touches a register.

| Type | Meaning |
|---|---|
| `jpeg_Config_t` | DMA channels (in, out) and priorities, the standard tables on / off, the header mode, callbacks, interrupt priority |
| `jpeg_ImageConfig_t`, `jpeg_ImageInfo_t` | Width, height, color space, subsampling, quality, the size of the raw data |
| `jpeg_Stream_t` | `JPEG_STREAM_INPUT`, `JPEG_STREAM_OUTPUT` |
| Callbacks | `HeaderParsedCallback( imageInfo )`, `InputNeededCallback`, `OutputReadyCallback( length )`, `EndOfConversionCallback( compressedLength )`, `ErrorCallback( errorMask )` |

---

## Usage

### Encoding a camera frame

```c
#include "Jpeg_Port.h"

static jpeg_Data_t rawMcu[ 2u * 4096u ];             /* the frame in the MCU order, two halves */
static jpeg_Data_t jpegData[ 32768u ];

jpeg_Config_t      jpegConfig;
jpeg_ImageConfig_t photo =
{
    .Width = 320u, .Height = 240u, .ColorSpace = JPEG_COLOR_YCBCR, .Subsampling = JPEG_SUBSAMPLING_420, .Quality = 80u,
};

(void)Jpeg_Get_DefaultConfig( &jpegConfig );
(void)Jpeg_Init( &jpegConfig );

(void)Jpeg_Set_EncodeStart( &photo );
(void)Jpeg_Set_InputBuffer( rawMcu, sizeof( rawMcu ) );
(void)Jpeg_Set_OutputBuffer( jpegData, sizeof( jpegData ) );
/* the InputNeededCallback gives the next part of the frame, EndOfConversionCallback( length ) tells the size of the file in jpegData */
```

### Decoding a picture

```c
(void)Jpeg_Set_DecodeStart();
(void)Jpeg_Set_InputBuffer( fileData, fileSize );
/* HeaderParsedCallback( imageInfo ): allocate the output for imageInfo, then Jpeg_Set_OutputBuffer(); the MCUs follow in OutputReadyCallback */
```

---

## Behaviour and limits

- **Baseline only.** The hardware does the baseline sequential JPEG with 8-bit components; the progressive and the arithmetic coded files are rejected.
- **MCU order.** The raw side is in the MCU blocks; the conversion to the line order and to RGB is the task of the graphics accelerator (Dma2D) or the middleware.
- **Output size.** The size of the compressed output is not known in advance; the buffer must be large enough for the worst case or the module is given the
  next buffer in the callback; an output overflow is an error.
- **Cache and DMA.** The buffers must be visible to the DMA (the non-cacheable memory or the cache maintenance by the application).
- **Not part of the first version:** the progressive JPEG, the restart-marker based parallelism, the image scaling during the decode, the EXIF handling.

---

## Dependencies

- **Rcc** - peripheral clock and reset
- **Nvic** - interrupt
- **Gpdma** - DMA transfers of the input and the output streams
- **Cache** - the data cache maintenance of the buffers on the families that have it
- **RAL (Register Abstraction Layer)** - used internally to access the registers

All mandatory RCC, NVIC and DMA configurations are handled internally.

---

## STM32H5 implementation

### Availability

The `JPEG` codec exists on the STM32H5E4, H5E5, H5F4 and H5F5 devices only. On the other eight lines (H503, H523, H533, H543, H553,
H562, H563, H573) every public function checks the CMSIS macro `JPEG` and returns `JPEG_REQUEST_ERROR`
(`Jpeg_Get_ModuleVersion` returns the version, `Jpeg_Task` does nothing). There is no errata sheet for the STM32H5E and
STM32H5F devices in the reference material, so no device errata is taken into account (not verified).

### Interface additions and deviations

| Item | Specification | STM32H5 implementation |
|---|---|---|
| `Jpeg_Get_RawSize( imageConfig, rawSize )` | not listed | size of the raw data of an image (MCUs of 8 x 8 blocks, one byte per sample) |
| `Jpeg_Get_Quality` | not listed | counterpart of `Jpeg_Set_Quality`; `jpeg_ImageConfig_t::Quality` = `JPEG_QUALITY_MODULE` selects the module quality |
| `Jpeg_Set_QuantizationTable( tableId, table )` | tables | four tables in the row order of the block, values 1 to 255; the quality scales every table (quality 50 leaves it unchanged) |
| `Jpeg_Set_HuffmanTable( tableId, table )` | tables | `DC_0`, `DC_1`, `AC_0`, `AC_1` as the code counts and the symbols of a DHT segment; the table is checked (codes fit their lengths, symbols are valid) |
| `Jpeg_Set_HeaderMode` | header on / off | applies to the encoder; the decoder always parses the header (the Huffman and quantization tables of the stream are loaded by the parser) |
| mode `JPEG_MODE_ENCODE / DECODE` | concept table | part of `jpeg_State_t` (`JPEG_STATE_ENCODING`, `JPEG_STATE_DECODING`) |
| `ErrorCallback` | error mask | `JPEG_ERROR_UNSUPPORTED`, `JPEG_ERROR_STREAM`, `JPEG_ERROR_DMA`; the module returns to idle |
| `jpeg_Config_t` | DMA channels, priorities, tables, header mode, callbacks, priority | plus `XferMode` (poll, interrupt, DMA) and the DMA controller |

### Encoder

- The raw data are the MCUs in the line order of the MCUs: 8 x 8 pixels for grayscale, RGB, CMYK and YCbCr 4:4:4, 16 x 8 pixels for
  YCbCr 4:2:2 and 16 x 16 pixels for YCbCr 4:2:0; every component of an MCU is a group of 8 x 8 blocks of 64 bytes (1, 3, 4 or 6
  blocks per MCU). The image is padded to whole MCUs. The encoder ends by itself after the count of the MCUs of the image.
- Quantization tables: grayscale uses table 0, YCbCr tables 0 / 1 / 1, RGB tables 0 / 1 / 2, CMYK tables 0 / 1 / 2 / 3. The standard
  tables are the sample tables of ITU-T T.81 Annex K (luminance for the tables 0 and 3, chrominance for 1 and 2), scaled by the quality
  factor of the common libraries (5000 / quality below 50, 200 - 2 * quality from 50, limited to 1 to 255).
- Huffman tables: component 0 uses the tables 0, the other components of YCbCr the tables 1; RGB and CMYK use the tables 0 only.
  The encoder memory receives the code length and the low eight bits of the code of every symbol (the layout of the reference manual),
  the DHT memory receives the four tables for the generated header. A table that is not set is replaced by the sample table.
- The size of the JPEG stream is reported in whole 32-bit words: the last bytes after the EOI marker are padding.

### Decoder

- The first input buffer is scanned for the start of frame segment: the SOI marker is required, a frame of the type SOF0 / SOF1 with
  8-bit samples is accepted, every other frame (progressive, lossless, hierarchical, arithmetic coded, 12-bit) ends the job with
  `JPEG_ERROR_UNSUPPORTED`, a buffer which does not start with a JPEG stream with `JPEG_ERROR_STREAM`. A buffer which ends before the
  start of frame segment cannot be judged and is accepted, so the first buffer should hold the whole header.
- After the header is parsed the image parameters are read from the registers: the count of the components gives the color space (1
  grayscale, 3 YCbCr, 4 CMYK; YCbCr and RGB cannot be told apart), the sampling factors of the first component the subsampling
  (4:4:4, 4:2:2, 4:2:0, others are reported as unsupported). The quality is estimated from the parsed quantization table 0.
- A job that ends before its header was parsed ends with `JPEG_ERROR_STREAM`. The hardware reports no error for a damaged entropy coded
  segment; the application checks the count of the produced bytes against the raw size of the image.
- A headerless stream cannot be decoded: the tables of the decoder (Huffman minimum, base and symbol memories) are produced by the header parser.

### Streams and transfer modes

- Input and output buffers are served one at a time. The sizes of the output buffers are multiples of 4 bytes, an input buffer may end
  with a partial word only at the end of the stream (the word is padded with zeros). A full output buffer and an exhausted input buffer
  stall the FIFO of the codec until the next buffer is given.
- Poll: `Jpeg_Task`. Interrupt: the not full interrupt of the input FIFO and the not empty interrupt of the output FIFO are enabled only while
  a buffer waits. DMA: two Gpdma channels with one 32-bit word per request, buffers aligned to 4 bytes, at most 65532 bytes; with the data
  cache enabled the buffers are aligned to 16 bytes and the Cache module cleans the input and flushes / invalidates the output buffers.
  The Gpdma interrupts have to have a higher priority than the JPEG interrupt. At the end of the conversion the channels are stopped, the bytes
  moved by the output channel follow from its remaining block size and the rest of the FIFO is read by the CPU.
- The Gpdma module has no request items for the JPEG codec; the module uses the request numbers 140 (input FIFO, `jpeg_rx_dma`) and 141
  (output FIFO, `jpeg_tx_dma`) of the reference manual. The assignment follows the table of the requests with peripheral early termination
  and is not verified on hardware.

---

## Testing

- **Unit tests** (host, Unity + CMock + RegMem): the quality factor to the quantization table conversion, the image size to the raw data size calculation for every
  subsampling, the header mode and the table loading, the job state machine (encode, decode, pause, resume, abort, the errors), the buffer hand-over with the callbacks, the rejected
  streams.
- **Integration tests** (target, probe-rs): the encode of a test picture compared with a software encoder (the decoded result within the tolerance), the decode of a set of the
  reference files, the corrupt stream, the throughput in pixels per second.

---

## Interface conventions

The module follows the common rules of the MCAL layer, so that it behaves like the other modules:

- **Lists instead of free members.** Everything that the hardware restricts (the pins, the kernel clock sources, the DMA
  channels, the triggers and the other selectable sources) is a `typedef enum` list with exactly the items that are valid
  for the device line; a pin is an item `JPEG_<SIGNAL>_PIN_<INSTANCE>_P<port><pin>` and there is an `*_UNUSED` item. A choice
  that is not in the list cannot be written.
- **Own peripheral only.** The module calls only the registers / LL functions of its own peripheral. The clocks, the
  interrupts, the pins, the DMA and the EXTI lines go through the API of the Rcc, Nvic, Gpio, Gpdma and Exti modules; a
  missing function is added to that module, it is not bypassed.
- **Init verifies itself.** `Jpeg_Init` checks the result at the end; when it is not as required, the module is
  deinitialized and an error is returned. The waiting for the hardware is a loop on the register state with a timeout,
  never a blind delay.
- **Transfer modes.** Where the module moves data, the POLL (served by `Jpeg_Task`), ISR and DMA modes have the same
  configuration structure, the same request and the same callbacks (error, half transfer, transfer complete).
- **Flags and states are enums of the module** (`jpeg_FunctionState_t`, `jpeg_FlagState_t`), the limits and the constants are
  named, there are no magic numbers in the interface.
- **Unsupported functions stay.** A function without a meaning in a family keeps its signature, suppresses the unused
  parameters and returns `JPEG_REQUEST_ERROR`.
- **Documentation.** Every function and type has a Doxygen description, the main header has the `\defgroup`.

---

## Repository layout and branches

The master holds this README and the license. The family branches (`Dev/<Family>`, `Releases/<Family>`) hold:

| Path | Content |
|---|---|
| `README.md` | README of the family: the instances of the device, specifics, errata, status |
| `Jpeg_Port.h`, `Jpeg_Types.h` | Public interface (identical in all families) |
| `Jpeg*.c`, `Jpeg.h` | Implementation of the family |
| `CMakeLists.txt` | Library `Jpeg_Lib` of the EmBi platform |
| `Tests/UnitTests` | Unit tests (Unity + CMock + RegMem) |
| `Tests/IntegrationTests` | Integration tests on the target (probe-rs) |

A change of the public interface is made in all families.

---

## CMake integration

```cmake
target_link_libraries( User_Lib PRIVATE Jpeg_Lib )
```

1. Link `Jpeg_Lib` to your CMake library.
2. Include `Jpeg_Port.h` in your project.
3. Configure the module as needed for your hardware.

---

## License

This project is licensed under the **Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)**.

You are free to use, modify, and share this work for **non-commercial purposes**, provided appropriate credit is given.

See [LICENSE.md](LICENSE.md) for full terms or visit [creativecommons.org/licenses/by-nc/4.0](https://creativecommons.org/licenses/by-nc/4.0/).

---

## Authors

- **Mr.Nobody** - [embedbits.com](https://embedbits.com)

Contributions are welcome! Please open a pull request.
