
#ifndef _EXTENSION_H_
#define _EXTENSION_H_

#include "smsdk_ext.h"

class Extension : public SDKExtension {
public:
	virtual bool SDK_OnLoad(char *error, size_t maxlen, bool late);
	virtual void SDK_OnUnload();
	virtual void SDK_OnAllLoaded();
	virtual void SDK_OnPauseChange(bool paused);
	virtual void SDK_OnDependenciesDropped();
};

#endif
