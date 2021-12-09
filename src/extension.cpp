#include "debugger.h"
#include "extension.h"
#include <string>
#include <thread>


Extension g_zr;
SMEXT_LINK(&g_zr);

#ifndef _WIN32
#define GetProcAddress dlsym
// Linux doesn't have this function so this emulates its functionality
void *GetModuleHandle(const char *name) {
#define HMODULE void *
	void *handle;
	if (name == nullptr) {
		// hmm, how can this be handled under linux....
		// is it even needed?
		return nullptr;
	}

	if ((handle = dlopen(name, RTLD_NOW)) == nullptr) {
		// printf("Error:%s\n",dlerror());
		// couldn't open this file
		return nullptr;
	}

	// read "man dlopen" for details
	// in short dlopen() inc a ref count
	// so dec the ref count by performing the close
	dlclose(handle);
	return handle;
}
#endif // _WIN32
extern void(DebugHandler)(SourcePawn::IPluginContext *IPlugin,
						  sp_debug_break_info_t &BreakOnfo,
						  const SourcePawn::IErrorReport *IErrorReport);

extern void debugThread();
bool Inited = false;

extern DebugReport DebugListener;

/*ConVar sm_debugger_port("sm_debugger_port", "12345", 0, "SourceMod Debugger Port.");
//ConVar sm_debugger_timeout("sm_debugger_wait", "10.0", 0, "Wait n secs to connect to debugger.");

int SM_Debugger_port()
{
	return sm_debugger_port.GetInt();
}
float SM_Debugger_timeout()
{
	return sm_debugger_timeout.GetFloat();
}*/

bool Extension::SDK_OnLoad(char *error, size_t maxlen, bool late) {
	ISourcePawnFactory *factory = nullptr;
	GetSourcePawnFactoryFn factoryFn = nullptr;
	ISourcePawnEnvironment *current_env = nullptr;
	std::string modulename = "sourcepawn.jit.x86.";

	modulename += PLATFORM_LIB_EXT;
	auto module = GetModuleHandle(modulename.c_str());
	if (module) {
		factoryFn = GetSourcePawnFactoryFn(
			GetProcAddress((HMODULE)module, "GetSourcePawnFactory"));
	}
	if (factoryFn) {
		factory = factoryFn(SOURCEPAWN_API_VERSION);
	}
	if (factory) {
		current_env = factory->CurrentEnvironment();
	}
	if (current_env) {
		if (!Inited) {
			std::thread(debugThread).detach();
			Inited = true;
		}
		current_env->EnableDebugBreak();
		DebugListener.original = current_env->APIv1()->SetDebugListener(&DebugListener);
		current_env->APIv1()->SetDebugBreakHandler(DebugHandler);
		std::this_thread::sleep_for(std::chrono::duration<float>(10.0));
	}
	return true;
}

void Extension::SDK_OnUnload()
{
	ISourcePawnFactory *factory = nullptr;
	GetSourcePawnFactoryFn factoryFn = nullptr;
	ISourcePawnEnvironment *current_env = nullptr;
	std::string modulename = "sourcepawn.jit.x86.";
	modulename += PLATFORM_LIB_EXT;
	auto module = GetModuleHandle(modulename.c_str());
	if (module) {
		factoryFn = GetSourcePawnFactoryFn(
			GetProcAddress((HMODULE)module, "GetSourcePawnFactory"));
	}
	if (factoryFn) {
		factory = factoryFn(SOURCEPAWN_API_VERSION);
	}
	if (factory) {
		current_env = factory->CurrentEnvironment();
	}
	if (current_env) {
		current_env->APIv1()->SetDebugListener(DebugListener.original);
	}
}

void Extension::SDK_OnAllLoaded() {
}

void Extension::SDK_OnPauseChange(bool paused) {
}

void Extension::SDK_OnDependenciesDropped() {
}
