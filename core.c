/*!
 * @file core.c
 *
 * Part of Adafruit's Protomatter library for HUB75-style RGB LED matrices.
 *
 * Adafruit invests time and resources providing this open source code,
 * please support Adafruit and open-source hardware by purchasing
 * products from Adafruit!
 *
 * Written by Phil "Paint Your Dragon" Burgess and Jeff Epler for
 * Adafruit Industries, with contributions from the open source community.
 *
 * BSD license, all text here must be included in any redistribution.
 *
 */

// Device- and environment-neutral core matrix-driving functionality.
// See notes near top of arch.h regarding assumptions of hardware
// "common ground." If you find yourself doing an "#ifdef ARDUINO" or
// "#ifdef _SAMD21_" in this file, STOP. Idea is that the code in this
// file is neutral and portable (within aforementioned assumptions).
// Nonportable elements should appear in arch.h. If arch.h functionality
// is lacking, extend it there, do not go making device- or environment-
// specific cases within this file.

// Function names are intentionally a little obtuse, idea is that one writes
// a more sensible wrapper around this for specific environments (e.g. the
// Arduino stuff in Adafruit_Protomatter.cpp). The "_PM_" prefix on most
// things hopefully makes function and variable name collisions much less
// likely with one's own code.

#include "core.h" // enums and structs
#include "arch.h" // Do NOT include this in any other source files
#include <stddef.h>
#include <string.h>

// Overall matrix refresh rate (frames/second) is a function of matrix width
// and chain length, number of address lines, number of bit planes, CPU speed
// and whether or not a GPIO toggle register is available. There is no "this
// will run at X-frames-per-second" constant figure. You typically just have
// to try it out and perhaps trade off some bit planes for refresh rate until
// the image looks good and stable. Anything over 100 Hz is usually passable,
// around 250 Hz is where things firm up. And while this could proceed higher
// in some situations, the tradeoff is that faster rates use progressively
// more CPU time (because it's timer interrupt based and not using DMA or
// special peripherals). So a throttle is set here, an approximate maximum
// frame rate which the software will attempt to avoid exceeding (but may
// refresh slower than this, and in many cases will...just need to set an
// upper limit to avoid excessive CPU load). An incredibly long comment block
// for a single constant, thank you for coming to my TED talk!
#define _PM_MAX_REFRESH_HZ 250 ///< Max matrix refresh rate

// Time (in microseconds) to pause following any change in address lines
// (individually or collectively). Some matrices respond slowly there...
// must pause on change for matrix to catch up. Defined here (rather than
// arch.h) because it's not architecture-specific.
#define _PM_ROW_DELAY 8 ///< Delay time between row address line changes (ms)

// Gamma correction is provided if the number of bitplanes requested
// exceeds 6 bits (the limit of RGB565 color fidelity). Gamma correction
// makes intermediate shades more perceptually linear, but incurs more RAM
// use and processor load.
#define _PM_GAMMA 2.6 ///< Exponent for pow() function in gamma setting

// These are the lowest-level functions for issing data to matrices.
// There are three versions because it depends on how the six RGB data bits
// (and clock bit) are arranged within a 32-bit PORT register. If all six
// (seven) fit within one byte or word of the PORT, the library's memory
// use (and corresponding data-issuing function) change. This will also have
// an impact on parallel chains in the future, where the number of concurrent
// RGB data bits isn't always six, but some multiple thereof (i.e. up to five
// parallel outputs -- 30 RGB bits + clock -- on a 32-bit PORT, though that's
// largely hypothetical as the chance of finding a PORT with that many bits
// exposed and NOT interfering with other peripherals on a board is highly
// improbable. But I could see four happening, maybe on a Grand Central or
// other kitchen-sink board.
static void blast_byte(Protomatter_core *core, uint8_t *data);
static void blast_word(Protomatter_core *core, uint16_t *data);
static void blast_long(Protomatter_core *core, uint32_t *data);

#define _PM_clearReg(x)                                                        \
  (*(volatile _PM_PORT_TYPE *)((x).clearReg) =                                 \
       ((x).bit)) ///< Clear non-RGB-data-or-clock control line (_PM_pin type)
#define _PM_setReg(x)                                                          \
  (*(volatile _PM_PORT_TYPE *)((x).setReg) =                                   \
       ((x).bit)) ///< Set non-RGB-data-or-clock control line (_PM_pin type)

// Validate and populate vital elements of core structure.
// Does NOT allocate core struct -- calling function must provide that.
// (In the Arduino C++ library, it’s part of the Protomatter class.)
ProtomatterStatus _PM_init(Protomatter_core *core, uint16_t bitWidth,
                           uint8_t bitDepth, uint8_t rgbCount, uint8_t *rgbList,
                           uint8_t addrCount, uint8_t *addrList,
                           uint8_t clockPin, uint8_t latchPin, uint8_t oePin,
                           bool doubleBuffer, void *timer) {
  if (!core)
    return PROTOMATTER_ERR_ARG;

  if (rgbCount > 5)
    rgbCount = 5; // Max 5 in parallel (32-bit PORT)
  if (addrCount > 5)
    addrCount = 5; // Max 5 address lines (A-E)

    // bitDepth is NOT constrained here, handle in calling function
    // (varies with implementation, e.g. GFX lib is max 6 bitplanes,
    // but might be more or less elsewhere, or if adding gamma correction).

#if defined(_PM_TIMER_DEFAULT)
  // If NULL timer was passed in (the default case for the constructor),
  // use default value from arch.h. For example, in the Arduino case it's
  // tied to TC4 specifically.
  if (timer == NULL)
    timer = _PM_TIMER_DEFAULT;
#else
  if (timer == NULL)
    return PROTOMATTER_ERR_ARG;
#endif

  core->timer = timer;
  core->width = bitWidth; // Total matrix chain length in bits
  core->numPlanes = bitDepth;
  core->parallel = rgbCount;
  core->numAddressLines = addrCount;
  core->clockPin = clockPin;
  core->latch.pin = latchPin;
  core->oe.pin = oePin;
  core->doubleBuffer = doubleBuffer;
  core->addr = NULL;
  core->screenData = NULL;

  // Make a copy of the rgbList and addrList tables in case they're
  // passed from local vars on the stack or some other non-persistent
  // source. screenData is NOT allocated here because data size (byte,
  // word, long) is not known until the begin function evaluates all
  // the pin bitmasks.

  rgbCount *= 6; // Convert parallel count to pin count
  if ((core->rgbPins = (uint8_t *)_PM_ALLOCATOR(rgbCount * sizeof(uint8_t)))) {
    if ((core->addr = (_PM_pin *)_PM_ALLOCATOR(addrCount * sizeof(_PM_pin)))) {
      memcpy(core->rgbPins, rgbList, rgbCount * sizeof(uint8_t));
      for (uint8_t i = 0; i < addrCount; i++) {
        core->addr[i].pin = addrList[i];
      }
      return PROTOMATTER_OK;
    }
    _PM_FREE(core->rgbPins);
    core->rgbPins = NULL;
  }
  return PROTOMATTER_ERR_MALLOC;
}

// Allocate display buffers and populate additional elements.
ProtomatterStatus _PM_begin(Protomatter_core *core) {
  if (!core) {
    return PROTOMATTER_ERR_ARG;
  }

  if (!core->rgbPins) { // NULL if copy failed to allocate
    return PROTOMATTER_ERR_MALLOC;
  }

  // Verify that rgbPins and clockPin are all on the same PORT. If not,
  // return an error. Pin list is not freed; please call dealloc function.
  // Also get bitmask of which bits within 32-bit PORT register are
  // referenced.
  uint8_t *port = (uint8_t *)_PM_portOutRegister(core->clockPin);
#if defined(_PM_portToggleRegister)
  // If a bit-toggle register is present, the clock pin is included
  // in determining which bytes of the PORT register are used (and thus
  // the data storage efficiency).
  uint32_t bitMask = _PM_portBitMask(core->clockPin);
#else
  // If no bit-toggle register, clock pin can be on any bit, doesn't
  // affect storage efficiency.
  uint32_t bitMask = 0;
#endif

  for (uint8_t i = 0; i < core->parallel * 6; i++) {
    uint8_t *p2 = (uint8_t *)_PM_portOutRegister(core->rgbPins[i]);
    if (p2 != port) {
      return PROTOMATTER_ERR_PINS;
    }
    bitMask |= _PM_portBitMask(core->rgbPins[i]);
  }

  // RGB + clock are on same port, we can proceed...

  // Determine data type for internal representation. If all the data
  // bitmasks (and possibly clock bitmask, depending whether toggle-bits
  // register is present) are in the same byte, this can be stored more
  // compact than if they're spread across a word or long.
  uint8_t byteMask = 0;
  if (bitMask & 0xFF000000)
    byteMask |= 0b1000;
  if (bitMask & 0x00FF0000)
    byteMask |= 0b0100;
  if (bitMask & 0x0000FF00)
    byteMask |= 0b0010;
  if (bitMask & 0x000000FF)
    byteMask |= 0b0001;
  switch (byteMask) {
  case 0b0001: // If all PORT bits are in the same byte...
  case 0b0010:
  case 0b0100:
  case 0b1000:
    core->bytesPerElement = 1; // Use 8-bit PORT accesses.
    break;
  case 0b0011: // If all PORT bits in upper/lower word...
  case 0b1100:
    core->bytesPerElement = 2; // Use 16-bit PORT accesses.
    // Although some devices might tolerate unaligned 16-bit accesses
    // ('middle' word of 32-bit PORT), that is NOT handled here.
    // It's a portability liability.
    break;
  default:                     // Any other situation...
    core->bytesPerElement = 4; // Use 32-bit PORT accesses.
    break;
  }

  // Planning for screen data allocation...
  core->numRowPairs = 1 << core->numAddressLines;
  uint8_t chunks = (core->width + (_PM_chunkSize - 1)) / _PM_chunkSize;
  uint16_t columns = chunks * _PM_chunkSize; // Padded matrix width
  uint32_t screenBytes =
      columns * core->numRowPairs * core->numPlanes * core->bytesPerElement;

  core->bufferSize = screenBytes; // Bytes per matrix buffer (1 or 2)
  if (core->doubleBuffer)
    screenBytes *= 2; // Total for matrix buffer(s)
  uint32_t rgbMaskBytes = core->parallel * 6 * core->bytesPerElement;

  // Allocate matrix buffer(s). Don't worry about the return type...
  // though we might be using words or longs for certain pin configs,
  // _PM_ALLOCATOR() by definition always aligns to the longest type.
  if (!(core->screenData =
            (uint8_t *)_PM_ALLOCATOR(screenBytes + rgbMaskBytes))) {
    return PROTOMATTER_ERR_MALLOC;
  }

  // rgbMask data follows the matrix buffer(s)
  core->rgbMask = core->screenData + screenBytes;

#if !defined(_PM_portToggleRegister)
  // Clear entire screenData buffer so there's no cruft in any pad bytes
  // (if using toggle register, each is set to clockMask below instead).
  memset(core->screenData, 0, screenBytes);
#endif

  // Figure out clockMask and rgbAndClockMask, clear matrix buffers
  if (core->bytesPerElement == 1) {
    core->portOffset = _PM_byteOffset(core->rgbPins[0]);
#if defined(_PM_portToggleRegister) && !defined(_PM_STRICT_32BIT_IO)
    // Clock and rgbAndClockMask are 8-bit values
    core->clockMask = _PM_portBitMask(core->clockPin) >> (core->portOffset * 8);
    core->rgbAndClockMask =
        (bitMask >> (core->portOffset * 8)) | core->clockMask;
    memset(core->screenData, core->clockMask, screenBytes);
#else
    // Clock and rgbAndClockMask are 32-bit values
    core->clockMask = _PM_portBitMask(core->clockPin);
    core->rgbAndClockMask = bitMask | core->clockMask;
#endif
    for (uint8_t i = 0; i < core->parallel * 6; i++) {
      ((uint8_t *)core->rgbMask)[i] = // Pin bitmasks are 8-bit
          _PM_portBitMask(core->rgbPins[i]) >> (core->portOffset * 8);
    }
  } else if (core->bytesPerElement == 2) {
    core->portOffset = _PM_wordOffset(core->rgbPins[0]);
#if defined(_PM_portToggleRegister) && !defined(_PM_STRICT_32BIT_IO)
    // Clock and rgbAndClockMask are 16-bit values
    core->clockMask =
        _PM_portBitMask(core->clockPin) >> (core->portOffset * 16);
    core->rgbAndClockMask =
        (bitMask >> (core->portOffset * 16)) | core->clockMask;
    uint32_t elements = screenBytes / 2;
    for (uint32_t i = 0; i < elements; i++) {
      ((uint16_t *)core->screenData)[i] = core->clockMask;
    }
#else
    // Clock and rgbAndClockMask are 32-bit values
    core->clockMask = _PM_portBitMask(core->clockPin);
    core->rgbAndClockMask = bitMask | core->clockMask;
#if defined(_PM_portToggleRegister)
    // TO DO: this ifdef and the one above can probably be wrapped up
    // in a more cohesive case. Think something similar will be needed
    // for the byte case. Will need Teensy 4.1 to test.
    uint32_t elements = screenBytes / 2;
    uint16_t mask = core->clockMask >> (core->portOffset * 16);
    for (uint32_t i = 0; i < elements; i++) {
      ((uint16_t *)core->screenData)[i] = mask;
    }
#endif
#endif
    for (uint8_t i = 0; i < core->parallel * 6; i++) {
      ((uint16_t *)core->rgbMask)[i] = // Pin bitmasks are 16-bit
          _PM_portBitMask(core->rgbPins[i]) >> (core->portOffset * 16);
    }
  } else {
    core->portOffset = 0;
    core->clockMask = _PM_portBitMask(core->clockPin);
    core->rgbAndClockMask = bitMask | core->clockMask;
#if defined(_PM_portToggleRegister)
    uint32_t elements = screenBytes / 4;
    for (uint32_t i = 0; i < elements; i++) {
      ((uint32_t *)core->screenData)[i] = core->clockMask;
    }
#endif
    for (uint8_t i = 0; i < core->parallel * 6; i++) {
      ((uint32_t *)core->rgbMask)[i] = // Pin bitmasks are 32-bit
          _PM_portBitMask(core->rgbPins[i]);
    }
  }

  // Set up remap_rb and remap_g tables, which assist in quickly converting
  // RGB565 pixel values (from the image canvas) to the number of bitplanes
  // allocated to the matrix (not always a simple shift).
  if (core->numPlanes < 6) {
    // 5 or fewer bitplanes, decimate 5-bit red+blue and 6-bit green to
    // that many planes. Shift right, in-to-out conversion is linear.
    uint8_t shift = 5 - core->numPlanes; // Might be zero, that's OK
    for (uint8_t i = 0; i < 32; i++) {
      core->remap_rb[i] = i >> shift;
    }
    shift = 6 - core->numPlanes;
    for (uint8_t i = 0; i < 64; i++) {
      core->remap_g[i] = i >> shift;
    }
  } else if (core->numPlanes == 6) {
    // 6 bitplanes exactly, 6-bit green is preserved, 5-bit red+blue
    // is expanded to 6 bits, in-to-out conversion is still linear.
    for (uint8_t i = 0; i < 32; i++) {
      core->remap_rb[i] = (i << 1) | (i >> 4); // Copy msb to lsb
    }
    for (uint8_t i = 0; i < 64; i++) {
      core->remap_g[i] = i;
    }
  } else {
    // Above 6 bitplanes, gamma correction kicks in, in-to-out conversion
    // is no longer linear, aiming for perceptual linearity instead. 5-bit
    // red+blue and 6-bit green are expanded to the number of bitplanes
    // requested (10 should be ample, but you can use more or less to
    // balance accuracy vs RAM & processor load).
    float top = (float)((1 << core->numPlanes) - 1);
    for (uint8_t i = 0; i < 32; i++) { // 5 bits red, blue
      core->remap_rb[i] =
          (uint16_t)(pow((float)i / 31.0, _PM_GAMMA) * top + 0.5);
    }
    for (uint8_t i = 0; i < 64; i++) { // 6 bits green
      core->remap_g[i] =
          (uint16_t)(pow((float)i / 63.0, _PM_GAMMA) * top + 0.5);
    }
  }

  // Estimate minimum bitplane #0 period for _PM_MAX_REFRESH_HZ rate.
  uint32_t minPeriodPerFrame = _PM_timerFreq / _PM_MAX_REFRESH_HZ;
  uint32_t minPeriodPerLine = minPeriodPerFrame / core->numRowPairs;
  core->minPeriod = minPeriodPerLine / ((1 << core->numPlanes) - 1);
  if (core->minPeriod < _PM_minMinPeriod) {
    core->minPeriod = _PM_minMinPeriod;
  }
  // Actual frame rate may be lower than this...it's only an estimate
  // and does not factor in things like address line selection delays
  // or interrupt overhead. That's OK, just don't want to exceed this
  // rate, as it'll eat all the CPU cycles.
  // Make a wild guess for the initial bit-zero interval. It's okay
  // that this is off, code adapts to actual timer results pretty quick.

  core->bitZeroPeriod = core->width * 5; // Initial guesstimate

  core->activeBuffer = 0;

  // Configure pins as outputs and initialize their states.

  core->latch.setReg = _PM_portSetRegister(core->latch.pin);
  core->latch.clearReg = _PM_portClearRegister(core->latch.pin);
  core->latch.bit = _PM_portBitMask(core->latch.pin);
  core->oe.setReg = _PM_portSetRegister(core->oe.pin);
  core->oe.clearReg = _PM_portClearRegister(core->oe.pin);
  core->oe.bit = _PM_portBitMask(core->oe.pin);

  _PM_pinOutput(core->clockPin);
  _PM_pinLow(core->clockPin); // Init clock LOW
  _PM_pinOutput(core->latch.pin);
  _PM_pinLow(core->latch.pin); // Init latch LOW
  _PM_pinOutput(core->oe.pin);
  _PM_pinHigh(core->oe.pin); // Init OE HIGH (disable output)

  for (uint8_t i = 0; i < core->parallel * 6; i++) {
    _PM_pinOutput(core->rgbPins[i]);
    _PM_pinLow(core->rgbPins[i]);
  }
#if defined(_PM_portToggleRegister)
  core->addrPortToggle = _PM_portToggleRegister(core->addr[0].pin);
  core->singleAddrPort = 1;
#endif
  core->prevRow = (1 << core->numAddressLines) - 2;
  for (uint8_t line = 0, bit = 1; line < core->numAddressLines;
       line++, bit <<= 1) {
    core->addr[line].setReg = _PM_portSetRegister(core->addr[line].pin);
    core->addr[line].clearReg = _PM_portClearRegister(core->addr[line].pin);
    core->addr[line].bit = _PM_portBitMask(core->addr[line].pin);
    _PM_pinOutput(core->addr[line].pin);
    if (core->prevRow & bit) {
      _PM_pinHigh(core->addr[line].pin);
    } else {
      _PM_pinLow(core->addr[line].pin);
    }
#if defined(_PM_portToggleRegister)
    // If address pin on different port than addr 0, no singleAddrPort.
    if (_PM_portToggleRegister(core->addr[line].pin) != core->addrPortToggle) {
      core->singleAddrPort = 0;
    }
#endif
  }

  // Get pointers to bit set and clear registers (and toggle, if present)
  core->setReg = (uint8_t *)_PM_portSetRegister(core->clockPin);
  core->clearReg = (uint8_t *)_PM_portClearRegister(core->clockPin);
#if defined(_PM_portToggleRegister)
  core->toggleReg = (uint8_t *)_PM_portToggleRegister(core->clockPin);
#endif

  // Reset plane/row counters, config and start timer
  _PM_resume(core);

  return PROTOMATTER_OK;
}

// Disable (but do not deallocate) a Protomatter matrix. Disables matrix by
// setting OE pin HIGH and writing all-zero data to matrix shift registers,
// so it won't halt with lit LEDs.
void _PM_stop(Protomatter_core *core) {
  if ((core)) {
    while (core->swapBuffers)
      ;                         // Wait for any pending buffer swap
    _PM_timerStop(core->timer); // Halt timer
    _PM_setReg(core->oe);       // Set OE HIGH (disable output)
    // So, in PRINCIPLE, setting OE high would be sufficient...
    // but in case that pin is shared with another function such
    // as the onloard LED (which pulses during bootloading) let's
    // also clear out the matrix shift registers for good measure.
    // Set all RGB pins LOW...
    for (uint8_t i = 0; i < core->parallel * 6; i++) {
      _PM_pinLow(core->rgbPins[i]);
    }
    // Clock out bits (just need to toggle clock with RGBs held low)
    for (uint32_t i = 0; i < core->width; i++) {
      _PM_pinHigh(core->clockPin);
      _PM_clockHoldHigh;
      _PM_pinLow(core->clockPin);
      _PM_clockHoldLow;
    }
    // Latch data
    _PM_setReg(core->latch);
    _PM_clearReg(core->latch);
  }
}

void _PM_resume(Protomatter_core *core) {
  if ((core)) {
    // Init plane & row to max values so they roll over on 1st interrupt
    core->plane = core->numPlanes - 1;
    core->row = core->numRowPairs - 1;
    core->prevRow = (core->numRowPairs > 1) ? (core->row - 1) : 1;
    core->swapBuffers = 0;
    core->frameCount = 0;

    _PM_timerInit(core->timer);        // Configure timer
    _PM_timerStart(core->timer, 1000); // Start timer
  }
}

// Free memory associated with core structure. Does NOT dealloc struct.
void _PM_free(Protomatter_core *core) {
  if ((core)) {
    _PM_stop(core);
    // TO DO: Set all pins back to inputs here?
    if (core->screenData)
      _PM_FREE(core->screenData);
    if (core->addr)
      _PM_FREE(core->addr);
    if (core->rgbPins) {
      _PM_FREE(core->rgbPins);
      core->rgbPins = NULL;
    }
  }
}

// ISR function (in arch.h) calls this function which it extern'd.
// Profuse apologies for the ESP32-specific IRAM_ATTR here -- the goal was
// for all architecture-specific detauls to be in arch.h -- but the need
// for one here caught me off guard. So, in arch.h, for all non-ESP32
// devices, IRAM_ATTR is defined to nothing and is ignored here. If any
// future architectures have their own attribute for making a function
// RAM-resident, #define IRAM_ATTR to that in the corresponding device-
// specific section of arch.h. Sorry. :/
// Any functions called by this function should also be IRAM_ATTR'd.
IRAM_ATTR void _PM_row_handler(Protomatter_core *core) {

  _PM_setReg(core->oe); // Disable LED output

  // ESP32 requires this next line, but not wanting to put arch-specific
  // ifdefs in this code...it's a trivial operation so just do it.
  // Latch is already clear at this point, but we go through the motions
  // to clear it again in order to sync up the setReg(OE) above with the
  // setReg(latch) that follows. Reason being, bit set/clear operations
  // on ESP32 aren't truly atomic, and if those two pins are on the same
  // port (quite common) the second setReg will be ignored. The nonsense
  // clearReg is used to sync up the two setReg operations. See also the
  // ESP32-specific PEW define in arch.h, same deal.
  _PM_clearReg(core->latch);

  _PM_setReg(core->latch);
  // Stop timer, save count value at stop
  uint32_t elapsed = _PM_timerStop(core->timer);
  uint8_t prevPlane = core->plane; // Save that plane # for later timing
  _PM_clearReg(core->latch);       // (split to add a few cycles)

  // If plane 0 just finished being displayed (plane 1 was loaded on prior
  // pass, or there's only one plane...I know, it's confusing), take note
  // of the elapsed timer value, for subsequent bitplane timing (each
  // plane period is double the previous). Value is filtered slightly to
  // avoid jitter.
  if ((prevPlane == 1) || (core->numPlanes == 1)) {
    core->bitZeroPeriod = ((core->bitZeroPeriod * 7) + elapsed) / 8;
    if (core->bitZeroPeriod < core->minPeriod) {
      core->bitZeroPeriod = core->minPeriod;
    }
  }

  if (prevPlane == 0) { // Plane 0 just finished loading
#if defined(_PM_portToggleRegister)
    // If all address lines are on a single PORT (and bit toggle is
    // available), do address line change all at once. Even doing all
    // this math takes MUCH less time than the delays required when
    // doing line-by-line changes.
    if (core->singleAddrPort) {
      // Make bitmasks of prior and new row bits
      uint32_t priorBits = 0, newBits = 0;
      for (uint8_t line = 0, bit = 1; line < core->numAddressLines;
           line++, bit <<= 1) {
        if (core->row & bit) {
          newBits |= core->addr[line].bit;
        }
        if (core->prevRow & bit) {
          priorBits |= core->addr[line].bit;
        }
      }
      *(volatile _PM_PORT_TYPE *)core->addrPortToggle = newBits ^ priorBits;
      _PM_delayMicroseconds(_PM_ROW_DELAY);
    } else {
#endif
      // Configure row address lines individually, making changes
      // (with delays) only where necessary.
      for (uint8_t line = 0, bit = 1; line < core->numAddressLines;
           line++, bit <<= 1) {
        if ((core->row & bit) != (core->prevRow & bit)) {
          if (core->row & bit) { // Set addr line high
            _PM_setReg(core->addr[line]);
          } else { // Set addr line low
            _PM_clearReg(core->addr[line]);
          }
          _PM_delayMicroseconds(_PM_ROW_DELAY);
        }
      }
#if defined(_PM_portToggleRegister)
    }
#endif
    core->prevRow = core->row;
  }

  // Advance bitplane index and/or row as necessary
  if (++core->plane >= core->numPlanes) {   // Next data bitplane, or
    core->plane = 0;                        // roll over bitplane to start
    if (++core->row >= core->numRowPairs) { // Next row, or
      core->row = 0;                        // roll over row to start
      // Switch matrix buffers if due (only if double-buffered)
      if (core->swapBuffers) {
        core->activeBuffer = 1 - core->activeBuffer;
        core->swapBuffers = 0; // Swapped!
      }
      core->frameCount++;
    }
  }

  // 'plane' now is index of data to issue, NOT data to display.
  // 'prevPlane' is the previously-loaded data, which gets displayed
  // now while the next plane data is loaded.

  // Set timer and enable LED output for data loaded on PRIOR pass:
  _PM_timerStart(core->timer, core->bitZeroPeriod << prevPlane);
  _PM_delayMicroseconds(1); // Appease Teensy4
  _PM_clearReg(core->oe);   // Enable LED output

  uint32_t elementsPerLine =
      _PM_chunkSize * ((core->width + (_PM_chunkSize - 1)) / _PM_chunkSize);
  uint32_t srcOffset = elementsPerLine *
                       (core->numPlanes * core->row + core->plane) *
                       core->bytesPerElement;
  if (core->doubleBuffer) {
    srcOffset += core->bufferSize * core->activeBuffer;
  }

  if (core->bytesPerElement == 1) {
    blast_byte(core, (uint8_t *)(core->screenData + srcOffset));
  } else if (core->bytesPerElement == 2) {
    blast_word(core, (uint16_t *)(core->screenData + srcOffset));
  } else {
    blast_long(core, (uint32_t *)(core->screenData + srcOffset));
  }

  // 'plane' data is now loaded, will be shown on NEXT pass
}

// Innermost data-stuffing loop functions

// The presence of a bit-toggle register can make the data-stuffing loop a
// fair bit faster (2 PORT accesses per column vs 3). But ironically, some
// devices (e.g. SAMD51) can outpace the matrix max CLK speed, so we slow
// them down with a few NOPs. These are defined in arch.h as needed.
// _PM_clockHoldLow is whatever code necessary to delay the clock rise
// after data is placed on the PORT. _PM_clockHoldHigh is code for delay
// before setting the clock back low. If undefined, nothing goes there.

#if !defined(PEW) // arch.h can define a custom PEW if needed (e.g. ESP32)

#if !defined(_PM_STRICT_32BIT_IO) // Partial access to 32-bit GPIO OK

#if defined(_PM_portToggleRegister)
#define PEW                                                                    \
  *toggle = *data++; /* Toggle in new data + toggle clock low */               \
  _PM_clockHoldLow;                                                            \
  *toggle = clock; /* Toggle clock high */                                     \
  _PM_clockHoldHigh;
#else
#define PEW                                                                    \
  *set = *data++; /* Set RGB data high */                                      \
  _PM_clockHoldLow;                                                            \
  *set_full = clock; /* Set clock high */                                      \
  _PM_clockHoldHigh;                                                           \
  *clear_full = rgbclock; /* Clear RGB data + clock */                         \
  ///< Bitbang one set of RGB data bits to matrix
#endif

#else // ONLY 32-bit GPIO

#if defined(_PM_portToggleRegister)
#define PEW                                                                    \
  *toggle = *data++ << shift; /* Toggle in new data + toggle clock low */      \
  _PM_clockHoldLow;                                                            \
  *toggle = clock; /* Toggle clock high */                                     \
  _PM_clockHoldHigh;
#else
#define PEW                                                                    \
  *set = *data++ << shift; /* Set RGB data high */                             \
  _PM_clockHoldLow;                                                            \
  *set = clock; /* Set clock high */                                           \
  _PM_clockHoldHigh;                                                           \
  *clear = rgbclock; /* Clear RGB data + clock */                              \
  ///< Bitbang one set of RGB data bits to matrix
#endif

#endif // end 32-bit GPIO

#endif // end PEW

#if _PM_chunkSize == 1
#define PEW_UNROLL PEW
#elif _PM_chunkSize == 2
#define PEW_UNROLL PEW PEW ///< 2-way PEW unroll
#elif _PM_chunkSize == 4
#define PEW_UNROLL PEW PEW PEW PEW ///< 4-way PEW unroll
#elif _PM_chunkSize == 8
#define PEW_UNROLL PEW PEW PEW PEW PEW PEW PEW PEW ///< 8-way PEW unroll
#elif _PM_chunkSize == 16
#define PEW_UNROLL                                                             \
  PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW
#elif _PM_chunkSize == 32
#define PEW_UNROLL                                                             \
  PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW  \
      PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW
#elif _PM_chunkSize == 64
#define PEW_UNROLL                                                             \
  PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW  \
      PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW  \
          PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW  \
              PEW PEW PEW PEW PEW PEW PEW PEW PEW PEW
#else
#error "Unimplemented _PM_chunkSize value"
#endif

// There are THREE COPIES of the following function -- one each for byte,
// word and long. If changes are made in any one of them, the others MUST
// be updated to match! (Decided against using macro tricks for the
// function, too often ends in disaster...but must be vigilant in the
// three-function maintenance then.)

IRAM_ATTR static void blast_byte(Protomatter_core *core, uint8_t *data) {
#if !defined(_PM_STRICT_32BIT_IO) // Partial access to 32-bit GPIO OK

#if defined(_PM_portToggleRegister)
  // If here, it was established in begin() that the RGB data bits and
  // clock are all within the same byte of a PORT register, else we'd be
  // in the word- or long-blasting functions now. So we just need an
  // 8-bit pointer to the PORT.
  volatile uint8_t *toggle =
      (volatile uint8_t *)core->toggleReg + core->portOffset;
#else
  // No-toggle version is a little different. If here, RGB data is all
  // in one byte of PORT register, clock can be any bit in 32-bit PORT.
  volatile uint8_t *set;              // For RGB data set
  volatile _PM_PORT_TYPE *set_full;   // For clock set
  volatile _PM_PORT_TYPE *clear_full; // For RGB data + clock clear
  set = (volatile uint8_t *)core->setReg + core->portOffset;
  set_full = (volatile _PM_PORT_TYPE *)core->setReg;
  clear_full = (volatile _PM_PORT_TYPE *)core->clearReg;
  _PM_PORT_TYPE rgbclock = core->rgbAndClockMask; // RGB + clock bit
#endif
  _PM_PORT_TYPE clock = core->clockMask; // Clock bit
  uint8_t chunks = (core->width + (_PM_chunkSize - 1)) / _PM_chunkSize;

  // PORT has already been initialized with RGB data + clock bits
  // all LOW, so we don't need to initialize that state here.

  while (chunks--) {
    PEW_UNROLL // _PM_chunkSize RGB+clock writes
  }

#if defined(_PM_portToggleRegister)
  // Want the PORT left with RGB data and clock LOW on function exit
  // (so it's easier to see on 'scope, and to prime it for the next call).
  // This is implicit in the no-toggle case (due to how the PEW macro
  // works), but toggle case requires explicitly clearing those bits.
  // rgbAndClockMask is an 8-bit value when toggling, hence offset here.
  *((volatile uint8_t *)core->clearReg + core->portOffset) =
      core->rgbAndClockMask;
#endif

#else // ONLY 32-bit GPIO

#if defined(_PM_portToggleRegister)
  volatile _PM_PORT_TYPE *toggle = (volatile _PM_PORT_TYPE *)core->toggleReg;
#else
  volatile _PM_PORT_TYPE *set = (volatile _PM_PORT_TYPE *)core->setReg;
  volatile _PM_PORT_TYPE *clear = (volatile _PM_PORT_TYPE *)core->clearReg;
  _PM_PORT_TYPE rgbclock = core->rgbAndClockMask; // RGB + clock bit
#endif
  _PM_PORT_TYPE clock = core->clockMask; // Clock bit
  uint8_t shift = core->portOffset * 8;
  uint8_t chunks = (core->width + (_PM_chunkSize - 1)) / _PM_chunkSize;

  // PORT has already been initialized with RGB data + clock bits
  // all LOW, so we don't need to initialize that state here.

  while (chunks--) {
    PEW_UNROLL // _PM_chunkSize RGB+clock writes
  }

#if defined(_PM_portToggleRegister)
  *((volatile uint32_t *)core->clearReg) = core->rgbAndClockMask;
#endif

#endif // 32-bit GPIO
}

IRAM_ATTR static void blast_word(Protomatter_core *core, uint16_t *data) {
#if !defined(_PM_STRICT_32BIT_IO) // Partial access to 32-bit GPIO OK

#if defined(_PM_portToggleRegister)
  // See notes above -- except now 16-bit word in PORT.
  volatile uint16_t *toggle =
      (volatile uint16_t *)core->toggleReg + core->portOffset;
#else
  volatile uint16_t *set;                         // For RGB data set
  volatile _PM_PORT_TYPE *set_full;               // For clock set
  volatile _PM_PORT_TYPE *clear_full;             // For RGB data + clock clear
  set = (volatile uint16_t *)core->setReg + core->portOffset;
  set_full = (volatile _PM_PORT_TYPE *)core->setReg;
  clear_full = (volatile _PM_PORT_TYPE *)core->clearReg;
  _PM_PORT_TYPE rgbclock = core->rgbAndClockMask; // RGB + clock bit
#endif
  _PM_PORT_TYPE clock = core->clockMask; // Clock bit
  uint8_t chunks = (core->width + (_PM_chunkSize - 1)) / _PM_chunkSize;
  while (chunks--) {
    PEW_UNROLL // _PM_chunkSize RGB+clock writes
  }
#if defined(_PM_portToggleRegister)
  // rgbAndClockMask is a 16-bit value when toggling, hence offset here.
  *((volatile uint16_t *)core->clearReg + core->portOffset) =
      core->rgbAndClockMask;
#endif

#else // ONLY 32-bit GPIO

#if defined(_PM_portToggleRegister)
  volatile _PM_PORT_TYPE *toggle = (volatile _PM_PORT_TYPE *)core->toggleReg;
#else
  volatile _PM_PORT_TYPE *set = (volatile _PM_PORT_TYPE *)core->setReg;
  volatile _PM_PORT_TYPE *clear = (volatile _PM_PORT_TYPE *)core->clearReg;
  _PM_PORT_TYPE rgbclock = core->rgbAndClockMask; // RGB + clock bit
#endif
  _PM_PORT_TYPE clock = core->clockMask; // Clock bit
  uint8_t shift = core->portOffset * 16;
  uint8_t chunks = (core->width + (_PM_chunkSize - 1)) / _PM_chunkSize;
  while (chunks--) {
    PEW_UNROLL // _PM_chunkSize RGB+clock writes
  }
#if defined(_PM_portToggleRegister)
  *((volatile _PM_PORT_TYPE *)core->clearReg) = core->rgbAndClockMask;
#endif

#endif // 32-bit GPIO
}

IRAM_ATTR static void blast_long(Protomatter_core *core, uint32_t *data) {
#if defined(_PM_portToggleRegister)
  // See notes above -- except now full 32-bit PORT.
  volatile uint32_t *toggle = (volatile uint32_t *)core->toggleReg;
#else
  // Note in this case two copies exist of the PORT set register.
  // The optimizer will most likely simplify this; leaving as-is, not
  // wanting a special case of the PEW macro due to divergence risk.
  volatile uint32_t *set;             // For RGB data set
  volatile _PM_PORT_TYPE *set_full;   // For clock set
  volatile _PM_PORT_TYPE *clear_full; // For RGB data + clock clear
  set = (volatile uint32_t *)core->setReg;
  set_full = (volatile _PM_PORT_TYPE *)core->setReg;
  clear_full = (volatile _PM_PORT_TYPE *)core->clearReg;
  _PM_PORT_TYPE rgbclock = core->rgbAndClockMask; // RGB + clock bit
#endif
  _PM_PORT_TYPE clock = core->clockMask; // Clock bit
#if defined(_PM_STRICT_32BIT_IO)
  uint8_t shift = 0;
#endif
  uint8_t chunks = (core->width + (_PM_chunkSize - 1)) / _PM_chunkSize;
  while (chunks--) {
    PEW_UNROLL // _PM_chunkSize RGB+clock writes
  }
#if defined(_PM_portToggleRegister)
  *(volatile uint32_t *)core->clearReg = core->rgbAndClockMask;
#endif
}

// Returns current value of frame counter and resets its value to zero.
// Two calls to this, timed one second apart (or use math with other
// intervals), can be used to get a rough frames-per-second value for
// the matrix (since this is difficult to estimate beforehand).
uint32_t _PM_getFrameCount(Protomatter_core *core) {
  uint32_t count = 0;
  if ((core)) {
    count = core->frameCount;
    core->frameCount = 0;
  }
  return count;
}

// Note to future self: I've gone back and forth between implementing all
// this either as it currently is (with byte, word and long cases for various
// steps), or using a uint32_t[64] table for expanding RGB bit combos to PORT
// bit combos. The latter would certainly simplify the code a ton, and the
// additional table lookup step wouldn't significantly impact performance,
// especially going forward with faster processors (the SAMD51 code already
// requires a few NOPs in the innermost loop to avoid outpacing the matrix).
// BUT, the reason this is NOT currently done is that it only allows for a
// single matrix chain (doing parallel chains would require either an
// impractically large lookup table, or adding together multiple tables'
// worth of bitmasks, which would slow things down in the vital inner loop).
// Although parallel matrix chains aren't yet 100% implemented in this code
// right now, I wanted to leave that possibility for the future, as a way to
// handle larger matrix combos, because long chains will slow down the
// refresh rate.
