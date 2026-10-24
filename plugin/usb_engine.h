// usb_engine.h — USB side of the Audio 8 DJ driver.
//
// coreaudiod is allowed to open the device directly (verified: USBDeviceOpen
// succeeds from inside the HAL plug-in), so there is no helper daemon and no
// shared memory. The engine owns a thread that keeps the isochronous pipes
// running and exchanges float32 frames with the plug-in through two rings.

#ifndef A8DJ_USB_ENGINE_H
#define A8DJ_USB_ENGINE_H

#include <CoreAudio/CoreAudioTypes.h>
#include <stdint.h>

#define A8DJ_CHANNELS  8

// Starts firmware upload (if needed), configures the device for inRate and
// begins streaming. Returns 0 on success.
// Allocates the long-lived buffers and, if the device is still on its EEPROM
// image, uploads the firmware on a background thread. Call it once when the
// plug-in is initialised so the seconds that costs are not spent inside
// StartIO, where CoreAudio would give up on a blocked plug-in.
void A8DJ_UsbPrepare(void);

// Nonzero registry identity only for connected, firmware-ready hardware.
// Call off the audio thread, serialized with Prepare/Start/Stop.
uint64_t A8DJ_UsbPollDevice(void);

// Re-reads the latency config if the file changed. Only acts while no
// session exists; nonzero means latency/safety offsets changed and CoreAudio
// should be told. Same serialization as A8DJ_UsbPollDevice.
int  A8DJ_UsbReloadConfig(void);

int  A8DJ_UsbStart(double inRate);
void A8DJ_UsbStop(void);

// Frames the device has clocked in so far, and the host time that count was
// taken at. Returns 0 when the stream is not running.
int  A8DJ_UsbGetClock(uint64_t *outFrames, uint64_t *outHostTime, uint64_t *outSeed);

// Ring access for the IO cycle. Both take absolute frame positions on the same
// timeline as A8DJ_UsbGetClock.
void A8DJ_UsbReadCapture(Float32 *outBuf, uint64_t inStartFrame, uint32_t inFrames);
void A8DJ_UsbWritePlayback(const Float32 *inBuf, uint64_t inStartFrame, uint32_t inFrames);

// How far ahead of the device's play position the engine reads the playback
// ring, and how stale the newest capture data can be. CoreAudio must be told
// both or it writes and reads the wrong part of the ring.
uint32_t A8DJ_UsbOutputLeadFrames(double inRate);
uint32_t A8DJ_UsbTransferFrames(double inRate);

// kAudioDevicePropertySafetyOffset: how close to "now" CoreAudio may read
// (input) or write (output). Includes the queue lead and configured margins.
uint32_t A8DJ_UsbSafetyOffsetFrames(double inRate, int isInput);
// kAudioDevicePropertyLatency: delay outside the published timeline
// (converters, device FIFO). Never includes the queue lead.
uint32_t A8DJ_UsbDeviceLatencyFrames(double inRate, int isInput);

// Short human-readable state, surfaced through the device name while bringing
// the driver up.
const char *A8DJ_UsbStatus(void);

#endif
