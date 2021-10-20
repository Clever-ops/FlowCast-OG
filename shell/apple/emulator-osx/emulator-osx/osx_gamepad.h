//
//  osx_gamepad.h
//  reicast-osx
//
//  Created by flyinghead on 26/02/2019.
//  Copyright © 2019 reicast. All rights reserved.
//
#include "input/mouse.h"

class OSXMouse : public SystemMouse
{
public:
	OSXMouse() : SystemMouse("OSX")
	{
		_unique_id = "osx_mouse";
		loadMapping();
	}
};


