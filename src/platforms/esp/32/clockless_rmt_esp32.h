/*
 * Integration into FastLED ClocklessController
 * Copyright (c) 2018,2019,2020 Samuel Z. Guyer
 * Copyright (c) 2017 Thomas Basler
 * Copyright (c) 2017 Martin F. Falatic
 *
 * ESP32 support is provided using the RMT peripheral device -- a unit
 * on the chip designed specifically for generating (and receiving)
 * precisely-timed digital signals. Nominally for use in infrared
 * remote controls, we use it to generate the signals for clockless
 * LED strips. The main advantage of using the RMT device is that,
 * once programmed, it generates the signal asynchronously, allowing
 * the CPU to continue executing other code. It is also not vulnerable
 * to interrupts or other timing problems that could disrupt the signal.
 *
 * The implementation strategy is borrowed from previous work and from
 * the RMT support built into the ESP32 IDF. The RMT device has 8
 * channels, which can be programmed independently to send sequences
 * of high/low bits. Memory for each channel is limited, however, so
 * in order to send a long sequence of bits, we need to continuously
 * refill the buffer until all the data is sent. To do this, we fill
 * half the buffer and then set an interrupt to go off when that half
 * is sent. Then we refill that half while the second half is being
 * sent. This strategy effectively overlaps computation (by the CPU)
 * and communication (by the RMT).
 *
 * Since the RMT device only has 8 channels, we need a strategy to
 * allow more than 8 LED controllers. Our driver assigns controllers
 * to channels on the fly, queuing up controllers as necessary until a
 * channel is free. The main showPixels routine just fires off the
 * first 8 controllers; the interrupt handler starts new controllers
 * asynchronously as previous ones finish. So, for example, it can
 * send the data for 8 controllers simultaneously, but 16 controllers
 * would take approximately twice as much time.
 *
 * There is a #define that allows a program to control the total
 * number of channels that the driver is allowed to use. It defaults
 * to 8 -- use all the channels. Setting it to 1, for example, results
 * in fully serial output:
 *
 *     #define FASTLED_RMT_MAX_CHANNELS 1
 *
 * OTHER RMT APPLICATIONS
 *
 * The default FastLED driver takes over control of the RMT interrupt
 * handler, making it hard to use the RMT device for other
 * (non-FastLED) purposes. You can change it's behavior to use the ESP
 * core driver instead, allowing other RMT applications to
 * co-exist. To switch to this mode, add the following directive
 * before you include FastLED.h:
 *
 *      #define FASTLED_RMT_BUILTIN_DRIVER 1
 *
 * There may be a performance penalty for using this mode. We need to
 * compute the RMT signal for the entire LED strip ahead of time,
 * rather than overlapping it with communication. We also need a large
 * buffer to hold the signal specification. Each bit of pixel data is
 * represented by a 32-bit pulse specification, so it is a 32X blow-up
 * in memory use.
 *
 * NEW: Use of Flash memory on the ESP32 can interfere with the timing
 *      of pixel output. The ESP-IDF system code disables all other
 *      code running on *either* core during these operation. To prevent
 *      this from happening, define this flag. It will force flash
 *      operations to wait until the show() is done.
 *
 * #define FASTLED_ESP32_FLASH_LOCK 1
 *
 * NEW (June 2020): The RMT controller has been split into two
 *      classes: ClocklessController, which is an instantiation of the
 *      FastLED CPixelLEDController template, and ESP32RMTController,
 *      which just handles driving the RMT peripheral. One benefit of
 *      this design is that ESP32RMTContoller is not a template, so
 *      its methods can be marked with the IRAM_ATTR and end up in
 *      IRAM memory. Another benefit is that all of the color channel
 *      processing is done up-front, in the templated class, so we
 *      can fill the RMT buffers more quickly.
 *
 *      IN THEORY, this design would also allow FastLED.show() to 
 *      send the data while the program continues to prepare the next
 *      frame of data.
 *
 * Based on public domain code created 19 Nov 2016 by Chris Osborn <fozztexx@fozztexx.com>
 * http://insentricity.com *
 *
 */
/*
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

FASTLED_NAMESPACE_BEGIN

#ifdef __cplusplus
extern "C" {
#endif

#include "esp32-hal.h"
#include "esp_intr.h"
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "driver/periph_ctrl.h"
#include "freertos/semphr.h"
#include "soc/rmt_struct.h"

#include "esp_log.h"

extern void spi_flash_op_lock(void);
extern void spi_flash_op_unlock(void);

#ifdef __cplusplus
}
#endif

__attribute__ ((always_inline)) inline static uint32_t __clock_cycles() {
  uint32_t cyc;
  __asm__ __volatile__ ("rsr %0,ccount":"=a" (cyc));
  return cyc;
}

#define FASTLED_HAS_CLOCKLESS 1
#define NUM_COLOR_CHANNELS 3

// NOT CURRENTLY IMPLEMENTED:
// -- Set to true to print debugging information about timing
//    Useful for finding out if timing is being messed up by other things
//    on the processor (WiFi, for example)
//#ifndef FASTLED_RMT_SHOW_TIMER
//#define FASTLED_RMT_SHOW_TIMER false
//#endif

// -- Configuration constants
#define DIVIDER       2 /* 4, 8 still seem to work, but timings become marginal */

// -- RMT memory configuration
//    By default we use two memory blocks for each RMT channel instead of 1. The
//    reason is that one memory block is only 64 bits, which causes the refill
//    interrupt to fire too often. When combined with WiFi, this leads to conflicts
//    between interrupts and weird flashy effects on the LEDs. Special thanks to
//    Brian Bulkowski for finding this problem and developing a fix.
#ifndef FASTLED_RMT_MEM_BLOCKS
#define FASTLED_RMT_MEM_BLOCKS 2
#endif

#define MAX_PULSES         (64 * FASTLED_RMT_MEM_BLOCKS) /* One block has a 64 "pulse" buffer */
#define PULSES_PER_FILL    (MAX_PULSES / 2)              /* Half of the channel buffer */

// -- Convert ESP32 CPU cycles to RMT device cycles, taking into account the divider
#define F_CPU_RMT                   (  80000000L)
#define RMT_CYCLES_PER_SEC          (F_CPU_RMT/DIVIDER)
#define RMT_CYCLES_PER_ESP_CYCLE    (F_CPU / RMT_CYCLES_PER_SEC)
#define ESP_TO_RMT_CYCLES(n)        ((n) / (RMT_CYCLES_PER_ESP_CYCLE))

// -- Number of cycles to signal the strip to latch
#define NS_PER_CYCLE                ( 1000000000L / RMT_CYCLES_PER_SEC )
#define NS_TO_CYCLES(n)             ( (n) / NS_PER_CYCLE )
#define RMT_RESET_DURATION          NS_TO_CYCLES(50000)

// -- Core or custom driver
#ifndef FASTLED_RMT_BUILTIN_DRIVER
#define FASTLED_RMT_BUILTIN_DRIVER false
#endif

// -- Max number of controllers we can support
#ifndef FASTLED_RMT_MAX_CONTROLLERS
#define FASTLED_RMT_MAX_CONTROLLERS 32
#endif

// -- Number of RMT channels to use (up to 8, but 4 by default)
//    Redefine this value to 1 to force serial output
#ifndef FASTLED_RMT_MAX_CHANNELS
#define FASTLED_RMT_MAX_CHANNELS (8/FASTLED_RMT_MEM_BLOCKS)
#endif

class ESP32RMTController
{
private:

    // -- RMT has 8 channels, numbered 0 to 7
    rmt_channel_t  mRMT_channel;

    // -- Store the GPIO pin
    gpio_num_t     mPin;

    // -- Timing values for zero and one bits, derived from T1, T2, and T3
    rmt_item32_t   mZero;
    rmt_item32_t   mOne;

    // -- Total expected time to send 32 bits
    //    Each strip should get an interrupt roughly at this interval
    uint32_t       mCyclesPerFill;
    uint32_t       mMaxCyclesPerFill;
    uint32_t       mLastFill;

    // -- Pixel data
    uint32_t *     mPixelData;
    int            mSize;
    int            mCur;

    // -- RMT memory
    volatile uint32_t * mRMT_mem_ptr;
    volatile uint32_t * mRMT_mem_start;
    int                 mWhichHalf;

    // -- Buffer to hold all of the pulses. For the version that uses
    //    the RMT driver built into the ESP core.
    rmt_item32_t * mBuffer;
    uint16_t       mBufferSize; // bytes
    int            mCurPulse;

public:

    // -- Constructor
    //    Mainly just stores the template parameters from the LEDController as
    //    member variables.
    ESP32RMTController(int DATA_PIN, int T1, int T2, int T3);

    // -- Get max cycles per fill
    uint32_t IRAM_ATTR getMaxCyclesPerFill() const { return mMaxCyclesPerFill; }

    // -- Get or create the pixel data buffer
    uint32_t * getPixelBuffer(int size_in_bytes);

    // -- Initialize RMT subsystem
    //    This only needs to be done once
    static void init();

    // -- Show this string of pixels
    //    This is the main entry point for the pixel controller
    void IRAM_ATTR showPixels();

    // -- Start up the next controller
    //    This method is static so that it can dispatch to the
    //    appropriate startOnChannel method of the given controller.
    static void IRAM_ATTR startNext(int channel);

    // -- Start this controller on the given channel
    //    This function just initiates the RMT write; it does not wait
    //    for it to finish.
    void IRAM_ATTR startOnChannel(int channel);

    // -- Start RMT transmission
    //    Setting this RMT flag is what actually kicks off the peripheral
    void IRAM_ATTR tx_start();

    // -- A controller is done 
    //    This function is called when a controller finishes writing
    //    its data. It is called either by the custom interrupt
    //    handler (below), or as a callback from the built-in
    //    interrupt handler. It is static because we don't know which
    //    controller is done until we look it up.
    static void IRAM_ATTR doneOnChannel(rmt_channel_t channel, void * arg);
    
    // -- Custom interrupt handler
    //    This interrupt handler handles two cases: a controller is
    //    done writing its data, or a controller needs to fill the
    //    next half of the RMT buffer with data.
    static void IRAM_ATTR interruptHandler(void *arg);

    // -- Fill RMT buffer
    //    Puts 32 bits of pixel data into the next 32 slots in the RMT memory
    //    Each data bit is represented by a 32-bit RMT item that specifies how
    //    long to hold the signal high, followed by how long to hold it low.
    //    NOTE: Now the default is to use 128-bit buffers, so half a buffer is
    //          is 64 bits. See FASTLED_RMT_MEM_BLOCKS
    void IRAM_ATTR fillNext(bool check_time);

    // -- Init pulse buffer
    //    Set up the buffer that will hold all of the pulse items for this
    //    controller. 
    //    This function is only used when the built-in RMT driver is chosen
    void initPulseBuffer(int size_in_bytes);

    // -- Convert a byte into RMT pulses
    //    This function is only used when the built-in RMT driver is chosen
    void convertByte(uint32_t byteval);
};

template <int DATA_PIN, int T1, int T2, int T3, EOrder RGB_ORDER = RGB, int XTRA0 = 0, bool FLIP = false, int WAIT_TIME = 5>
class ClocklessController : public CPixelLEDController<RGB_ORDER>
{
private:

    // -- The actual controller object for ESP32
    ESP32RMTController mRMTController;

    // -- This instantiation forces a check on the pin choice
    FastPin<DATA_PIN> mFastPin;

public:

    ClocklessController()
        : mRMTController(DATA_PIN, T1, T2, T3)
        {}

    void init()
    {
        // mRMTController = new ESP32RMTController(DATA_PIN, T1, T2, T3);
    }

    virtual uint16_t getMaxRefreshRate() const { return 400; }

protected:

    // -- Load pixel data
    //    This method loads all of the pixel data into a separate buffer for use by
    //    by the RMT driver. Copying does two important jobs: it fixes the color
    //    order for the pixels, and it performs the scaling/adjusting ahead of time.
    //    It also packs the bytes into 32 bit chunks with the right bit order.
    void loadPixelData(PixelController<RGB_ORDER> & pixels)
    {
        // -- Make sure the buffer is allocated
        int size_in_bytes = pixels.size() * 3;
        uint32_t * pData = mRMTController.getPixelBuffer(size_in_bytes);

        // -- Read out the pixel data using the pixel controller methods that
        //    perform the scaling and adjustments 
        int count = 0;
        int which = 0;
        while (pixels.has(1)) {
            // -- Get the next four bytes of data
            uint8_t four[4] = {0,0,0,0};
            for (int i = 0; i < 4; i++) {
                switch (which) {
                case 0: 
                    four[i] = pixels.loadAndScale0();
                    break;
                case 1:
                    four[i] = pixels.loadAndScale1();
                    break;
                case 2:
                    four[i] = pixels.loadAndScale2();
                    pixels.advanceData();
                    pixels.stepDithering();
                    break;
                }
                // -- Move to the next color
                which++;
                if (which > 2) which = 0;

                // -- Stop if there's no more data
                if ( ! pixels.has(1)) break;
            }

            // -- Pack the four bytes into a 32-bit value with the right bit order
            uint8_t a = four[0];
            uint8_t b = four[1];
            uint8_t c = four[2];
            uint8_t d = four[3];
            pData[count++] = a << 24 | b << 16 | c << 8 | d;
        }
    }

    // -- Show pixels
    //    This is the main entry point for the controller.
    virtual void showPixels(PixelController<RGB_ORDER> & pixels)
    {
        if (FASTLED_RMT_BUILTIN_DRIVER) {
            convertAllPixelData(pixels);
        } else {
            loadPixelData(pixels);
        }

        mRMTController.showPixels();
    }

    // -- Convert all pixels to RMT pulses
    //    This function is only used when the user chooses to use the
    //    built-in RMT driver, which needs all of the RMT pulses
    //    up-front.
    void convertAllPixelData(PixelController<RGB_ORDER> & pixels)
    {
        // -- Make sure the data buffer is allocated
        mRMTController.initPulseBuffer(pixels.size() * 3);

        // -- Cycle through the R,G, and B values in the right order,
        //    storing the pulses in the big buffer

        uint32_t byteval;
        while (pixels.has(1)) {
            byteval = pixels.loadAndScale0();
            mRMTController.convertByte(byteval);
            byteval = pixels.loadAndScale1();
            mRMTController.convertByte(byteval);
            byteval = pixels.loadAndScale2();
            mRMTController.convertByte(byteval);
            pixels.advanceData();
            pixels.stepDithering();
        }
    }
};


FASTLED_NAMESPACE_END