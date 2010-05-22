// Small scroll event test client
// Author: Max Schwarz <Max@x-quadraht.de>

#include <stdio.h>

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
					
					printf("Valuator class: %s\n", XGetAtomName(dpy, valInfo->label));
					
					if(strcmp(XGetAtomName(dpy, valInfo->label), WHEEL_LABEL) == 0)
					{
						XIEventMask eventmask;
						unsigned char mask[1] = {0};
						
						wheel_device = dev->deviceid;
						wheel_valuator = j;
						
						eventmask.deviceid = wheel_device;
						eventmask.mask = mask;
						eventmask.mask_len = sizeof(mask);
						
						XISetMask(mask, XI_Motion);
						
						XISelectEvents(dpy, win, &eventmask, 1);
						
						printf("Found pscroll wheel valuator\n");
						
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
	
	if(XIMaskIsSet(data->valuators.mask, wheel_valuator))
		return;
	
	for(i = 0; i < wheel_valuator - 1; ++i)
		if(XIMaskIsSet(data->valuators.mask, i))
			++idx;
	
	printf("Scroll valuator event: %f\n", data->valuators.values[idx]);
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
	
	evmask.mask = mask;
	evmask.mask_len = sizeof(mask);
	evmask.deviceid = XIAllDevices;
	
	XISetMask(mask, XI_DeviceChanged);
	
	XISelectEvents(dpy, DefaultRootWindow(dpy), &evmask, 1);
	
	win = create_win();
	
	findWheelDevice();
	
	while(1)
	{
		XEvent event;
		XNextEvent(dpy, &event);
		
		if(event.xcookie.type != GenericEvent)
		{
			printf("wrong event type (is %d, should be %d)\n", event.xcookie.type, GenericEvent);
			continue;
		}
		
		if(event.xcookie.extension != xi_opcode)
		{
			printf("wrong opcode\n");
			continue;
		}
		
		if(!XGetEventData(dpy, &event.xcookie))
		{
			printf("Could not get event data\n");
			continue;
		}
		
		switch(event.xcookie.evtype)
		{
			case XI_Motion:
				process_event(event.xcookie.data);
				break;
			case XI_HierarchyChanged:
				findWheelDevice();
				break;
		}
	}
	
	return 0;
}
