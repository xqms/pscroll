// Small scroll event test client
// Author: Max Schwarz <Max@x-quadraht.de>

#include <stdio.h>
#include <assert.h>

//#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <string.h>

#define WHEEL_LABEL "Rel Vert Wheel"

int xi_opcode;
Display *dpy;
int wheel_device;
int wheel_valuator;
Window win;

static Window create_win()
{
    Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 200,
            200, 0, 0, WhitePixel(dpy, 0));
    Window subwindow = XCreateSimpleWindow(dpy, win, 50, 50, 50, 50, 0, 0,
            BlackPixel(dpy, 0));

    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);

    XMapWindow(dpy, subwindow);
    XMapWindow(dpy, win);
    XFlush(dpy);

    while (1) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == MapNotify)
            break;
    }

    return win;
}

static void findWheelDevice()
{
	XIDeviceInfo *info, *dev;
	XIValuatorClassInfo *valInfo;
	int ndevices;
	int i, j;
	const char *type = "";
	
	printf("\n==================================================\n");
	printf("Looking for pscroll wheel valuator...\n");
	
	info = XIQueryDevice(dpy, XIAllMasterDevices, &ndevices);
	
	wheel_device = -1;
	
	for(i = 0; i < ndevices; i++) {
		dev = &info[i];
		
		if(dev->use != XIMasterPointer)
			continue;
		
		printf("Device: %s\n", dev->name);
		
		for(j = 0; j < dev->num_classes; ++j) {
			XIAnyClassInfo *aclass = dev->classes[j];
			
			switch(aclass->type) {
				case XIValuatorClass:
					valInfo = (XIValuatorClassInfo*)aclass;
					
					if(strcmp(XGetAtomName(dpy, valInfo->label), WHEEL_LABEL) == 0)
					{
						XIEventMask eventmask;
						unsigned char mask[1] = {0};
						
						wheel_device = dev->deviceid;
						wheel_valuator = valInfo->number;
						
						eventmask.deviceid = XIAllMasterDevices;
						eventmask.mask = mask;
						eventmask.mask_len = sizeof(mask);
						
						XISetMask(mask, XI_Motion);
						
						XISelectEvents(dpy, win, &eventmask, 1);
						
						printf("Found pscroll wheel valuator\n");
						printf("Resolution is %d\n", valInfo->resolution);
						printf("\n");
						
						goto exit;
					}
					
					break;
			}
		}
	}
	
	fprintf(stderr, "No pscroll wheel device found.\n");
exit:
	XIFreeDeviceInfo(info);
}

static void process_event(XIDeviceEvent *data)
{
	unsigned int idx = 0;
	int i;
	
	if(wheel_device == -1)
		return; // No new-style wheel device found
	
	if(data->deviceid != wheel_device)
		return;
	
	if(!XIMaskIsSet(data->valuators.mask, wheel_valuator))
		return;
	
	for(i = 0; i < wheel_valuator; ++i)
		if(XIMaskIsSet(data->valuators.mask, i))
                {
			++idx;
                }
	
	printf("Scroll valuator event: %f\n", data->valuators.values[idx]);
}

static int has_xi2()
{
	int major, minor;
	int rc;
	
	major = 2;
	minor = 0;
	
	rc = XIQueryVersion(dpy, &major, &minor);
	if(rc == BadRequest)
	{
		printf("No XI2 support. Server supports version %d.%d only.\n", major, minor);
		return 0;
	}
	
	assert(rc == Success);
	
	printf("XI2 supported. Server provides version %d.%d\n", major, minor);
	
	return 1;
}

int main(int argc, char **argv)
{
	int event, error;
	XIEventMask evmask;
	unsigned char mask[2] = {0,0};
	
	dpy = XOpenDisplay(NULL);
	
	if (!dpy)
	{
		fprintf(stderr, "Failed to open display.\n");
		return -1;
	}
	
	if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &event, &error)) {
		fprintf(stderr, "X Input extension not available.\n");
		return -1;
	}
	
	if(!has_xi2())
	{
		return -1;
	}
	
	evmask.mask = mask;
	evmask.mask_len = sizeof(mask);
	evmask.deviceid = XIAllMasterDevices;
	
	XISetMask(mask, XI_DeviceChanged);
	
	XISelectEvents(dpy, DefaultRootWindow(dpy), &evmask, 1);
	XFlush(dpy);
	
	win = create_win();
	
	findWheelDevice();
	
	printf("Listening for events...\n");
	
	while(1)
	{
		XEvent event;
		XNextEvent(dpy, &event);
		
		if(event.xcookie.type != GenericEvent)
			continue;
		
		if(event.xcookie.extension != xi_opcode)
			continue;
		
		if(!XGetEventData(dpy, &event.xcookie))
		{
			fprintf(stderr, "Could not get event data\n");
			continue;
		}
		
		switch(event.xcookie.evtype)
		{
			case XI_Motion:
				process_event(event.xcookie.data);
				break;
			case XI_DeviceChanged:
				findWheelDevice();
				break;
		}
		
		XFreeEventData(dpy, &event.xcookie);
	}
	
	XCloseDisplay(dpy);
	return 0;
}
