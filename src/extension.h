
#ifndef _EXTENSION_H_
#define _EXTENSION_H_

#include "smsdk_ext.h"
//#include <convar.h>

class Extension : public SDKExtension {
public:
	virtual bool SDK_OnLoad(char *error, size_t maxlen, bool late);
	virtual void SDK_OnUnload();
	virtual void SDK_OnAllLoaded();
	virtual void SDK_OnPauseChange(bool paused);
	virtual void SDK_OnDependenciesDropped();
	/*
	virtual bool SDK_OnMetamodLoad(ISmmAPI* ismm, char* error, size_t maxlen, bool late);
public: // IConCommandBaseAccessor
	bool RegisterConCommandBase(ConCommandBase* pVar);
	*/
};
extern int SM_Debugger_port();
extern float SM_Debugger_timeout();

#endif
