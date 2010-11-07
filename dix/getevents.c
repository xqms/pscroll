/*
 * Copyright © 2006 Nokia Corporation
 * Copyright © 2006-2007 Daniel Stone
 * Copyright © 2008 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors: Daniel Stone <daniel@fooishbar.org>
 *          Peter Hutterer <peter.hutterer@who-t.net>
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <X11/X.h>
#include <X11/keysym.h>
#include <X11/Xproto.h>
#include <math.h>

#include "misc.h"
#include "resource.h"
#include "inputstr.h"
#include "scrnintstr.h"
#include "cursorstr.h"
#include "dixstruct.h"
#include "globals.h"
#include "dixevents.h"
#include "mipointer.h"
#include "eventstr.h"
#include "eventconvert.h"

#include <X11/extensions/XKBproto.h>
#include "xkbsrv.h"

#ifdef PANORAMIX
#include "panoramiX.h"
#include "panoramiXsrv.h"
#endif

#include <X11/extensions/XI.h>
#include <X11/extensions/XIproto.h>
#include <pixman.h>
#include "exglobals.h"
#include "exevents.h"
#include "exglobals.h"
#include "extnsionst.h"
#include "listdev.h" /* for sizing up DeviceClassesChangedEvent */

/* Number of motion history events to store. */
#define MOTION_HISTORY_SIZE 256

/* InputEventList is the container list for all input events generated by the
 * DDX. The DDX is expected to call GetEventList() and then pass the list into
 * Get{Pointer|Keyboard}Events.
 */
EventListPtr InputEventList = NULL;
int InputEventListLen = 0;

int
GetEventList(EventListPtr* list)
{
    *list = InputEventList;
    return InputEventListLen;
}

/**
 * Pick some arbitrary size for Xi motion history.
 */
int
GetMotionHistorySize(void)
{
    return MOTION_HISTORY_SIZE;
}

void
set_button_down(DeviceIntPtr pDev, int button, int type)
{
    if (type == BUTTON_PROCESSED)
        SetBit(pDev->button->down, button);
    else
        SetBit(pDev->button->postdown, button);
}

void
set_button_up(DeviceIntPtr pDev, int button, int type)
{
    if (type == BUTTON_PROCESSED)
        ClearBit(pDev->button->down, button);
    else
        ClearBit(pDev->button->postdown, button);
}

Bool
button_is_down(DeviceIntPtr pDev, int button, int type)
{
    int ret = 0;

    if (type & BUTTON_PROCESSED)
        ret |= !!BitIsOn(pDev->button->down, button);
    if (type & BUTTON_POSTED)
        ret |= !!BitIsOn(pDev->button->postdown, button);

    return ret;
}

void
set_key_down(DeviceIntPtr pDev, int key_code, int type)
{
    if (type == KEY_PROCESSED)
        SetBit(pDev->key->down, key_code);
    else
        SetBit(pDev->key->postdown, key_code);
}

void
set_key_up(DeviceIntPtr pDev, int key_code, int type)
{
    if (type == KEY_PROCESSED)
        ClearBit(pDev->key->down, key_code);
    else
        ClearBit(pDev->key->postdown, key_code);
}

Bool
key_is_down(DeviceIntPtr pDev, int key_code, int type)
{
    int ret = 0;

    if (type & KEY_PROCESSED)
        ret |= !!BitIsOn(pDev->key->down, key_code);
    if (type & KEY_POSTED)
        ret |= !!BitIsOn(pDev->key->postdown, key_code);

    return ret;
}

static Bool
key_autorepeats(DeviceIntPtr pDev, int key_code)
{
    return !!(pDev->kbdfeed->ctrl.autoRepeats[key_code >> 3] &
              (1 << (key_code & 7)));
}

static void
init_event(DeviceIntPtr dev, DeviceEvent* event, Time ms)
{
    memset(event, 0, sizeof(DeviceEvent));
    event->header = ET_Internal;
    event->length = sizeof(DeviceEvent);
    event->time = ms;
    event->deviceid = dev->id;
    event->sourceid = dev->id;
}

static void
init_raw(DeviceIntPtr dev, RawDeviceEvent *event, Time ms, int type, int detail)
{
    memset(event, 0, sizeof(RawDeviceEvent));
    event->header = ET_Internal;
    event->length = sizeof(RawDeviceEvent);
    event->type = ET_RawKeyPress - ET_KeyPress + type;
    event->time = ms;
    event->deviceid = dev->id;
    event->sourceid = dev->id;
    event->detail.button = detail;
}

static void
set_raw_valuators(RawDeviceEvent *event, int first, int num, int *valuators, int32_t* data)
{
    int i;
    for (i = first; i < first + num; i++)
        SetBit(event->valuators.mask, i);

    memcpy(&data[first], valuators, num * sizeof(uint32_t));
}


static void
set_valuators(DeviceIntPtr dev, DeviceEvent* event, int first_valuator,
              int num_valuators, int *valuators)
{
    int i;

    for (i = first_valuator; i < first_valuator + num_valuators; i++)
    {
        SetBit(event->valuators.mask, i);
        if (dev->valuator->mode == Absolute)
            SetBit(event->valuators.mode, i);
        event->valuators.data_frac[i] =
            dev->last.remainder[i] * (1 << 16) * (1 << 16);
    }

    memcpy(&event->valuators.data[first_valuator],
           valuators, num_valuators * sizeof(int32_t));

}

void
CreateClassesChangedEvent(EventList* event,
                          DeviceIntPtr master,
                          DeviceIntPtr slave,
                          int type)
{
    int i;
    DeviceChangedEvent *dce;
    CARD32 ms = GetTimeInMillis();

    dce = (DeviceChangedEvent*)event->event;
    memset(dce, 0, sizeof(DeviceChangedEvent));
    dce->deviceid = slave->id;
    dce->masterid = master->id;
    dce->header = ET_Internal;
    dce->length = sizeof(DeviceChangedEvent);
    dce->type = ET_DeviceChanged;
    dce->time = ms;
    dce->flags = type;
    dce->flags |= DEVCHANGE_SLAVE_SWITCH;
    dce->sourceid = slave->id;

    if (slave->button)
    {
        dce->buttons.num_buttons = slave->button->numButtons;
        for (i = 0; i < dce->buttons.num_buttons; i++)
            dce->buttons.names[i] = slave->button->labels[i];
    }
    if (slave->valuator)
    {
        dce->num_valuators = slave->valuator->numAxes;
        for (i = 0; i < dce->num_valuators; i++)
        {
            dce->valuators[i].min = slave->valuator->axes[i].min_value;
            dce->valuators[i].max = slave->valuator->axes[i].max_value;
            dce->valuators[i].resolution = slave->valuator->axes[i].resolution;
            /* This should, eventually, be a per-axis mode */
            dce->valuators[i].mode = slave->valuator->mode;
            dce->valuators[i].name = slave->valuator->axes[i].label;
        }
    }
    if (slave->key)
    {
        dce->keys.min_keycode = slave->key->xkbInfo->desc->min_key_code;
        dce->keys.max_keycode = slave->key->xkbInfo->desc->max_key_code;
    }
}

/**
 * Rescale the coord between the two axis ranges.
 */
static int
rescaleValuatorAxis(int coord, float remainder, float *remainder_return, AxisInfoPtr from, AxisInfoPtr to,
                    int defmax)
{
    int fmin = 0, tmin = 0, fmax = defmax, tmax = defmax, coord_return;
    float value;

    if(from && from->min_value < from->max_value) {
        fmin = from->min_value;
        fmax = from->max_value;
    }
    if(to && to->min_value < to->max_value) {
        tmin = to->min_value;
        tmax = to->max_value;
    }

    if(fmin == tmin && fmax == tmax) {
        if (remainder_return)
            *remainder_return = remainder;
        return coord;
    }

    if(fmax == fmin) { /* avoid division by 0 */
        if (remainder_return)
            *remainder_return = 0.0;
        return 0;
    }

    value = (coord + remainder - fmin) * (tmax - tmin) / (fmax - fmin) + tmin;
    coord_return = lroundf(value);
    if (remainder_return)
        *remainder_return = value - coord_return;
    return coord_return;
}

/**
 * Update all coordinates when changing to a different SD
 * to ensure that relative reporting will work as expected
 * without loss of precision.
 *
 * pDev->last.valuators will be in absolute device coordinates after this
 * function.
 */
static void
updateSlaveDeviceCoords(DeviceIntPtr master, DeviceIntPtr pDev)
{
    ScreenPtr scr = miPointerGetScreen(pDev);
    int i;
    DeviceIntPtr lastSlave;

    /* master->last.valuators[0]/[1] is in screen coords and the actual
     * position of the pointer */
    pDev->last.valuators[0] = master->last.valuators[0];
    pDev->last.valuators[1] = master->last.valuators[1];

    if (!pDev->valuator)
        return;

    /* scale back to device coordinates */
    if(pDev->valuator->numAxes > 0)
        pDev->last.valuators[0] = rescaleValuatorAxis(pDev->last.valuators[0], pDev->last.remainder[0],
                        &pDev->last.remainder[0], NULL, pDev->valuator->axes + 0, scr->width);
    if(pDev->valuator->numAxes > 1)
        pDev->last.valuators[1] = rescaleValuatorAxis(pDev->last.valuators[1], pDev->last.remainder[1],
                        &pDev->last.remainder[1], NULL, pDev->valuator->axes + 1, scr->height);

    /* calculate the other axis as well based on info from the old
     * slave-device. If the old slave had less axes than this one,
     * last.valuators is reset to 0.
     */
    if ((lastSlave = master->last.slave) && lastSlave->valuator) {
        for (i = 2; i < pDev->valuator->numAxes; i++) {
            if (i >= lastSlave->valuator->numAxes)
                pDev->last.valuators[i] = 0;
            else
                pDev->last.valuators[i] =
                    rescaleValuatorAxis(pDev->last.valuators[i],
                            pDev->last.remainder[i],
                            &pDev->last.remainder[i],
                            lastSlave->valuator->axes + i,
                            pDev->valuator->axes + i, 0);
        }
    }

}

/**
 * Allocate the motion history buffer.
 */
void
AllocateMotionHistory(DeviceIntPtr pDev)
{
    int size;
    free(pDev->valuator->motion);

    if (pDev->valuator->numMotionEvents < 1)
        return;

    /* An MD must have a motion history size large enough to keep all
     * potential valuators, plus the respective range of the valuators.
     * 3 * INT32 for (min_val, max_val, curr_val))
     */
    if (IsMaster(pDev))
        size = sizeof(INT32) * 3 * MAX_VALUATORS;
    else
        size = sizeof(INT32) * pDev->valuator->numAxes;

    size += sizeof(Time);

    pDev->valuator->motion = calloc(pDev->valuator->numMotionEvents, size);
    pDev->valuator->first_motion = 0;
    pDev->valuator->last_motion = 0;
    if (!pDev->valuator->motion)
        ErrorF("[dix] %s: Failed to alloc motion history (%d bytes).\n",
                pDev->name, size * pDev->valuator->numMotionEvents);
}

/**
 * Dump the motion history between start and stop into the supplied buffer.
 * Only records the event for a given screen in theory, but in practice, we
 * sort of ignore this.
 *
 * If core is set, we only generate x/y, in INT16, scaled to screen coords.
 */
int
GetMotionHistory(DeviceIntPtr pDev, xTimecoord **buff, unsigned long start,
                 unsigned long stop, ScreenPtr pScreen, BOOL core)
{
    char *ibuff = NULL, *obuff;
    int i = 0, ret = 0;
    int j, coord;
    Time current;
    /* The size of a single motion event. */
    int size;
    int dflt;
    AxisInfo from, *to; /* for scaling */
    INT32 *ocbuf, *icbuf; /* pointer to coordinates for copying */
    INT16 *corebuf;
    AxisInfo core_axis = {0};

    if (!pDev->valuator || !pDev->valuator->numMotionEvents)
        return 0;

    if (core && !pScreen)
        return 0;

    if (IsMaster(pDev))
        size = (sizeof(INT32) * 3 * MAX_VALUATORS) + sizeof(Time);
    else
        size = (sizeof(INT32) * pDev->valuator->numAxes) + sizeof(Time);

    *buff = malloc(size * pDev->valuator->numMotionEvents);
    if (!(*buff))
        return 0;
    obuff = (char *)*buff;

    for (i = pDev->valuator->first_motion;
         i != pDev->valuator->last_motion;
         i = (i + 1) % pDev->valuator->numMotionEvents) {
        /* We index the input buffer by which element we're accessing, which
         * is not monotonic, and the output buffer by how many events we've
         * written so far. */
        ibuff = (char *) pDev->valuator->motion + (i * size);
        memcpy(&current, ibuff, sizeof(Time));

        if (current > stop) {
            return ret;
        }
        else if (current >= start) {
            if (core)
            {
                memcpy(obuff, ibuff, sizeof(Time)); /* copy timestamp */

                icbuf = (INT32*)(ibuff + sizeof(Time));
                corebuf = (INT16*)(obuff + sizeof(Time));

                /* fetch x coordinate + range */
                memcpy(&from.min_value, icbuf++, sizeof(INT32));
                memcpy(&from.max_value, icbuf++, sizeof(INT32));
                memcpy(&coord, icbuf++, sizeof(INT32));

                /* scale to screen coords */
                to = &core_axis;
                to->max_value = pScreen->width;
                coord = rescaleValuatorAxis(coord, 0.0, NULL, &from, to, pScreen->width);

                memcpy(corebuf, &coord, sizeof(INT16));
                corebuf++;

                /* fetch y coordinate + range */
                memcpy(&from.min_value, icbuf++, sizeof(INT32));
                memcpy(&from.max_value, icbuf++, sizeof(INT32));
                memcpy(&coord, icbuf++, sizeof(INT32));

                to->max_value = pScreen->height;
                coord = rescaleValuatorAxis(coord, 0.0, NULL, &from, to, pScreen->height);
                memcpy(corebuf, &coord, sizeof(INT16));

            } else if (IsMaster(pDev))
            {
                memcpy(obuff, ibuff, sizeof(Time)); /* copy timestamp */

                ocbuf = (INT32*)(obuff + sizeof(Time));
                icbuf = (INT32*)(ibuff + sizeof(Time));
                for (j = 0; j < MAX_VALUATORS; j++)
                {
                    if (j >= pDev->valuator->numAxes)
                        break;

                    /* fetch min/max/coordinate */
                    memcpy(&from.min_value, icbuf++, sizeof(INT32));
                    memcpy(&from.max_value, icbuf++, sizeof(INT32));
                    memcpy(&coord, icbuf++, sizeof(INT32));

                    to = (j < pDev->valuator->numAxes) ? &pDev->valuator->axes[j] : NULL;

                    /* x/y scaled to screen if no range is present */
                    if (j == 0 && (from.max_value < from.min_value))
                        from.max_value = pScreen->width;
                    else if (j == 1 && (from.max_value < from.min_value))
                        from.max_value = pScreen->height;

                    if (j == 0 && (to->max_value < to->min_value))
                        dflt = pScreen->width;
                    else if (j == 1 && (to->max_value < to->min_value))
                        dflt = pScreen->height;
                    else
                        dflt = 0;

                    /* scale from stored range into current range */
                    coord = rescaleValuatorAxis(coord, 0.0, NULL, &from, to, 0);
                    memcpy(ocbuf, &coord, sizeof(INT32));
                    ocbuf++;
                }
            } else
                memcpy(obuff, ibuff, size);

            /* don't advance by size here. size may be different to the
             * actually written size if the MD has less valuators than MAX */
            if (core)
                obuff += sizeof(INT32) + sizeof(Time);
            else
                obuff += (sizeof(INT32) * pDev->valuator->numAxes) + sizeof(Time);
            ret++;
        }
    }

    return ret;
}


/**
 * Update the motion history for a specific device, with the list of
 * valuators.
 *
 * Layout of the history buffer:
 *   for SDs: [time] [val0] [val1] ... [valn]
 *   for MDs: [time] [min_val0] [max_val0] [val0] [min_val1] ... [valn]
 *
 * For events that have some valuators unset (first_valuator > 0):
 *      min_val == max_val == val == 0.
 */
static void
updateMotionHistory(DeviceIntPtr pDev, CARD32 ms, int first_valuator,
                    int num_valuators, int *valuators)
{
    char *buff = (char *) pDev->valuator->motion;
    ValuatorClassPtr v;
    int i;

    if (!pDev->valuator->numMotionEvents)
        return;

    v = pDev->valuator;
    if (IsMaster(pDev))
    {
        buff += ((sizeof(INT32) * 3 * MAX_VALUATORS) + sizeof(CARD32)) *
                v->last_motion;

        memcpy(buff, &ms, sizeof(Time));
        buff += sizeof(Time);

        memset(buff, 0, sizeof(INT32) * 3 * MAX_VALUATORS);
        buff += 3 * sizeof(INT32) * first_valuator;

        for (i = first_valuator; i < first_valuator + num_valuators; i++)
        {
            if (i >= v->numAxes)
                break;
            memcpy(buff, &v->axes[i].min_value, sizeof(INT32));
            buff += sizeof(INT32);
            memcpy(buff, &v->axes[i].max_value, sizeof(INT32));
            buff += sizeof(INT32);
            memcpy(buff, &valuators[i - first_valuator], sizeof(INT32));
            buff += sizeof(INT32);
        }
    } else
    {

        buff += ((sizeof(INT32) * pDev->valuator->numAxes) + sizeof(CARD32)) *
            pDev->valuator->last_motion;

        memcpy(buff, &ms, sizeof(Time));
        buff += sizeof(Time);

        memset(buff, 0, sizeof(INT32) * pDev->valuator->numAxes);
        buff += sizeof(INT32) * first_valuator;

        memcpy(buff, valuators, sizeof(INT32) * num_valuators);
    }

    pDev->valuator->last_motion = (pDev->valuator->last_motion + 1) %
        pDev->valuator->numMotionEvents;
    /* If we're wrapping around, just keep the circular buffer going. */
    if (pDev->valuator->first_motion == pDev->valuator->last_motion)
        pDev->valuator->first_motion = (pDev->valuator->first_motion + 1) %
                                       pDev->valuator->numMotionEvents;

    return;
}


/**
 * Returns the maximum number of events GetKeyboardEvents,
 * GetKeyboardValuatorEvents, and GetPointerEvents will ever return.
 *
 * This MUST be absolutely constant, from init until exit.
 */
int
GetMaximumEventsNum(void) {
    /* One raw event
     * One device event
     * One possible device changed event
     */
    return 3;
}


/**
 * Clip an axis to its bounds, which are declared in the call to
 * InitValuatorAxisClassStruct.
 */
static void
clipAxis(DeviceIntPtr pDev, int axisNum, int *val)
{
    AxisInfoPtr axis;

    if (axisNum >= pDev->valuator->numAxes)
        return;

    axis = pDev->valuator->axes + axisNum;

    /* If a value range is defined, clip. If not, do nothing */
    if (axis->max_value <= axis->min_value)
        return;

    if (*val < axis->min_value)
        *val = axis->min_value;
    if (*val > axis->max_value)
        *val = axis->max_value;
}

/**
 * Clip every axis in the list of valuators to its bounds.
 */
static void
clipValuators(DeviceIntPtr pDev, int first_valuator, int num_valuators,
              int *valuators)
{
    int i;

    for (i = 0; i < num_valuators; i++)
        clipAxis(pDev, i + first_valuator, &(valuators[i]));
}

/**
 * Create the DCCE event (does not update the master's device state yet, this
 * is done in the event processing).
 * Pull in the coordinates from the MD if necessary.
 *
 * @param events Pointer to a pre-allocated event list.
 * @param dev The slave device that generated an event.
 * @param type Either DEVCHANGE_POINTER_EVENT and/or DEVCHANGE_KEYBOARD_EVENT
 * @param num_events The current number of events, returns the number of
 *        events if a DCCE was generated.
 * @return The updated @events pointer.
 */
EventListPtr
UpdateFromMaster(EventListPtr events, DeviceIntPtr dev, int type, int *num_events)
{
    DeviceIntPtr master;

    master = GetMaster(dev, (type & DEVCHANGE_POINTER_EVENT) ?  MASTER_POINTER : MASTER_KEYBOARD);

    if (master && master->last.slave != dev)
    {
        CreateClassesChangedEvent(events, master, dev, type);
        if (IsPointerDevice(master))
        {
            updateSlaveDeviceCoords(master, dev);
            master->last.numValuators = dev->last.numValuators;
        }
        master->last.slave = dev;
        (*num_events)++;
        events++;
    }
    return events;
}

/**
 * Move the device's pointer to the position given in the valuators.
 *
 * @param dev The device which's pointer is to be moved.
 * @param x Returns the x position of the pointer after the move.
 * @param y Returns the y position of the pointer after the move.
 * @param first The first valuator in @valuators
 * @param num Total number of valuators in @valuators.
 * @param valuators Valuator data for each axis between @first and
 *        @first+@num.
 */
static void
moveAbsolute(DeviceIntPtr dev, int *x, int *y,
             int first, int num, int *valuators)
{
    int i;


    if (num >= 1 && first == 0)
        *x = *(valuators + 0);
    else
        *x = dev->last.valuators[0];

    if (first <= 1 && num >= (2 - first))
        *y = *(valuators + 1 - first);
    else
        *y = dev->last.valuators[1];

    clipAxis(dev, 0, x);
    clipAxis(dev, 1, y);

    i = (first > 2) ? 0 : 2;
    for (; i < num; i++)
    {
        dev->last.valuators[i + first] = valuators[i];
        clipAxis(dev, i, &dev->last.valuators[i + first]);
    }
}

/**
 * Move the device's pointer by the values given in @valuators.
 *
 * @param dev The device which's pointer is to be moved.
 * @param x Returns the x position of the pointer after the move.
 * @param y Returns the y position of the pointer after the move.
 * @param first The first valuator in @valuators
 * @param num Total number of valuators in @valuators.
 * @param valuators Valuator data for each axis between @first and
 *        @first+@num.
 */
static void
moveRelative(DeviceIntPtr dev, int *x, int *y,
             int first, int num, int *valuators)
{
    int i;

    *x = dev->last.valuators[0];
    *y = dev->last.valuators[1];

    if (num >= 1 && first == 0)
        *x += *(valuators +0);

    if (first <= 1 && num >= (2 - first))
        *y += *(valuators + 1 - first);

    /* if attached, clip both x and y to the defined limits (usually
     * co-ord space limit). If it is attached, we need x/y to go over the
     * limits to be able to change screens. */
    if(dev->u.master && dev->valuator->mode == Absolute) {
        clipAxis(dev, 0, x);
        clipAxis(dev, 1, y);
    }

    /* calc other axes, clip, drop back into valuators */
    i = (first > 2) ? 0 : 2;
    for (; i < num; i++)
    {
        dev->last.valuators[i + first] += valuators[i];
        if (dev->valuator->mode == Absolute)
            clipAxis(dev, i, &dev->last.valuators[i + first]);
        valuators[i] = dev->last.valuators[i + first];
    }
}

/**
 * Accelerate the data in valuators based on the device's acceleration scheme.
 *
 * @param dev The device which's pointer is to be moved.
 * @param first The first valuator in @valuators
 * @param num Total number of valuators in @valuators.
 * @param valuators Valuator data for each axis between @first and
 *        @first+@num.
 * @param ms Current time.
 */
static void
accelPointer(DeviceIntPtr dev, int first, int num, int *valuators, CARD32 ms)
{
    if (dev->valuator->accelScheme.AccelSchemeProc)
        dev->valuator->accelScheme.AccelSchemeProc(dev, first, num, valuators, ms);
}

/**
 * If we have HW cursors, this actually moves the visible sprite. If not, we
 * just do all the screen crossing, etc.
 *
 * We scale from device to screen coordinates here, call
 * miPointerSetPosition() and then scale back into device coordinates (if
 * needed). miPSP will change x/y if the screen was crossed.
 *
 * @param dev The device to be moved.
 * @param x Pointer to current x-axis value, may be modified.
 * @param y Pointer to current y-axis value, may be modified.
 * @param x_frac Fractional part of current x-axis value, may be modified.
 * @param y_frac Fractional part of current y-axis value, may be modified.
 * @param scr Screen the device's sprite is currently on.
 * @param screenx Screen x coordinate the sprite is on after the update.
 * @param screeny Screen y coordinate the sprite is on after the update.
 * @param screenx_frac Fractional part of screen x coordinate, as above.
 * @param screeny_frac Fractional part of screen y coordinate, as above.
 */
static void
positionSprite(DeviceIntPtr dev, int *x, int *y, float x_frac, float y_frac,
               ScreenPtr scr, int *screenx, int *screeny, float *screenx_frac, float *screeny_frac)
{
    int old_screenx, old_screeny;

    /* scale x&y to screen */
    if (dev->valuator->numAxes > 0) {
        *screenx = rescaleValuatorAxis(*x, x_frac, screenx_frac,
                dev->valuator->axes + 0, NULL, scr->width);
    } else {
        *screenx = dev->last.valuators[0];
        *screenx_frac = dev->last.remainder[0];
    }

    if (dev->valuator->numAxes > 1) {
        *screeny = rescaleValuatorAxis(*y, y_frac, screeny_frac,
                dev->valuator->axes + 1, NULL, scr->height);
    } else {
        *screeny = dev->last.valuators[1];
        *screeny_frac = dev->last.remainder[1];
    }

    /* Hit the left screen edge? */
    if (*screenx <= 0 && *screenx_frac < 0.0f)
    {
        *screenx_frac = 0.0f;
        x_frac = 0.0f;
    }
    if (*screeny <= 0 && *screeny_frac < 0.0f)
    {
        *screeny_frac = 0.0f;
        y_frac = 0.0f;
    }


    old_screenx = *screenx;
    old_screeny = *screeny;
    /* This takes care of crossing screens for us, as well as clipping
     * to the current screen. */
    miPointerSetPosition(dev, screenx, screeny);

    if (dev->u.master) {
        dev->u.master->last.valuators[0] = *screenx;
        dev->u.master->last.valuators[1] = *screeny;
        dev->u.master->last.remainder[0] = *screenx_frac;
        dev->u.master->last.remainder[1] = *screeny_frac;
    }

    /* Crossed screen? Scale back to device coordiantes */
    if(*screenx != old_screenx)
    {
        scr = miPointerGetScreen(dev);
        *x = rescaleValuatorAxis(*screenx, *screenx_frac, &x_frac, NULL,
                                dev->valuator->axes + 0, scr->width);
    }
    if(*screeny != old_screeny)
    {
        scr = miPointerGetScreen(dev);
        *y = rescaleValuatorAxis(*screeny, *screeny_frac, &y_frac, NULL,
                                 dev->valuator->axes + 1, scr->height);
    }

    /* dropy x/y (device coordinates) back into valuators for next event */
    dev->last.valuators[0] = *x;
    dev->last.valuators[1] = *y;
    dev->last.remainder[0] = x_frac;
    dev->last.remainder[1] = y_frac;
}

/**
 * Update the motion history for the device and (if appropriate) for its
 * master device.
 * @param dev Slave device to update.
 * @param first First valuator to append to history.
 * @param num Total number of valuators to append to history.
 * @param ms Current time
 */
static void
updateHistory(DeviceIntPtr dev, int first, int num, CARD32 ms)
{
    updateMotionHistory(dev, ms, first, num, &dev->last.valuators[first]);
    if (dev->u.master)
    {
        DeviceIntPtr master = GetMaster(dev, MASTER_POINTER);
        updateMotionHistory(master, ms, first, num, &dev->last.valuators[first]);
    }
}

/**
 * Convenience wrapper around GetKeyboardValuatorEvents, that takes no
 * valuators.
 */
int
GetKeyboardEvents(EventList *events, DeviceIntPtr pDev, int type, int key_code) {
    return GetKeyboardValuatorEvents(events, pDev, type, key_code, 0, 0, NULL);
}


/**
 * Returns a set of InternalEvents for KeyPress/KeyRelease, optionally
 * also with valuator events.
 *
 * events is not NULL-terminated; the return value is the number of events.
 * The DDX is responsible for allocating the event structure in the first
 * place via GetMaximumEventsNum(), and for freeing it.
 */
int
GetKeyboardValuatorEvents(EventList *events, DeviceIntPtr pDev, int type,
                          int key_code, int first_valuator,
                          int num_valuators, int *valuators_in) {
    int num_events = 0;
    CARD32 ms = 0;
    DeviceEvent *event;
    RawDeviceEvent *raw;
    int valuators[MAX_VALUATORS];

    /* refuse events from disabled devices */
    if (!pDev->enabled)
        return 0;

    if (!events ||!pDev->key || !pDev->focus || !pDev->kbdfeed ||
        num_valuators > MAX_VALUATORS ||
       (type != KeyPress && type != KeyRelease) ||
       (key_code < 8 || key_code > 255))
        return 0;

    num_events = 1;

    events = UpdateFromMaster(events, pDev, DEVCHANGE_KEYBOARD_EVENT, &num_events);

    /* Handle core repeating, via press/release/press/release. */
    if (type == KeyPress && key_is_down(pDev, key_code, KEY_POSTED)) {
        /* If autorepeating is disabled either globally or just for that key,
         * or we have a modifier, don't generate a repeat event. */
        if (!pDev->kbdfeed->ctrl.autoRepeat ||
            !key_autorepeats(pDev, key_code) ||
            pDev->key->xkbInfo->desc->map->modmap[key_code])
            return 0;
    }

    ms = GetTimeInMillis();

    raw = (RawDeviceEvent*)events->event;
    events++;
    num_events++;

    memcpy(valuators, valuators_in, num_valuators * sizeof(int));

    init_raw(pDev, raw, ms, type, key_code);
    set_raw_valuators(raw, first_valuator, num_valuators, valuators,
                      raw->valuators.data_raw);

    if (num_valuators)
        clipValuators(pDev, first_valuator, num_valuators, valuators);

    set_raw_valuators(raw, first_valuator, num_valuators, valuators,
                      raw->valuators.data);

    event = (DeviceEvent*) events->event;
    init_event(pDev, event, ms);
    event->detail.key = key_code;

    if (type == KeyPress) {
        event->type = ET_KeyPress;
	set_key_down(pDev, key_code, KEY_POSTED);
    }
    else if (type == KeyRelease) {
        event->type = ET_KeyRelease;
	set_key_up(pDev, key_code, KEY_POSTED);
    }

    if (num_valuators)
        clipValuators(pDev, first_valuator, num_valuators, valuators);

    set_valuators(pDev, event, first_valuator, num_valuators, valuators);

    return num_events;
}

/**
 * Initialize an event list and fill with 32 byte sized events.
 * This event list is to be passed into GetPointerEvents() and
 * GetKeyboardEvents().
 *
 * @param num_events Number of elements in list.
 */
EventListPtr
InitEventList(int num_events)
{
    EventListPtr events;
    int i;

    events = (EventListPtr)calloc(num_events, sizeof(EventList));
    if (!events)
        return NULL;

    for (i = 0; i < num_events; i++)
    {
        events[i].evlen = sizeof(InternalEvent);
        events[i].event = calloc(1, sizeof(InternalEvent));
        if (!events[i].event)
        {
            /* rollback */
            while(i--)
                free(events[i].event);
            free(events);
            events = NULL;
            break;
        }
    }

    return events;
}

/**
 * Free an event list.
 *
 * @param list The list to be freed.
 * @param num_events Number of elements in list.
 */
void
FreeEventList(EventListPtr list, int num_events)
{
    if (!list)
        return;
    while(num_events--)
        free(list[num_events].event);
    free(list);
}

static void
transformAbsolute(DeviceIntPtr dev, int v[MAX_VALUATORS])
{
    struct pixman_f_vector p;

    /* p' = M * p in homogeneous coordinates */
    p.v[0] = v[0];
    p.v[1] = v[1];
    p.v[2] = 1.0;

    pixman_f_transform_point(&dev->transform, &p);

    v[0] = lround(p.v[0]);
    v[1] = lround(p.v[1]);
}

/**
 * Generate a series of InternalEvents (filled into the EventList)
 * representing pointer motion, or button presses.
 *
 * events is not NULL-terminated; the return value is the number of events.
 * The DDX is responsible for allocating the event structure in the first
 * place via InitEventList() and GetMaximumEventsNum(), and for freeing it.
 *
 * In the generated events rootX/Y will be in absolute screen coords and
 * the valuator information in the absolute or relative device coords.
 *
 * last.valuators[x] of the device is always in absolute device coords.
 * last.valuators[x] of the master device is in absolute screen coords.
 *
 * master->last.valuators[x] for x > 2 is undefined.
 */
int
GetPointerEvents(EventList *events, DeviceIntPtr pDev, int type, int buttons,
                 int flags, int first_valuator, int num_valuators,
                 int *valuators_in) {
    int num_events = 1;
    CARD32 ms;
    DeviceEvent *event;
    RawDeviceEvent    *raw;
    int x = 0, y = 0, /* device coords */
        cx, cy; /* only screen coordinates */
    float x_frac = 0.0, y_frac = 0.0, cx_frac, cy_frac;
    ScreenPtr scr = miPointerGetScreen(pDev);
    int valuators[MAX_VALUATORS];

    /* refuse events from disabled devices */
    if (!pDev->enabled)
        return 0;

    ms = GetTimeInMillis(); /* before pointer update to help precision */

    if (!scr || !pDev->valuator || first_valuator < 0 ||
        num_valuators > MAX_VALUATORS ||
        ((num_valuators + first_valuator) > pDev->valuator->numAxes) ||
        (type != MotionNotify && type != ButtonPress && type != ButtonRelease) ||
        (type != MotionNotify && !pDev->button) ||
        ((type == ButtonPress || type == ButtonRelease) && !buttons) ||
        (type == MotionNotify && num_valuators <= 0))
        return 0;

    events = UpdateFromMaster(events, pDev, DEVCHANGE_POINTER_EVENT, &num_events);

    raw = (RawDeviceEvent*)events->event;
    events++;
    num_events++;

    memcpy(valuators, valuators_in, num_valuators * sizeof(int));

    init_raw(pDev, raw, ms, type, buttons);
    set_raw_valuators(raw, first_valuator, num_valuators, valuators,
                      raw->valuators.data_raw);

    if (flags & POINTER_ABSOLUTE)
    {
        if (flags & POINTER_SCREEN) /* valuators are in screen coords */
        {

            if (num_valuators >= 1 && first_valuator == 0)
                valuators[0] = rescaleValuatorAxis(valuators[0], 0.0, &x_frac, NULL,
                        pDev->valuator->axes + 0,
                        scr->width);
            if (first_valuator <= 1 && num_valuators >= (2 - first_valuator))
                valuators[1 - first_valuator] = rescaleValuatorAxis(valuators[1 - first_valuator], 0.0, &y_frac, NULL,
                        pDev->valuator->axes + 1,
                        scr->height);
        }

        transformAbsolute(pDev, valuators);
        moveAbsolute(pDev, &x, &y, first_valuator, num_valuators, valuators);
    } else {
        if (flags & POINTER_ACCELERATE) {
            accelPointer(pDev, first_valuator, num_valuators, valuators, ms);
            /* The pointer acceleration code modifies the fractional part
             * in-place, so we need to extract this information first */
            x_frac = pDev->last.remainder[0];
            y_frac = pDev->last.remainder[1];
        }
        moveRelative(pDev, &x, &y, first_valuator, num_valuators, valuators);
    }

    set_raw_valuators(raw, first_valuator, num_valuators, valuators,
            raw->valuators.data);

    positionSprite(pDev, &x, &y, x_frac, y_frac, scr, &cx, &cy, &cx_frac, &cy_frac);
    updateHistory(pDev, first_valuator, num_valuators, ms);

    /* Update the valuators with the true value sent to the client*/
    if (num_valuators >= 1 && first_valuator == 0)
        valuators[0] = x;
    if (first_valuator <= 1 && num_valuators >= (2 - first_valuator))
        valuators[1 - first_valuator] = y;

    if (num_valuators)
        clipValuators(pDev, first_valuator, num_valuators, valuators);

    event = (DeviceEvent*) events->event;
    init_event(pDev, event, ms);

    if (type == MotionNotify) {
        event->type = ET_Motion;
        event->detail.button = 0;
    }
    else {
        if (type == ButtonPress) {
            event->type = ET_ButtonPress;
            set_button_down(pDev, buttons, BUTTON_POSTED);
        }
        else if (type == ButtonRelease) {
            event->type = ET_ButtonRelease;
            set_button_up(pDev, buttons, BUTTON_POSTED);
        }
        event->detail.button = buttons;
    }

    event->root_x = cx; /* root_x/y always in screen coords */
    event->root_y = cy;
    event->root_x_frac = cx_frac;
    event->root_y_frac = cy_frac;

    set_valuators(pDev, event, first_valuator, num_valuators, valuators);

    return num_events;
}


/**
 * Generate ProximityIn/ProximityOut InternalEvents, accompanied by
 * valuators.
 *
 * events is not NULL-terminated; the return value is the number of events.
 * The DDX is responsible for allocating the event structure in the first
 * place via GetMaximumEventsNum(), and for freeing it.
 */
int
GetProximityEvents(EventList *events, DeviceIntPtr pDev, int type,
                   int first_valuator, int num_valuators, int *valuators_in)
{
    int num_events = 1;
    DeviceEvent *event;
    int valuators[MAX_VALUATORS];

    /* refuse events from disabled devices */
    if (!pDev->enabled)
        return 0;

    /* Sanity checks. */
    if (type != ProximityIn && type != ProximityOut)
        return 0;
    if (!pDev->valuator)
        return 0;
    /* Do we need to send a DeviceValuator event? */
    if ((pDev->valuator->mode & 1) == Relative)
        num_valuators = 0;

    /* You fail. */
    if (first_valuator < 0 || num_valuators > MAX_VALUATORS ||
        (num_valuators + first_valuator) > pDev->valuator->numAxes)
        return 0;

    events = UpdateFromMaster(events, pDev, DEVCHANGE_POINTER_EVENT, &num_events);

    event = (DeviceEvent *) events->event;
    init_event(pDev, event, GetTimeInMillis());
    event->type = (type == ProximityIn) ? ET_ProximityIn : ET_ProximityOut;

    if (num_valuators) {
        memcpy(valuators, valuators_in, num_valuators * sizeof(int));
        clipValuators(pDev, first_valuator, num_valuators, valuators);
    }

    set_valuators(pDev, event, first_valuator, num_valuators, valuators);

    return num_events;
}

/**
 * Synthesize a single motion event for the core pointer.
 *
 * Used in cursor functions, e.g. when cursor confinement changes, and we need
 * to shift the pointer to get it inside the new bounds.
 */
void
PostSyntheticMotion(DeviceIntPtr pDev,
                    int x,
                    int y,
                    int screen,
                    unsigned long time)
{
    DeviceEvent ev;

#ifdef PANORAMIX
    /* Translate back to the sprite screen since processInputProc
       will translate from sprite screen to screen 0 upon reentry
       to the DIX layer. */
    if (!noPanoramiXExtension) {
        x += screenInfo.screens[0]->x - screenInfo.screens[screen]->x;
        y += screenInfo.screens[0]->y - screenInfo.screens[screen]->y;
    }
#endif

    memset(&ev, 0, sizeof(DeviceEvent));
    init_event(pDev, &ev, time);
    ev.root_x = x;
    ev.root_y = y;
    ev.type = ET_Motion;
    ev.time = time;

    /* FIXME: MD/SD considerations? */
    (*pDev->public.processInputProc)((InternalEvent*)&ev, pDev);
}
