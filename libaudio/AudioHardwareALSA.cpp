/* AudioHardwareALSA.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

#define LOG_TAG "AudioHardwareALSA"
#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include "AudioHardwareALSA.h"

#undef DISABLE_HARWARE_RESAMPLING

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

#define SND_MIXER_VOL_RANGE_MIN  (0)
#define SND_MIXER_VOL_RANGE_MAX  (100)

#define ALSA_NAME_MAX 128

#define ALSA_STRCAT(x,y) \
    if (strlen(x) + strlen(y) < ALSA_NAME_MAX) \
        strcat(x, y);

extern "C"
{
    //
    // Make sure this prototype is consistent with what's in
    // external/libasound/alsa-lib-1.0.16/src/pcm/pcm_null.c!
    //
    extern int snd_pcm_null_open(snd_pcm_t **pcmp,
                                 const char *name,
                                 snd_pcm_stream_t stream,
                                 int mode);

    //
    // Function for dlsym() to look up for creating a new AudioHardwareInterface.
    //
    android::AudioHardwareInterface *createAudioHardware(void) {
        return new android::AudioHardwareALSA();
    }
}         // extern "C"

namespace android
{

typedef AudioSystem::audio_routes audio_routes;

#define ROUTE_ALL            AudioSystem::ROUTE_ALL
#define ROUTE_EARPIECE       AudioSystem::ROUTE_EARPIECE
#define ROUTE_SPEAKER        AudioSystem::ROUTE_SPEAKER
#define ROUTE_BLUETOOTH_SCO  AudioSystem::ROUTE_BLUETOOTH_SCO
#define ROUTE_HEADSET        AudioSystem::ROUTE_HEADSET
#define ROUTE_BLUETOOTH_A2DP AudioSystem::ROUTE_BLUETOOTH_A2DP
#ifdef FM_ROUTE_SUPPORT
#define ROUTE_FM             AudioSystem::ROUTE_FM
#endif

// ----------------------------------------------------------------------------

static const int DEFAULT_SAMPLE_RATE = ALSA_DEFAULT_SAMPLE_RATE;

static const char _nullALSADeviceName[] = "NULL_Device";

static void ALSAErrorHandler(const char *file,
                             int line,
                             const char *function,
                             int err,
                             const char *fmt,
                             ...)
{
    char buf[BUFSIZ];
    va_list arg;
    int l;

    va_start(arg, fmt);
    l = snprintf(buf, BUFSIZ, "%s:%i:(%s) ", file, line, function);
    vsnprintf(buf + l, BUFSIZ - l, fmt, arg);
    buf[BUFSIZ-1] = '\0';
    LOG(LOG_ERROR, "ALSALib", buf);
    va_end(arg);
}

// ----------------------------------------------------------------------------

/* The following table(s) need to match in order of the route bits
 */
static const char *deviceSuffix[] = {
    /* ROUTE_EARPIECE       */ "_Earpiece",
    /* ROUTE_SPEAKER        */ "_Speaker",
    /* ROUTE_BLUETOOTH_SCO  */ "_Bluetooth",
    /* ROUTE_HEADSET        */ "_Headset",
    /* ROUTE_BLUETOOTH_A2DP */ "_Bluetooth-A2DP",
#ifdef FM_ROUTE_SUPPORT
    /* ROUTE_FM             */ "_FM",
#endif
};

static const int deviceSuffixLen = (sizeof(deviceSuffix) / sizeof(char *));

struct mixer_info_t;

struct alsa_properties_t
{
    const audio_routes  routes;
    const char         *propName;
    const char         *propDefault;
    mixer_info_t       *mInfo;
};

static alsa_properties_t
mixerMasterProp[SND_PCM_STREAM_LAST+1] = {
    { ROUTE_ALL, "alsa.mixer.playback.master",  "PCM",     NULL},
    { ROUTE_ALL, "alsa.mixer.capture.master",   "Capture", NULL}
};

static alsa_properties_t
mixerProp[][SND_PCM_STREAM_LAST+1] = {
    {
        {ROUTE_EARPIECE,       "alsa.mixer.playback.earpiece",       "Earpiece", NULL},
        {ROUTE_EARPIECE,       "alsa.mixer.capture.earpiece",        "Capture",  NULL}
    },
    {
        {ROUTE_SPEAKER,        "alsa.mixer.playback.speaker",        "Speaker", NULL},
        {ROUTE_SPEAKER,        "alsa.mixer.capture.speaker",         "",        NULL}
    },
    {
        {ROUTE_BLUETOOTH_SCO,  "alsa.mixer.playback.bluetooth.sco",  "Bluetooth",         NULL},
        {ROUTE_BLUETOOTH_SCO,  "alsa.mixer.capture.bluetooth.sco",   "Bluetooth Capture", NULL}
    },
    {
        {ROUTE_HEADSET,        "alsa.mixer.playback.headset",        "Headphone", NULL},
        {ROUTE_HEADSET,        "alsa.mixer.capture.headset",         "Capture",   NULL}
    },
    {
        {ROUTE_BLUETOOTH_A2DP, "alsa.mixer.playback.bluetooth.a2dp", "Bluetooth A2DP",         NULL},
        {ROUTE_BLUETOOTH_A2DP, "alsa.mixer.capture.bluetooth.a2dp",  "Bluetooth A2DP Capture", NULL}
    },
#ifdef FM_ROUTE_SUPPORT
    {
        {ROUTE_FM,             "alsa.mixer.playback.fm",             "FM", NULL},
        {ROUTE_FM,             "alsa.mixer.capture.fm",              "",   NULL}
    },
#endif
    {
        {static_cast<audio_routes>(0), NULL, NULL, NULL},
        {static_cast<audio_routes>(0), NULL, NULL, NULL}
    }
};

// ----------------------------------------------------------------------------

AudioHardwareALSA::AudioHardwareALSA() :
    mOutput(0),
    mInput(0),
    mAcousticDevice(0)
{
    snd_lib_error_set_handler(&ALSAErrorHandler);
    mMixer = new ALSAMixer;

    hw_module_t *module;
    int err = hw_get_module(ACOUSTICS_HARDWARE_MODULE_ID,
            (hw_module_t const**)&module);

    if (err == 0) {
        hw_device_t* device;
        err = module->methods->open(module, ACOUSTICS_HARDWARE_NAME, &device);
        if (err == 0) {
            mAcousticDevice = (acoustic_device_t *)device;
        }
    }
}

AudioHardwareALSA::~AudioHardwareALSA()
{
    if (mOutput) delete mOutput;
    if (mInput) delete mInput;
    if (mMixer) delete mMixer;
    if (mAcousticDevice)
        mAcousticDevice->common.close(&mAcousticDevice->common);
}

status_t AudioHardwareALSA::initCheck()
{
    if (mMixer && mMixer->isValid())
        return NO_ERROR;
    else
        return NO_INIT;
}

status_t AudioHardwareALSA::setVoiceVolume(float volume)
{
    // The voice volume is used by the VOICE_CALL audio stream.
    if (mMixer)
        return mMixer->setVolume(ROUTE_EARPIECE, volume);
    else
        return INVALID_OPERATION;
}

status_t AudioHardwareALSA::setMasterVolume(float volume)
{
    if (mMixer)
        return mMixer->setMasterVolume(volume);
    else
        return INVALID_OPERATION;
}

// non-default implementation
size_t AudioHardwareALSA::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    /*comment for Mp3 recording*/
 /*
    if (!((sampleRate == 8000) || (sampleRate == 16000)))  {
        LOGW("getInputBufferSize bad sampling rate: %d", sampleRate);
        return 0;
    }
  */
    if (format != AudioSystem::PCM_16_BIT) {
        LOGW("getInputBufferSize bad format: %d", format);
        return 0;
    }
    if (channelCount != 1) {
        LOGW("getInputBufferSize bad channel count: %d", channelCount);
        return 0;
    }

    return 320;
}

AudioStreamOut *
AudioHardwareALSA::openOutputStream(uint32_t devices,
				    int format,
                                    int channelCount,
                                    uint32_t sampleRate,
                                    status_t *status)
{
    AutoMutex lock(mLock);

    // only one output stream allowed
    if (mOutput) {
        *status = ALREADY_EXISTS;
        return 0;
    }

    AudioStreamOutALSA *out = new AudioStreamOutALSA(this);

    *status = out->set(format, channelCount, sampleRate);

    if (*status == NO_ERROR) {
        mOutput = out;
        // Some information is expected to be available immediately after
        // the device is open.
        *status = mOutput->setDevice(mMode, mRoutes[mMode]);
    }
    else {
        delete out;
    }

    return mOutput;
}

status_t AudioHardwareALSA::setInputStream(uint32_t devices,
				    int format,
                                    int channelCount,
                                    uint32_t sampleRate)
{
    status_t ret;

    if (! mOutput) {
       return NO_ERROR; /*no output stream exist*/
    }

    LOGV("setInputStream format %d, channalCount %d, sampleRate %d ", format, channelCount, sampleRate);

    ret = mOutput->getOutStreamParam();
    if(ret){
       LOGV("ERROR in get the OutputStream parameters");
       return NO_INIT;
     }

    ret = mOutput->set(format, channelCount, sampleRate);

    if(ret)
       LOGV("resetOutputStream set Error");

     uint32_t routes = mRoutes[mMode];

     ret = mOutput->setDevice(mMode, routes);

    if(ret)
       LOGV("resetOutputStream setDevice Error");

    return NO_ERROR;
}

status_t AudioHardwareALSA::recoverOutputStream()
{
    LOGV("recoverOutputStream");
    status_t ret;
    if (! mOutput) {
        LOGV("recoverOutputStream set Error");
    return NO_ERROR; /*no output stream exist*/
    }

   ret = mOutput->set((int)OutStreamParam[0], (int)OutStreamParam[1], OutStreamParam[2]);

   if(ret)
     LOGV("recoverOutputStream set Error");
    uint32_t routes = mRoutes[mMode];
     ret = mOutput->setDevice(mMode, routes);
   if(ret)
    LOGV("recoverOutputStream setDevice Error");

    return NO_ERROR;
}


AudioStreamIn *
AudioHardwareALSA::openInputStream(int      inputSource,
                                   int      format,
                                   int      channelCount,
                                   uint32_t sampleRate,
                                   status_t *status,
                                   AudioSystem::audio_in_acoustics acoustics)
{
    // check for valid input source
    if ((inputSource < AudioRecord::DEFAULT_INPUT) ||
        (inputSource >= AudioRecord::NUM_INPUT_SOURCES)) {
        return 0;
    }

    AutoMutex lock(mLock);

    // only one input stream allowed
    if (mInput) {
        *status = ALREADY_EXISTS;
        return 0;
    }
    //for the hardware limitation : only one ssi, so it has the set
    //the streamOut work in the same mode as streamIn
    if (mOutput){
         setInputStream(format,channelCount,sampleRate);
    }

    AudioStreamInALSA *in = new AudioStreamInALSA(this, acoustics);

    *status = in->set(format, channelCount, sampleRate);
    if (*status == NO_ERROR) {
        mInput = in;
        // Some information is expected to be available immediately after
        // the device is open.
        *status = mInput->setDevice(mMode, mRoutes[mMode]);
    }
    else {
        delete in;
    }

    return mInput;
}

status_t AudioHardwareALSA::doRouting()
{
    AutoMutex lock(mLock);

    if (!mOutput)
        return NO_INIT;

    return mOutput->setDevice(mMode, mRoutes[mMode]);
}

status_t AudioHardwareALSA::setMicMute(bool state)
{
    if (mMixer)
        return mMixer->setCaptureMuteState(ROUTE_EARPIECE, state);

    return NO_INIT;
}

status_t AudioHardwareALSA::getMicMute(bool *state)
{
    if (mMixer)
        return mMixer->getCaptureMuteState(ROUTE_EARPIECE, state);

    return NO_ERROR;
}

status_t AudioHardwareALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

// ----------------------------------------------------------------------------

ALSAStreamOps::ALSAStreamOps(AudioHardwareALSA *parent) :
    mParent(parent),
    mHandle(0),
    mHardwareParams(0),
    mSoftwareParams(0),
    mMode(-1),
    mDevice(0),
    mPowerLock(false)
{
    if (snd_pcm_hw_params_malloc(&mHardwareParams) < 0) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA hardware parameters!");
    }

    if (snd_pcm_sw_params_malloc(&mSoftwareParams) < 0) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA software parameters!");
    }
}

ALSAStreamOps::~ALSAStreamOps()
{
    AutoMutex lock(mLock);

    close();

    if (mHardwareParams)
        snd_pcm_hw_params_free(mHardwareParams);

    if (mSoftwareParams)
        snd_pcm_sw_params_free(mSoftwareParams);
}

status_t ALSAStreamOps::set(int      format,
                            int      channels,
                            uint32_t rate)
{
    if (channels > 0)
        mDefaults->channels = channels;

    if (rate > 0)
        mDefaults->sampleRate = rate;

    switch(format) {
      // format == 0
        case AudioSystem::DEFAULT:
            break;

        case AudioSystem::PCM_16_BIT:
            mDefaults->format = SND_PCM_FORMAT_S16_LE;
            break;

        case AudioSystem::PCM_8_BIT:
            mDefaults->format = SND_PCM_FORMAT_S8;
            break;

        default:
            LOGE("Unknown PCM format %i. Forcing default", format);
            break;
    }

    return NO_ERROR;
}

uint32_t ALSAStreamOps::sampleRate() const
{
    unsigned int rate;
    int err;

    if (!mHandle)
        return mDefaults->sampleRate;

    return snd_pcm_hw_params_get_rate(mHardwareParams, &rate, 0) < 0
        ? 0 : static_cast<uint32_t>(rate);
}

status_t ALSAStreamOps::sampleRate(uint32_t rate)
{
    const char *stream;
    unsigned int requestedRate;
    int err;

    if (!mHandle)
        return NO_INIT;

    stream = streamName();
    requestedRate = rate;
    err = snd_pcm_hw_params_set_rate_near(mHandle,
                                          mHardwareParams,
                                          &requestedRate,
                                          0);

    if (err < 0) {
        LOGE("Unable to set %s sample rate to %u: %s",
            stream, rate, snd_strerror(err));
        return BAD_VALUE;
    }
    if (requestedRate != rate) {
        // Some devices have a fixed sample rate, and can not be changed.
        // This may cause resampling problems; i.e. PCM playback will be too
        // slow or fast.
        LOGW("Requested rate (%u HZ) does not match actual rate (%u HZ)",
            rate, requestedRate);
    }
    else {
        LOGV("Set %s sample rate to %u HZ", stream, requestedRate);
    }
    return NO_ERROR;
}

//
// Return the number of bytes (not frames)
//
size_t ALSAStreamOps::bufferSize() const
{
    if (!mHandle)
        return NO_INIT;

    snd_pcm_uframes_t bufferSize = 0;
    snd_pcm_uframes_t periodSize = 0;
    int err;

    err = snd_pcm_get_params(mHandle, &bufferSize, &periodSize);

    if (err < 0)
        return -1;

    size_t bytes = static_cast<size_t>(snd_pcm_frames_to_bytes(mHandle, bufferSize));

    // Not sure when this happened, but unfortunately it now
    // appears that the bufferSize must be reported as a
    // power of 2. This might be the fault of 3rd party
    // users.
    for (size_t i = 1; (bytes & ~i) != 0; i<<=1)
        bytes &= ~i;

    return bytes;
}

int ALSAStreamOps::format() const
{
    snd_pcm_format_t ALSAFormat;
    int pcmFormatBitWidth;
    int audioSystemFormat;

    if (!mHandle)
        return -1;

    if (snd_pcm_hw_params_get_format(mHardwareParams, &ALSAFormat) < 0) {
        return -1;
    }

    pcmFormatBitWidth = snd_pcm_format_physical_width(ALSAFormat);
    audioSystemFormat = AudioSystem::DEFAULT;
    switch(pcmFormatBitWidth) {
        case 8:
            audioSystemFormat = AudioSystem::PCM_8_BIT;
            break;

        case 16:
            audioSystemFormat = AudioSystem::PCM_16_BIT;
            break;

        default:
            LOG_FATAL("Unknown AudioSystem bit width %i!", pcmFormatBitWidth);
    }

    return audioSystemFormat;
}

int ALSAStreamOps::channelCount() const
{
    if (!mHandle)
        return mDefaults->channels;

    unsigned int val;
    int err;

    err = snd_pcm_hw_params_get_channels(mHardwareParams, &val);
    if (err < 0) {
        LOGE("Unable to get device channel count: %s",
            snd_strerror(err));
        return mDefaults->channels;
    }

    return val;
}

status_t ALSAStreamOps::channelCount(int channels) {
    int err;

    if (!mHandle)
        return NO_INIT;

    err = snd_pcm_hw_params_set_channels(mHandle, mHardwareParams, channels);
    if (err < 0) {
        LOGE("Unable to set channel count to %i: %s",
            channels, snd_strerror(err));
        return BAD_VALUE;
    }

    LOGV("Using %i %s for %s.",
        channels, channels == 1 ? "channel" : "channels", streamName());

    return NO_ERROR;
}

status_t ALSAStreamOps::open(int mode, uint32_t device)
{
    const char *stream = streamName();
    const char *devName = deviceName(mode, device);

    int         err;

    for(;;) {
        // The PCM stream is opened in blocking mode, per ALSA defaults.  The
        // AudioFlinger seems to assume blocking mode too, so asynchronous mode
        // should not be used.
        err = snd_pcm_open(&mHandle, devName, mDefaults->direction, 0);
        if (err == 0) break;

        // See if there is a less specific name we can try.
        // Note: We are changing the contents of a const char * here.
        char *tail = strrchr(devName, '_');
        if (!tail) break;
        *tail = 0;
    }

    if (err < 0) {
        // None of the Android defined audio devices exist. Open a generic one.
        devName = "default";
        err = snd_pcm_open(&mHandle, devName, mDefaults->direction, 0);
        if (err < 0) {
            // Last resort is the NULL device (i.e. the bit bucket).
            devName = _nullALSADeviceName;
            err = snd_pcm_open(&mHandle, devName, mDefaults->direction, 0);
        }
    }

    mMode   = mode;
    mDevice = device;

    LOGI("Initialized ALSA %s device %s", stream, devName);
    return err;
}

void ALSAStreamOps::close()
{
    snd_pcm_t *handle = mHandle;
    mHandle = NULL;

    if (handle)
        snd_pcm_close(handle);
}

status_t ALSAStreamOps::setSoftwareParams()
{
    if (!mHandle)
        return NO_INIT;

    int err;

    // Get the current software parameters
    err = snd_pcm_sw_params_current(mHandle, mSoftwareParams);
    if (err < 0) {
        LOGE("Unable to get software parameters: %s", snd_strerror(err));
        return NO_INIT;
    }

    snd_pcm_uframes_t bufferSize = 0;
    snd_pcm_uframes_t periodSize = 0;
    snd_pcm_uframes_t startThreshold, stopThreshold;

    // Configure ALSA to start the transfer when the buffer is almost full.
    snd_pcm_get_params(mHandle, &bufferSize, &periodSize);

    if (mDefaults->direction == SND_PCM_STREAM_PLAYBACK) {
        // For playback, configure ALSA to start the transfer when the
        // buffer is full.
        startThreshold = bufferSize - periodSize;
        stopThreshold = bufferSize;
    }
    else {
        // For recording, configure ALSA to start the transfer on the
        // first frame.
        startThreshold = 1;
        stopThreshold = bufferSize;
}

    err = snd_pcm_sw_params_set_start_threshold(mHandle,
                                                mSoftwareParams,
                                                startThreshold);
    if (err < 0) {
        LOGE("Unable to set start threshold to %lu frames: %s",
            startThreshold, snd_strerror(err));
        return NO_INIT;
    }

    err = snd_pcm_sw_params_set_stop_threshold(mHandle,
                                               mSoftwareParams,
                                               stopThreshold);
    if (err < 0) {
        LOGE("Unable to set stop threshold to %lu frames: %s",
            stopThreshold, snd_strerror(err));
        return NO_INIT;
    }

    // Allow the transfer to start when at least periodSize samples can be
    // processed.
    err = snd_pcm_sw_params_set_avail_min(mHandle,
                                          mSoftwareParams,
                                          periodSize);
    if (err < 0) {
        LOGE("Unable to configure available minimum to %lu: %s",
            periodSize, snd_strerror(err));
        return NO_INIT;
    }

    // Commit the software parameters back to the device.
    err = snd_pcm_sw_params(mHandle, mSoftwareParams);
    if (err < 0) {
        LOGE("Unable to configure software parameters: %s",
            snd_strerror(err));
        return NO_INIT;
    }

    return NO_ERROR;
}

status_t ALSAStreamOps::setPCMFormat(snd_pcm_format_t format)
{
    const char *formatDesc;
    const char *formatName;
    bool validFormat;
    int err;

    // snd_pcm_format_description() and snd_pcm_format_name() do not perform
    // proper bounds checking.
    validFormat = (static_cast<int>(format) > SND_PCM_FORMAT_UNKNOWN) &&
        (static_cast<int>(format) <= SND_PCM_FORMAT_LAST);
    formatDesc = validFormat ?
        snd_pcm_format_description(format) : "Invalid Format";
    formatName = validFormat ?
        snd_pcm_format_name(format) : "UNKNOWN";

    err = snd_pcm_hw_params_set_format(mHandle, mHardwareParams, format);
    if (err < 0) {
        LOGE("Unable to configure PCM format %s (%s): %s",
            formatName, formatDesc, snd_strerror(err));
        return NO_INIT;
    }

    LOGV("Set %s PCM format to %s (%s)", streamName(), formatName, formatDesc);
    return NO_ERROR;
}

status_t ALSAStreamOps::setHardwareResample(bool resample)
{
    int err;

    err = snd_pcm_hw_params_set_rate_resample(mHandle,
                                              mHardwareParams,
                                              static_cast<int>(resample));
    if (err < 0) {
        LOGE("Unable to %s hardware resampling: %s",
            resample ? "enable" : "disable",
            snd_strerror(err));
        return NO_INIT;
    }
    return NO_ERROR;
}

const char *ALSAStreamOps::streamName()
{
    // Don't use snd_pcm_stream(mHandle), as the PCM stream may not be
    // opened yet.  In such case, snd_pcm_stream() will abort().
    return snd_pcm_stream_name(mDefaults->direction);
}

//
// Set playback or capture PCM device.  It's possible to support audio output
// or input from multiple devices by using the ALSA plugins, but this is
// not supported for simplicity.
//
// The AudioHardwareALSA API does not allow one to set the input routing.
//
// If the "routes" value does not map to a valid device, the default playback
// device is used.
//
status_t ALSAStreamOps::setDevice(int mode, uint32_t device)
{
    // Close off previously opened device.
    // It would be nice to determine if the underlying device actually
    // changes, but we might be manipulating mixer settings (see asound.conf).
    //
    close();

    const char *stream = streamName();

    status_t    status = open (mode, device);
    int     err;

    if (status != NO_ERROR)
        return status;

    err = snd_pcm_hw_params_any(mHandle, mHardwareParams);
    if (err < 0) {
        LOGE("Unable to configure hardware: %s", snd_strerror(err));
        return NO_INIT;
    }

    // Set the interleaved read and write format.
    err = snd_pcm_hw_params_set_access(mHandle, mHardwareParams,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        LOGE("Unable to configure PCM read/write format: %s",
            snd_strerror(err));
        return NO_INIT;
    }

    status = setPCMFormat(mDefaults->format);

    //
    // Some devices do not have the default two channels.  Force an error to
    // prevent AudioMixer from crashing and taking the whole system down.
    //
    // Note that some devices will return an -EINVAL if the channel count
    // is queried before it has been set.  i.e. calling channelCount()
    // before channelCount(channels) may return -EINVAL.
    //
    status = channelCount(mDefaults->channels);
    if (status != NO_ERROR)
        return status;

    // Don't check for failure; some devices do not support the default
    // sample rate.
    sampleRate(mDefaults->sampleRate);

#ifdef DISABLE_HARWARE_RESAMPLING
    // Disable hardware resampling.
    status = setHardwareResample(false);
    if (status != NO_ERROR)
        return status;
#endif

    snd_pcm_uframes_t bufferSize = mDefaults->bufferSize;

    // Make sure we have at least the size we originally wanted
    err = snd_pcm_hw_params_set_buffer_size(mHandle, mHardwareParams, bufferSize);
    if (err < 0) {
        LOGE("Unable to set buffer size to %d:  %s",
             (int)bufferSize, snd_strerror(err));
        return NO_INIT;
    }

    unsigned int latency = mDefaults->latency;

    // Setup buffers for latency
    err = snd_pcm_hw_params_set_buffer_time_near (mHandle, mHardwareParams,
                                                  &latency, NULL);
    if (err < 0) {
        /* That didn't work, set the period instead */
        unsigned int periodTime = latency / 4;
        err = snd_pcm_hw_params_set_period_time_near (mHandle, mHardwareParams,
                                                      &periodTime, NULL);
        if (err < 0) {
            LOGE("Unable to set the period time for latency: %s", snd_strerror(err));
            return NO_INIT;
        }
        snd_pcm_uframes_t periodSize;
        err = snd_pcm_hw_params_get_period_size (mHardwareParams, &periodSize, NULL);
        if (err < 0) {
            LOGE("Unable to get the period size for latency: %s", snd_strerror(err));
            return NO_INIT;
        }
        bufferSize = periodSize * 4;
        if (bufferSize < mDefaults->bufferSize)
            bufferSize = mDefaults->bufferSize;
        err = snd_pcm_hw_params_set_buffer_size_near (mHandle, mHardwareParams, &bufferSize);
        if (err < 0) {
            LOGE("Unable to set the buffer size for latency: %s", snd_strerror(err));
            return NO_INIT;
        }
    } else {
        // OK, we got buffer time near what we expect. See what that did for bufferSize.
        err = snd_pcm_hw_params_get_buffer_size (mHardwareParams, &bufferSize);
        if (err < 0) {
            LOGE("Unable to get the buffer size for latency: %s", snd_strerror(err));
            return NO_INIT;
        }
        // Does set_buffer_time_near change the passed value? It should.
        err = snd_pcm_hw_params_get_buffer_time (mHardwareParams, &latency, NULL);
        if (err < 0) {
            LOGE("Unable to get the buffer time for latency: %s", snd_strerror(err));
            return NO_INIT;
        }
        unsigned int periodTime = latency / 4;
        err = snd_pcm_hw_params_set_period_time_near (mHandle, mHardwareParams,
                                                      &periodTime, NULL);
        if (err < 0) {
            LOGE("Unable to set the period time for latency: %s", snd_strerror(err));
            return NO_INIT;
        }
    }

    LOGV("Buffer size: %d", (int)bufferSize);
    LOGV("Latency: %d", (int)latency);

    mDefaults->bufferSize = bufferSize;
    mDefaults->latency = latency;

    // Commit the hardware parameters back to the device.
    err = snd_pcm_hw_params(mHandle, mHardwareParams);
    if (err < 0) {
        LOGE("Unable to set hardware parameters: %s", snd_strerror(err));
        return NO_INIT;
    }

    status = setSoftwareParams();

    return status;
}

const char *ALSAStreamOps::deviceName(int mode, uint32_t device)
{
    static char devString[ALSA_NAME_MAX];
    int hasDevExt = 0;

    strcpy (devString, mDefaults->devicePrefix);

    for (int dev=0; device; dev++)
        if (device & (1 << dev)) {
            /* Don't go past the end of our list */
            if (dev >= deviceSuffixLen)
                break;
            ALSA_STRCAT (devString, deviceSuffix[dev]);
            device &= ~(1 << dev);
            hasDevExt = 1;
        }

    if (hasDevExt)
        switch (mode) {
            case AudioSystem::MODE_NORMAL:
                ALSA_STRCAT (devString, "_normal");
                break;
            case AudioSystem::MODE_RINGTONE:
                ALSA_STRCAT (devString, "_ringtone");
                break;
            case AudioSystem::MODE_IN_CALL:
                ALSA_STRCAT (devString, "_incall");
                break;
        };
   switch(mDefaults->sampleRate){
    case 32000:
      ALSA_STRCAT (devString, "_32000");
      break;
    case 44100:
      ALSA_STRCAT (devString, "_44100");
      break;
    case 48000:
      ALSA_STRCAT (devString, "_48000");
      break;
      case 96000:
        ALSA_STRCAT (devString, "_96000");
      break;
    default :
    {
      if (mDefaults->sampleRate < 32000)
         {
            ALSA_STRCAT (devString, "_32000");
          }
      else if (mDefaults->sampleRate < 44100)
        {
          ALSA_STRCAT (devString, "_44100");
        }
      else if (mDefaults->sampleRate < 48000)
        {
          ALSA_STRCAT (devString, "_48000");
        }
      else
        {
          ALSA_STRCAT (devString, "_96000");
        }
      break;
    }
    }

    return devString;
}

// ----------------------------------------------------------------------------

AudioStreamOutALSA::AudioStreamOutALSA(AudioHardwareALSA *parent) :
    ALSAStreamOps(parent)
{
    static StreamDefaults _defaults = {
        devicePrefix   : "AndroidPlayback",
        direction      : SND_PCM_STREAM_PLAYBACK,
        format         : SND_PCM_FORMAT_S16_LE,   // AudioSystem::PCM_16_BIT
        channels       : 2,
        sampleRate     : DEFAULT_SAMPLE_RATE,
        latency        : 250000,                  // Desired Delay in usec
        bufferSize     : 6144,                   // Desired Number of samples
        };

    setStreamDefaults(&_defaults);

    snd_pcm_uframes_t bufferSize = mDefaults->bufferSize;

    // See comment in bufferSize() method.
    for (size_t i = 1; (bufferSize & ~i) != 0; i<<=1)
        bufferSize &= ~i;

    mDefaults->bufferSize = bufferSize;
}

AudioStreamOutALSA::~AudioStreamOutALSA()
{
    standby();
    mParent->mOutput = NULL;
}

int AudioStreamOutALSA::channelCount() const
{
    int c = ALSAStreamOps::channelCount();

    // AudioMixer will seg fault if it doesn't have two channels.
    LOGW_IF(c != 2,
        "AudioMixer expects two channels, but only %i found!", c);
    return c;
}

status_t AudioStreamOutALSA::setVolume(float volume)
{
    if (!mParent->mMixer || !mDevice)
        return NO_INIT;

    return mParent->mMixer->setVolume (mDevice, volume);
}

ssize_t AudioStreamOutALSA::write(const void *buffer, size_t bytes)
{
    snd_pcm_sframes_t n;
    size_t            sent = 0;
    status_t          err;

    AutoMutex lock(mLock);

    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioOutLock");
        mPowerLock = true;
    }

    if (!mHandle)
        ALSAStreamOps::setDevice(mMode, mDevice);

    do {
        n = snd_pcm_writei(mHandle,
                           (char *)buffer + sent,
                           snd_pcm_bytes_to_frames(mHandle, bytes));
        if (n == -EBADFD) {
            // Somehow the stream is in a bad state. The driver probably
            // has a bug and snd_pcm_recover() doesn't seem to handle this.
            ALSAStreamOps::setDevice(mMode, mDevice);
        }
        else if (n < 0) {
            if (mHandle) {
                // snd_pcm_recover() will return 0 if successful in recovering from
                // an error, or -errno if the error was unrecoverable.
                n = snd_pcm_recover(mHandle, n, 1);
                if (n)
                    return static_cast<ssize_t>(n);
            }
        }
        else
            sent += static_cast<ssize_t>(snd_pcm_frames_to_bytes(mHandle, n));

    } while (mHandle && sent < bytes);

    return sent;
}

status_t AudioStreamOutALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}
status_t AudioStreamOutALSA::getOutStreamParam()
{
    if(!mParent)
        return NO_INIT;
    if(!mParent->OutStreamParam)
        return NO_INIT;
    if(!(ALSAStreamOps::mDefaults))
        return NO_INIT;
    mParent->OutStreamParam[0] =  ALSAStreamOps::mDefaults->format;
    mParent->OutStreamParam[1] =  ALSAStreamOps::mDefaults->channels;
    mParent->OutStreamParam[2] =  ALSAStreamOps::mDefaults->sampleRate;
    return NO_ERROR;
}

status_t AudioStreamOutALSA::setDevice(int mode, uint32_t newDevice)
{
    AutoMutex lock(mLock);
    return ALSAStreamOps::setDevice(mode, newDevice);
}

status_t AudioStreamOutALSA::standby()
{
    AutoMutex lock(mLock);

    if (mHandle) {
        snd_pcm_drain (mHandle);
        close();
    }

    if (mPowerLock) {
        release_wake_lock ("AudioOutLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

#define USEC_TO_MSEC(x) ((x + 999) / 1000)

uint32_t AudioStreamOutALSA::latency() const
{
    // Android wants latency in milliseconds.
    return USEC_TO_MSEC (mDefaults->latency);
}

// ----------------------------------------------------------------------------

AudioStreamInALSA::AudioStreamInALSA(AudioHardwareALSA *parent,
                                     AudioSystem::audio_in_acoustics acoustics) :
    ALSAStreamOps(parent),
    mAcoustics(acoustics)
{
    static StreamDefaults _defaults = {
        devicePrefix   : "AndroidRecord",
        direction      : SND_PCM_STREAM_CAPTURE,
        format         : SND_PCM_FORMAT_S16_LE,   // AudioSystem::PCM_16_BIT
        channels       : 1,
        sampleRate     : AudioRecord::DEFAULT_SAMPLE_RATE,
        latency        : 250000,                  // Desired Delay in usec
        bufferSize     : 3072,                   // Desired Number of samples
        };

    setStreamDefaults(&_defaults);
}

AudioStreamInALSA::~AudioStreamInALSA()
{
    standby();
    mParent->mInput = NULL;
    mParent->recoverOutputStream();
}

status_t AudioStreamInALSA::setGain(float gain)
{
    if (mParent->mMixer)
        return mParent->mMixer->setMasterGain (gain);
    else
        return NO_INIT;
}

ssize_t AudioStreamInALSA::read(void *buffer, ssize_t bytes)
{
    snd_pcm_sframes_t n, frames = snd_pcm_bytes_to_frames(mHandle, bytes);
    status_t          err;

    AutoMutex lock(mLock);

    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioInLock");
        mPowerLock = true;
    }

    if (!mHandle)
        ALSAStreamOps::setDevice(mMode, mDevice);

    n = snd_pcm_readi(mHandle, buffer, frames);
    if (n < frames) {
        if (mHandle) {
            if (n < 0)
                n = snd_pcm_recover(mHandle, n, 0);
            else
                n = snd_pcm_prepare(mHandle);
        }
        return static_cast<ssize_t>(n);
    }

    if (mParent->mAcousticDevice &&
        mParent->mAcousticDevice->filter) {
        n = mParent->mAcousticDevice->filter(mHandle, buffer, frames);
        if (n < 0)
            return static_cast<ssize_t>(n);
    }

    return static_cast<ssize_t>(snd_pcm_frames_to_bytes(mHandle, n));
}

status_t AudioStreamInALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioStreamInALSA::setDevice(int mode, uint32_t newDevice)
{
    AutoMutex lock(mLock);

    status_t status = ALSAStreamOps::setDevice(mode, newDevice);

    if (status == NO_ERROR && mParent->mAcousticDevice)
        status = mParent->mAcousticDevice->set_acoustics(mHandle, mAcoustics);

    return status;
}

status_t AudioStreamInALSA::standby()
{
    AutoMutex lock(mLock);

    close();

    if (mPowerLock) {
        release_wake_lock ("AudioInLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

// ----------------------------------------------------------------------------

struct mixer_info_t
{
    mixer_info_t() :
        elem(0),
        min(SND_MIXER_VOL_RANGE_MIN),
        max(SND_MIXER_VOL_RANGE_MAX),
        mute(false)
    {
    }

    snd_mixer_elem_t *elem;
    long              min;
    long              max;
    long              volume;
    bool              mute;
    char              name[ALSA_NAME_MAX];
};

static int initMixer (snd_mixer_t **mixer, const char *name)
{
    int err;

    if ((err = snd_mixer_open(mixer, 0)) < 0) {
        LOGE("Unable to open mixer: %s", snd_strerror(err));
        return err;
    }

    if ((err = snd_mixer_attach(*mixer, name)) < 0) {
        LOGE("Unable to attach mixer to device %s: %s",
            name, snd_strerror(err));

        if ((err = snd_mixer_attach(*mixer, "hw:00")) < 0) {
            LOGE("Unable to attach mixer to device default: %s",
                snd_strerror(err));

            snd_mixer_close (*mixer);
            *mixer = NULL;
            return err;
        }
    }

    if ((err = snd_mixer_selem_register(*mixer, NULL, NULL)) < 0) {
        LOGE("Unable to register mixer elements: %s", snd_strerror(err));
        snd_mixer_close (*mixer);
        *mixer = NULL;
        return err;
    }

    // Get the mixer controls from the kernel
    if ((err = snd_mixer_load(*mixer)) < 0) {
        LOGE("Unable to load mixer elements: %s", snd_strerror(err));
        snd_mixer_close (*mixer);
        *mixer = NULL;
        return err;
    }

    return 0;
}

typedef int (*hasVolume_t)(snd_mixer_elem_t*);

static const hasVolume_t hasVolume[] = {
    snd_mixer_selem_has_playback_volume,
    snd_mixer_selem_has_capture_volume
};

typedef int (*getVolumeRange_t)(snd_mixer_elem_t*, long int*, long int*);

static const getVolumeRange_t getVolumeRange[] = {
    snd_mixer_selem_get_playback_volume_range,
    snd_mixer_selem_get_capture_volume_range
};

typedef int (*setVolume_t)(snd_mixer_elem_t*, long int);

static const setVolume_t setVol[] = {
    snd_mixer_selem_set_playback_volume_all,
    snd_mixer_selem_set_capture_volume_all
};

ALSAMixer::ALSAMixer()
{
    int err;

    initMixer (&mMixer[SND_PCM_STREAM_PLAYBACK], "AndroidPlayback");
    initMixer (&mMixer[SND_PCM_STREAM_CAPTURE], "AndroidRecord");

    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);

    for (int i = 0; i <= SND_PCM_STREAM_LAST; i++) {

        mixer_info_t *info = mixerMasterProp[i].mInfo = new mixer_info_t;

        property_get (mixerMasterProp[i].propName,
                      info->name,
                      mixerMasterProp[i].propDefault);

        for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mMixer[i]);
             elem;
             elem = snd_mixer_elem_next(elem)) {

            if (!snd_mixer_selem_is_active(elem))
                continue;

            snd_mixer_selem_get_id(elem, sid);

            // Find PCM playback volume control element.
            const char *elementName = snd_mixer_selem_id_get_name(sid);

            if (info->elem == NULL &&
                strcmp(elementName, info->name) == 0 &&
                hasVolume[i] (elem)) {

                info->elem = elem;
                getVolumeRange[i] (elem, &info->min, &info->max);
                info->volume = info->max;
                setVol[i] (elem, info->volume);
                if (i == SND_PCM_STREAM_PLAYBACK &&
                    snd_mixer_selem_has_playback_switch (elem))
                    snd_mixer_selem_set_playback_switch_all (elem, 1);
                break;
            }
        }

        LOGV("Mixer: master '%s' %s.", info->name, info->elem ? "found" : "not found");

        for (int j = 0; mixerProp[j][i].routes; j++) {

            mixer_info_t *info = mixerProp[j][i].mInfo = new mixer_info_t;

            property_get (mixerProp[j][i].propName,
                          info->name,
                          mixerProp[j][i].propDefault);

            for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mMixer[i]);
                 elem;
                 elem = snd_mixer_elem_next(elem)) {

                if (!snd_mixer_selem_is_active(elem))
                    continue;

                snd_mixer_selem_get_id(elem, sid);

                // Find PCM playback volume control element.
                const char *elementName = snd_mixer_selem_id_get_name(sid);

               if (info->elem == NULL &&
                    strcmp(elementName, info->name) == 0 &&
                    hasVolume[i] (elem)) {

                    info->elem = elem;
                    getVolumeRange[i] (elem, &info->min, &info->max);
                    info->volume = info->max;
                    setVol[i] (elem, info->volume);
                    if (i == SND_PCM_STREAM_PLAYBACK &&
                        snd_mixer_selem_has_playback_switch (elem))
                        snd_mixer_selem_set_playback_switch_all (elem, 1);
                    break;
                }
            }
            LOGV("Mixer: route '%s' %s.", info->name, info->elem ? "found" : "not found");
        }
    }
    LOGV("mixer initialized.");
}

ALSAMixer::~ALSAMixer()
{
    for (int i = 0; i <= SND_PCM_STREAM_LAST; i++) {
        if (mMixer[i]) snd_mixer_close (mMixer[i]);
        if (mixerMasterProp[i].mInfo) {
            delete mixerMasterProp[i].mInfo;
            mixerMasterProp[i].mInfo = NULL;
        }
        for (int j = 0; mixerProp[j][i].routes; j++) {
            if (mixerProp[j][i].mInfo) {
                delete mixerProp[j][i].mInfo;
                mixerProp[j][i].mInfo = NULL;
            }
        }
    }
    LOGV("mixer destroyed.");
}

status_t ALSAMixer::setMasterVolume(float volume)
{
    mixer_info_t *info = mixerMasterProp[SND_PCM_STREAM_PLAYBACK].mInfo;
    if (!info || !info->elem) return INVALID_OPERATION;

    long minVol = info->min;
    long maxVol = info->max;

    // Make sure volume is between bounds.
    long vol = minVol + volume * (maxVol - minVol);
    if (vol > maxVol) vol = maxVol;
    if (vol < minVol) vol = minVol;

    info->volume = vol;
    snd_mixer_selem_set_playback_volume_all (info->elem, vol);

    return NO_ERROR;
}

status_t ALSAMixer::setMasterGain(float gain)
{
    mixer_info_t *info = mixerMasterProp[SND_PCM_STREAM_CAPTURE].mInfo;
    if (!info || !info->elem) return INVALID_OPERATION;

    long minVol = info->min;
    long maxVol = info->max;

    // Make sure volume is between bounds.
    long vol = minVol + gain * (maxVol - minVol);
    if (vol > maxVol) vol = maxVol;
    if (vol < minVol) vol = minVol;

    info->volume = vol;
    snd_mixer_selem_set_capture_volume_all (info->elem, vol);

    return NO_ERROR;
}

status_t ALSAMixer::setVolume(uint32_t device, float volume)
{
    for (int j = 0; mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_PLAYBACK].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            long minVol = info->min;
            long maxVol = info->max;

            // Make sure volume is between bounds.
            long vol = minVol + volume * (maxVol - minVol);
            if (vol > maxVol) vol = maxVol;
            if (vol < minVol) vol = minVol;

            info->volume = vol;
            snd_mixer_selem_set_playback_volume_all (info->elem, vol);
        }

    return NO_ERROR;
}

status_t ALSAMixer::setGain(uint32_t device, float gain)
{
    for (int j = 0; mixerProp[j][SND_PCM_STREAM_CAPTURE].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_CAPTURE].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_CAPTURE].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            long minVol = info->min;
            long maxVol = info->max;

            // Make sure volume is between bounds.
            long vol = minVol + gain * (maxVol - minVol);
            if (vol > maxVol) vol = maxVol;
            if (vol < minVol) vol = minVol;

            info->volume = vol;
            snd_mixer_selem_set_capture_volume_all (info->elem, vol);
        }

    return NO_ERROR;
}

status_t ALSAMixer::setCaptureMuteState(uint32_t device, bool state)
{
    for (int j = 0; mixerProp[j][SND_PCM_STREAM_CAPTURE].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_CAPTURE].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_CAPTURE].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            if (snd_mixer_selem_has_capture_switch (info->elem)) {

                int err = snd_mixer_selem_set_capture_switch_all (info->elem, static_cast<int>(!state));
                if (err < 0) {
                    LOGE("Unable to %s capture mixer switch %s",
                        state ? "enable" : "disable", info->name);
                    return INVALID_OPERATION;
                }
            }

            info->mute = state;
        }

    return NO_ERROR;
}

status_t ALSAMixer::getCaptureMuteState(uint32_t device, bool *state)
{
    if (!state) return BAD_VALUE;

    for (int j = 0; mixerProp[j][SND_PCM_STREAM_CAPTURE].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_CAPTURE].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_CAPTURE].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            *state = info->mute;
            return NO_ERROR;
        }

    return BAD_VALUE;
}

status_t ALSAMixer::setPlaybackMuteState(uint32_t device, bool state)
{
    for (int j = 0; mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_PLAYBACK].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            if (snd_mixer_selem_has_playback_switch (info->elem)) {

                int err = snd_mixer_selem_set_playback_switch_all (info->elem, static_cast<int>(!state));
                if (err < 0) {
                    LOGE("Unable to %s playback mixer switch %s",
                        state ? "enable" : "disable", info->name);
                    return INVALID_OPERATION;
                }
            }

            info->mute = state;
        }

    return NO_ERROR;
}

status_t ALSAMixer::getPlaybackMuteState(uint32_t device, bool *state)
{
    if (!state) return BAD_VALUE;

    for (int j = 0; mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_PLAYBACK].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            *state = info->mute;
            return NO_ERROR;
        }

    return BAD_VALUE;
}

// ----------------------------------------------------------------------------

ALSAControl::ALSAControl(const char *device)
{
    snd_ctl_open(&mHandle, device, 0);
}

ALSAControl::~ALSAControl()
{
    if (mHandle) snd_ctl_close(mHandle);
}

status_t ALSAControl::get(const char *name, unsigned int &value, int index)
{
    if (!mHandle) return NO_INIT;

    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *control;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&control);

    snd_ctl_elem_id_set_name(id, name);
    snd_ctl_elem_info_set_id(info, id);

    int ret = snd_ctl_elem_info(mHandle, info);
    if (ret < 0) return BAD_VALUE;

    snd_ctl_elem_info_get_id(info, id);
    snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
    unsigned int count = snd_ctl_elem_info_get_count(info);
    if ((unsigned int)index >= count) return BAD_VALUE;

    snd_ctl_elem_value_set_id(control, id);

    ret = snd_ctl_elem_read(mHandle, control);
    if (ret < 0) return BAD_VALUE;

    switch (type) {
        case SND_CTL_ELEM_TYPE_BOOLEAN:
            value = snd_ctl_elem_value_get_boolean(control, index);
            break;
        case SND_CTL_ELEM_TYPE_INTEGER:
            value = snd_ctl_elem_value_get_integer(control, index);
            break;
        case SND_CTL_ELEM_TYPE_INTEGER64:
            value = snd_ctl_elem_value_get_integer64(control, index);
            break;
        case SND_CTL_ELEM_TYPE_ENUMERATED:
            value = snd_ctl_elem_value_get_enumerated(control, index);
            break;
        case SND_CTL_ELEM_TYPE_BYTES:
            value = snd_ctl_elem_value_get_byte(control, index);
            break;
        default:
            return BAD_VALUE;
    }

    return NO_ERROR;
}

status_t ALSAControl::set(const char *name, unsigned int value, int index)
{
    if (!mHandle) return NO_INIT;

    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *control;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&control);

    snd_ctl_elem_id_set_name(id, name);
    snd_ctl_elem_info_set_id(info, id);

    int ret = snd_ctl_elem_info(mHandle, info);
    if (ret < 0) return BAD_VALUE;

    snd_ctl_elem_info_get_id(info, id);
    snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
    unsigned int count = snd_ctl_elem_info_get_count(info);
    if ((unsigned int)index >= count) return BAD_VALUE;

    if (index == -1)
        index = 0; // Range over all of them
    else
        count = index + 1; // Just do the one specified

    snd_ctl_elem_value_set_id(control, id);

    for (unsigned int i = index; i < count; i++)
        switch (type) {
            case SND_CTL_ELEM_TYPE_BOOLEAN:
                snd_ctl_elem_value_set_boolean(control, i, value);
                break;
            case SND_CTL_ELEM_TYPE_INTEGER:
                snd_ctl_elem_value_set_integer(control, i, value);
                break;
            case SND_CTL_ELEM_TYPE_INTEGER64:
                snd_ctl_elem_value_set_integer64(control, i, value);
                break;
            case SND_CTL_ELEM_TYPE_ENUMERATED:
                snd_ctl_elem_value_set_enumerated(control, i, value);
                break;
            case SND_CTL_ELEM_TYPE_BYTES:
                snd_ctl_elem_value_set_byte(control, i, value);
                break;
            default:
                break;
        }

    ret = snd_ctl_elem_write(mHandle, control);
    return (ret < 0) ? BAD_VALUE : NO_ERROR;
}

// ----------------------------------------------------------------------------

};        // namespace android
