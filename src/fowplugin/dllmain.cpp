#include "core/stdafx.h"
#include "fowplugin.h"

extern "C" __declspec(dllexport) bool PluginInstance_OnLoad(const char* pszSelfModule, const char* pszSDKModule)
{
	return FOWPlugin_OnLoad(pszSelfModule, pszSDKModule);
}

extern "C" __declspec(dllexport) bool PluginInstance_OnUnload()
{
	return FOWPlugin_OnUnload();
}