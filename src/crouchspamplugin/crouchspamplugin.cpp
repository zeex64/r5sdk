//=============================================================================//
//
// Purpose: Server-side crouch transition filtering plugin.
//
//=============================================================================//

#include "core/stdafx.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

#include "pluginsdk/ifactory.h"
#include "vpc/interfaces.h"

#include "filesystem/filesystem.h"

#include "game/shared/in_buttons.h"
#include "game/shared/usercmd.h"

class CPlayer;
class IMoveHelper;

CFileSystem_Stdio* g_pFileSystem_Stdio = nullptr;

CFileSystem_Stdio* FileSystem()
{
	Assert(g_pFileSystem_Stdio);
	return g_pFileSystem_Stdio;
}

namespace
{
using PlayerRunCommandFn = void(*)(CPlayer*, CUserCmd*, IMoveHelper*);
using CreateMoveFn = void(*)(void*, int, float, bool);
using GetUserCmdFn = CUserCmd*(*)(void*, int, int);

struct CrouchFilterState
{
	int lastCommandNumber = 0;
	double lastSeenTime = 0.0;
	double nextTransitionTime = 0.0;
	bool crouched = false;
};

ConVar sv_crouchspam_enable(
	"sv_crouchspam_enable", "1", FCVAR_RELEASE | FCVAR_REPLICATED,
	"Enforce server-side crouch hold and transition debounce windows.",
	true, 0.0f, true, 1.0f, "bool");

ConVar sv_crouchspam_min_hold_ms(
	"sv_crouchspam_min_hold_ms", "150", FCVAR_RELEASE | FCVAR_REPLICATED,
	"Minimum time in milliseconds that an accepted crouch press remains active.",
	true, 0.0f, true, 2000.0f, "milliseconds");

ConVar sv_crouchspam_debounce_ms(
	"sv_crouchspam_debounce_ms", "100", FCVAR_RELEASE | FCVAR_REPLICATED,
	"Minimum time in milliseconds between accepted crouch state transitions.",
	true, 0.0f, true, 2000.0f, "milliseconds");

ConVar cl_crouchspam_predict(
	"cl_crouchspam_predict", "1", FCVAR_RELEASE,
	"Apply crouch filtering before local prediction when the plugin is installed on a client.",
	true, 0.0f, true, 1.0f, "bool");

PlayerRunCommandFn s_PlayerRunCommand = nullptr;
CreateMoveFn s_CreateMove = nullptr;
GetUserCmdFn s_GetUserCmd = nullptr;
char s_ClientStreamKey;
std::unordered_map<const void*, CrouchFilterState> s_PlayerStates;
std::mutex s_PlayerStatesMutex;
bool s_Initialized = false;

constexpr double kStateExpirySeconds = 5.0;
constexpr double kStatePruneIntervalSeconds = 10.0;
double s_LastStatePruneTime = 0.0;

double MonotonicTimeSeconds()
{
	using Clock = std::chrono::steady_clock;
	return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

void ResetAllStates()
{
	std::lock_guard<std::mutex> lock(s_PlayerStatesMutex);
	s_PlayerStates.clear();
	s_LastStatePruneTime = 0.0;
}

void FilterCrouchInput(const void* streamKey, CUserCmd* cmd, const bool resetOnCommandRollback)
{
	if (!sv_crouchspam_enable.GetBool())
	{
		std::lock_guard<std::mutex> lock(s_PlayerStatesMutex);
		s_PlayerStates.erase(streamKey);
		return;
	}

	// Synthetic/null commands don't belong to a client input stream.
	if (cmd->command_number <= 0)
		return;

	const double now = MonotonicTimeSeconds();
	const bool requestedCrouch = (cmd->buttons & IN_DUCK) != 0;

	std::lock_guard<std::mutex> lock(s_PlayerStatesMutex);

	if ((now - s_LastStatePruneTime) >= kStatePruneIntervalSeconds)
	{
		for (auto it = s_PlayerStates.begin(); it != s_PlayerStates.end();)
		{
			if ((now - it->second.lastSeenTime) > kStateExpirySeconds)
				it = s_PlayerStates.erase(it);
			else
				++it;
		}

		s_LastStatePruneTime = now;
	}

	CrouchFilterState& state = s_PlayerStates[streamKey];

	// Expiry resets a player address reused after disconnect. A duplicate or
	// rolled-back command number deliberately does not reset the filter, since
	// untrusted command sequencing must not provide a debounce bypass.
	const bool resetState = state.lastCommandNumber == 0
		|| (resetOnCommandRollback && cmd->command_number < state.lastCommandNumber)
		|| (now - state.lastSeenTime) > kStateExpirySeconds;

	if (resetState)
	{
		state = {};
		state.crouched = requestedCrouch;

		if (state.crouched)
		{
			const double holdSeconds = sv_crouchspam_min_hold_ms.GetFloat() / 1000.0;
			const double debounceSeconds = sv_crouchspam_debounce_ms.GetFloat() / 1000.0;
			state.nextTransitionTime = now + Max(holdSeconds, debounceSeconds);
		}
	}
	else if (requestedCrouch != state.crouched && now >= state.nextTransitionTime)
	{
		state.crouched = requestedCrouch;

		const double debounceSeconds = sv_crouchspam_debounce_ms.GetFloat() / 1000.0;
		double transitionDelay = debounceSeconds;

		if (state.crouched)
		{
			const double holdSeconds = sv_crouchspam_min_hold_ms.GetFloat() / 1000.0;
			transitionDelay = Max(transitionDelay, holdSeconds);
		}

		state.nextTransitionTime = now + transitionDelay;
	}

	state.lastCommandNumber = Max(state.lastCommandNumber, cmd->command_number);
	state.lastSeenTime = now;

	if (state.crouched)
		cmd->buttons |= IN_DUCK;
	else
		cmd->buttons &= ~IN_DUCK;
}

void PlayerRunCommandHook(CPlayer* player, CUserCmd* cmd, IMoveHelper* moveHelper)
{
	FilterCrouchInput(player, cmd, false);
	s_PlayerRunCommand(player, cmd, moveHelper);
}

void CreateMoveHook(void* input, int sequenceNumber, float inputSampleFrameTime, bool active)
{
	s_CreateMove(input, sequenceNumber, inputSampleFrameTime, active);

	if (!cl_crouchspam_predict.GetBool())
	{
		std::lock_guard<std::mutex> lock(s_PlayerStatesMutex);
		s_PlayerStates.erase(&s_ClientStreamKey);
		return;
	}

	CUserCmd* const cmd = s_GetUserCmd(input, 0, sequenceNumber);
	if (cmd)
		FilterCrouchInput(&s_ClientStreamKey, cmd, true);
}

bool SetHooks(const bool attach)
{
	if (DetourTransactionBegin() != NO_ERROR)
		return false;

	if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR)
	{
		DetourTransactionAbort();
		return false;
	}

	const LONG hookResult = attach
		? DetourAttach(&s_PlayerRunCommand, &PlayerRunCommandHook)
		: DetourDetach(&s_PlayerRunCommand, &PlayerRunCommandHook);

	if (hookResult != NO_ERROR)
	{
		DetourTransactionAbort();
		return false;
	}

	if (s_CreateMove)
	{
		const LONG clientHookResult = attach
			? DetourAttach(&s_CreateMove, &CreateMoveHook)
			: DetourDetach(&s_CreateMove, &CreateMoveHook);

		if (clientHookResult != NO_ERROR)
		{
			DetourTransactionAbort();
			return false;
		}
	}

	return DetourTransactionCommit() == NO_ERROR;
}

bool Initialize(const char* sdkModuleName)
{
	CModule sdkModule(sdkModuleName);
	const InstantiateInterfaceFn getFactorySystem = sdkModule
		.GetExportedSymbol("GetFactorySystem")
		.RCast<InstantiateInterfaceFn>();

	if (!getFactorySystem)
		return false;

	IFactorySystem* const factorySystem = static_cast<IFactorySystem*>(getFactorySystem());
	if (!factorySystem || V_strcmp(factorySystem->GetVersion(), FACTORY_INTERFACE_VERSION) != 0)
		return false;

	g_pCVar = static_cast<CCvar*>(factorySystem->GetFactory(CVAR_INTERFACE_VERSION));
	if (!g_pCVar)
		return false;

	g_pFileSystem_Stdio = static_cast<CFileSystem_Stdio*>(
		factorySystem->GetFactory(BASEFILESYSTEM_INTERFACE_VERSION));
	if (!g_pFileSystem_Stdio)
		return false;

	CModule gameModule(CModule::GetProcessEnvironmentBlock()->ImageBaseAddress);
	s_PlayerRunCommand = Module_FindPattern(
		gameModule,
		"E8 ?? ?? ?? ?? 8B 03 49 81 C6 ?? ?? ?? ??")
		.FollowNearCall()
		.RCast<PlayerRunCommandFn>();

	if (!s_PlayerRunCommand)
		return false;

	// The input interface is absent in dedicated builds. Its declared ABI puts
	// CreateMove at slot 4 and GetUserCmd(slot, sequence) at slot 9.
	const CMemory inputVtableAddress = gameModule.GetVirtualMethodTable(".?AVCInput@@");
	if (inputVtableAddress)
	{
		void** const vtable = inputVtableAddress.RCast<void**>();
		if (vtable[4] && vtable[9])
		{
			s_CreateMove = reinterpret_cast<CreateMoveFn>(vtable[4]);
			s_GetUserCmd = reinterpret_cast<GetUserCmdFn>(vtable[9]);
		}
	}

	ConVar_Register();
	if (!SetHooks(true))
	{
		ConVar_Unregister();
		return false;
	}

	s_Initialized = true;
	return true;
}

bool Shutdown()
{
	if (!s_Initialized)
		return false;

	if (!SetHooks(false))
		return false;

	ResetAllStates();
	ConVar_Unregister();
	s_Initialized = false;
	return true;
}
} // namespace

extern "C" __declspec(dllexport) bool PluginInstance_OnLoad(
	const char* /*selfModuleName*/, const char* sdkModuleName)
{
	return Initialize(sdkModuleName);
}

extern "C" __declspec(dllexport) bool PluginInstance_OnUnload()
{
	return Shutdown();
}
