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

> **Status: design.** No implementation exists yet, the interface below is a proposal. Stories and branches follow.

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
| STM32H5 | `Dev/STM32H5` | `JPEG` on the STM32H5E / H5F lines: encoder and decoder, one clock per pixel, grayscale / YCbCr / RGB / CMYK, 8-bit components, header generator and parser, four quantization tables, two AC and two DC Huffman tables, concurrent input and output streams | planned (first) |
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
