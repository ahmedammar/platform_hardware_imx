/* Copyright (c) 2011 Freescale Semiconductor Inc. */

#define LOG_TAG "Sensor"

#include <hardware/hardware.h>
#include <hardware/sensors.h>

#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <poll.h>
#include <linux/input.h>
#include <utils/Log.h>
#include <cutils/atomic.h>


// 720 LSG = 1G
#define LSG                         (720.0f)

// conversion of acceleration data to SI units (m/s^2)
//#define CONVERT_A                   (GRAVITY_EARTH / LSG)
#define CONVERT_A                   1
#define CONVERT_A_X                 (CONVERT_A)
#define CONVERT_A_Y                 (-CONVERT_A)
#define CONVERT_A_Z                 (CONVERT_A)

/* sensor rate in me */
#define SENSORS_RATE_MS     20
/* timeout (constant value) in ms */
#define SENSORS_TIMEOUT_MS  100
/* # of samples to look at in the past for filtering */
#define COUNT               24
/* prediction ratio */
#define PREDICTION_RATIO    (1.0f/3.0f)
/* prediction time in seconds (>=0) */
#define PREDICTION_TIME     ((SENSORS_RATE_MS*COUNT/1000.0f)*PREDICTION_RATIO)

static float mV[COUNT*2];
static float mT[COUNT*2];
static int mIndex;

static inline
float normalize(float x)
{
    x *= (1.0f / 360.0f);
    if (fabsf(x) >= 0.5f)
        x = x - ceilf(x + 0.5f) + 1.0f;
    if (x < 0)
        x += 1.0f;
    x *= 360.0f;
    return x;
}

static void LMSInit(void)
{
    memset(mV, 0, sizeof(mV));
    memset(mT, 0, sizeof(mT));
    mIndex = COUNT;
}

static float LMSFilter(int64_t time, int v)
{
    const float ns = 1.0f / 1000000000.0f;
    const float t = time*ns;
    float v1 = mV[mIndex];
    if ((v-v1) > 180) {
        v -= 360;
    } else if ((v1-v) > 180) {
        v += 360;
    }
    /* Manage the circular buffer, we write the data twice spaced by COUNT
     * values, so that we don't have to memcpy() the array when it's full */
    mIndex++;
    if (mIndex >= COUNT*2)
        mIndex = COUNT;
    mV[mIndex] = v;
    mT[mIndex] = t;
    mV[mIndex-COUNT] = v;
    mT[mIndex-COUNT] = t;

    float A, B, C, D, E;
    float a, b;
    int i;

    A = B = C = D = E = 0;
    for (i=0 ; i<COUNT-1 ; i++) {
        const int j = mIndex - 1 - i;
        const float Z = mV[j];
        const float T = 0.5f*(mT[j] + mT[j+1]) - t;
        float dT = mT[j] - mT[j+1];
        dT *= dT;
        A += Z*dT;
        B += T*(T*dT);
        C +=   (T*dT);
        D += Z*(T*dT);
        E += dT;
    }
    b = (A*B + C*D) / (E*B + C*C);
    a = (E*b - A) / C;
    float f = b + PREDICTION_TIME*a;

    //LOGD("A=%f, B=%f, C=%f, D=%f, E=%f", A,B,C,D,E);
    //LOGD("%lld  %d  %f  %f", time, v, f, a);

    f = normalize(f);
    return f;
}

/*****************************************************************************/

struct sensors_control_context_t {
    struct sensors_control_device_t device;
    /* our private state goes below here */
};

struct sensors_data_context_t {
    struct sensors_data_device_t device;
    /* our private state goes below here */
};

static int sensors_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

static struct hw_module_methods_t sensors_module_methods = {
    open: sensors_device_open
};

static struct sensor_t const sensor_mma7450 = {
	name: "MMA7450L",
	vendor: "FSL",
	version: 2,
	handle: SENSORS_HANDLE_BASE + 1,
	type: SENSOR_TYPE_ACCELEROMETER,
	maxRange: 1023,
	resolution: 2,
};

struct sensor_handle_t : public native_handle {
    /* add the data fields we need here, for instance: */
    int ctl_fd;
};

sensor_handle_t sensor_data_handle;

static int get_sensors_list(struct sensors_module_t* module, struct sensor_t const** sensor)
{
    *sensor = &sensor_mma7450;

    LOGD("sensor name %s, handle %d, type %d",
	    (*sensor)->name, (*sensor)->handle, (*sensor)->type);

    return 1; //just one functions
}

struct sensors_module_t HAL_MODULE_INFO_SYM = {
	common: {
		tag: HARDWARE_MODULE_TAG,
		version_major: 1,
		version_minor: 0,
		id: SENSORS_HARDWARE_MODULE_ID,
		name: "Sonsor module",
		author: "Shen Yong at Freescale",
		methods: &sensors_module_methods,
	},
	get_sensors_list: get_sensors_list,
};

static int open_input()
{
    /* scan all input drivers and look for "compass" */
    int fd = -1;
    const char *dirname = "/dev/input";
    char devname[PATH_MAX];
    char *filename;
    DIR *dir;
    struct dirent *de;
    dir = opendir(dirname);
    if(dir == NULL)
        return -1;
    strcpy(devname, dirname);
    filename = devname + strlen(devname);
    *filename++ = '/';
    while((de = readdir(dir))) {
        if(de->d_name[0] == '.' &&
           (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        strcpy(filename, de->d_name);
        fd = open(devname, O_RDONLY);
        if (fd>=0) {
            char name[80];
            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1) {
                name[0] = '\0';
            }
                LOGD("name %s", name);
            if (!strcmp(name, "mma7450")) {
                LOGD("using %s (name=%s)", devname, name);
                break;
            }
            close(fd);
            fd = -1;
        }
    }
    closedir(dir);

    if (fd < 0) {
        LOGE("Couldn't find or open 'compass' driver (%s)", strerror(errno));
    }
    return fd;
}

/**
* Returns a native_handle_t, which will be the parameter to
* sensors_data_device_t::open_data().
* The caller takes ownership of this handle. This is intended to be
* passed cross processes.
*
* @return a native_handle_t if successful, NULL on error
*/
static native_handle_t* open_data_source(struct sensors_control_device_t *dev)
{
    LOGD("open_data_source");
    memset(&sensor_data_handle, 0, sizeof(sensor_data_handle));
    sensor_data_handle.numFds      = 1;
    sensor_data_handle.numInts     = 0; // extra ints we have in our handle
    sensor_data_handle.ctl_fd = open_input();
    return &sensor_data_handle;
}

static int activate(struct sensors_control_device_t *dev, int handle, int enabled)
{
    LOGI("active handle %d",handle);

    if (handle != sensor_mma7450.handle)
	    return -1;

    return sensor_mma7450.handle;
}

static int set_delay(struct sensors_control_device_t *dev, int32_t ms)
{
    LOGD("set delay %d ms",ms);
    return 0;
}

static int wake(struct sensors_control_device_t *dev)
{
    LOGD("sensor wake");
    return 0;
}

//data operation //////////////////////
static int sInputFD = -1;
static sensors_data_t sSensors;

/**
* Prepare to read sensor data.
*
* This routine does NOT take ownership of the handle
* and must not close it. Typically this routine would
* use a duplicate of the nh parameter.
*
* @param nh from sensors_control_open.
*
* @return 0 if successful, < 0 on error
*/
static int data_open(struct sensors_data_device_t *dev, native_handle_t* nh)
{
    int i, fd;
    fd = ((sensor_handle_t *)nh)->ctl_fd;
    LMSInit();
    memset(&sSensors, 0, sizeof(sSensors));
    sSensors.vector.status = SENSOR_STATUS_ACCURACY_HIGH;
    sInputFD = dup(fd);

    LOGD("sensors_data_open: fd = %d", sInputFD);
    return 0;
}

static int data_close(struct sensors_data_device_t *dev)
{
    LOGI("sensor data close");
    close(sInputFD);
    sInputFD = -1;
    return 0;
}

static int sensor_poll(struct sensors_data_device_t *dev, sensors_data_t* data)
{
    LOGI("sensor_poll");
    struct input_event event;
    int64_t t;
    int nread;

    int fd = sInputFD;

    if (fd <= 0)
        return -1;

    // wait until we get a complete event for an enabled sensor
    while (1) {
        nread = read(fd, &event, sizeof(event));

        if (nread == sizeof(event)) {
            uint32_t v;
            if (event.type == EV_ABS) {
                LOGI("type: %d code: %d value: %-5d time: %ds",
                        event.type, event.code, event.value,
                      (int)event.time.tv_sec);
                switch (event.code) {

                    case ABS_X:
                        sSensors.acceleration.x = event.value * CONVERT_A_X;
                        break;
                    case ABS_Y:
                        sSensors.acceleration.y = event.value * CONVERT_A_Y;
                        break;
                    case ABS_Z:
                        sSensors.acceleration.z = event.value * CONVERT_A_Z;
                        break;
                }
            } else if (event.type == EV_SYN) {
                int64_t t = event.time.tv_sec*1000000000LL + event.time.tv_usec*1000;
                *data = sSensors;
                return SENSORS_HANDLE_BASE + 1;
            }
        }
    }
}

static int sensors_device_control_close(struct hw_device_t *dev)
{
    LOGI("sensors_device_control_close");
    struct sensors_control_context_t* ctx = (struct sensors_control_context_t*)dev;
    if (ctx) {
        /* free all resources associated with this device here
         * in particular the sensors_handle_t, outstanding sensors_t, etc...
         */
        free(ctx);
    }
    return 0;
}

static int sensors_device_data_close(struct hw_device_t *dev)
{
    LOGI("sensors_device_data_close");
    struct sensors_data_context_t* ctx = (struct sensors_data_context_t*)dev;
    if (ctx) {
        /* free all resources associated with this device here
         * in particular all pending sensors_buffer_t if needed.
         *
         * NOTE: sensors_handle_t passed in initialize() is NOT freed and
         * its file descriptors are not closed (this is the responsibility
         * of the caller).
         */
        free(ctx);
    }
    return 0;
}

/*****************************************************************************/

static int sensors_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    int status = -EINVAL;

	LOGI("sensor device open %s", name);
    if (!strcmp(name, SENSORS_HARDWARE_CONTROL)) {
        struct sensors_control_context_t *dev;
        dev = (struct sensors_control_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = sensors_device_control_close;

        dev->device.open_data_source = open_data_source;
        dev->device.activate = activate;
        dev->device.set_delay = set_delay;
        dev->device.wake = wake;

        *device = &dev->device.common;
        status = 0;
    } else if (!strcmp(name, SENSORS_HARDWARE_DATA)) {
        struct sensors_data_context_t *dev;
        dev = (struct sensors_data_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = sensors_device_data_close;

        dev->device.data_open = data_open;
        dev->device.data_close = data_close;
        dev->device.poll = sensor_poll;

        *device = &dev->device.common;
        status = 0;
    }
    return status;
}
