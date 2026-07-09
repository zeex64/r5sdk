#include "core/stdafx.h"
#include "fowplugin.h"

#include "pluginsdk/ifactory.h"
#include "pluginsystem/ipluginsystem.h"

#include "core/logdef.h"
#include "engine/debugoverlay.h"
#include "engine/enginetrace.h"
#include "engine/client/client.h"
#include "engine/server/server.h"
#include "fow_worker.h"
#include "game/client/c_baseentity.h"
#include "game/client/cliententitylist.h"
#include "game/server/entitylist.h"
#include "game/server/player.h"
#include "public/bspflags.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "vpc/interfaces.h"
#include "vscript/languages/squirrel_re/include/sqclosure.h"
#include "vscript/languages/squirrel_re/include/sqfuncproto.h"
#include "vscript/languages/squirrel_re/include/sqstring.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"

namespace
{
constexpr size_t FOW_SNAPSHOT_RECORD_BASE = 0x50C0;
constexpr size_t FOW_SNAPSHOT_RECORD_SIZE = 24;
constexpr unsigned int FOW_TRACE_MASK = 24705;
constexpr int FOW_MAX_EDICTS = 16384;
constexpr int FOW_MAX_CLIENTS = 128;
constexpr int FOW_MAX_RELATED_EDICTS = 96;
constexpr const char* FOW_OWNER_VISUAL_CLASS_FILTER = "first_person_proxy,predicted_first_person_proxy,vortex_sphere,grapple_hook";
constexpr float FOW_MAX_PREDICTION_SPEED = 500.0f;
constexpr float FOW_MIN_PREDICTION_SPEED = 1.0f;
constexpr float FOW_MIN_LOOKAHEAD_SECONDS = 0.0f;
constexpr float FOW_MAX_LOOKAHEAD_SECONDS = 0.15f;
constexpr float FOW_PEEK_MARGIN_UNITS = 16.0f;
constexpr float FOW_BOUNDS_INFLATE = 4.0f;
constexpr float FOW_ORIGIN_VERTICAL_OFFSET = 20.0f;
constexpr float FOW_REVEAL_HOLD_SECONDS = 0.15f;
constexpr float FOW_DEFAULT_TICK_INTERVAL = 1.0f / 60.0f;
constexpr ptrdiff_t FOW_COLLISION_PROP_OFFSET = 0x328;
constexpr unsigned char FOW_SURFACEPROP_METALGRATE = 7;
constexpr unsigned char FOW_SURFACEPROP_GLASS = 16;
constexpr unsigned char FOW_SURFACEPROP_GLASS_BREAKABLE = 17;
constexpr unsigned char FOW_SURFACEPROP_BROKENGLASS = 24;

inline char(*ServerGameEnts_BuildDeltaSnapshot)(void* thisptr, int clientSlot, void* baselineFrame, void* currentFrame, void* outFrame);
inline __int64(*ServerGameEnts_BuildFullSnapshot)(void* thisptr, unsigned int clientSlot, void* currentFrame, void* outFrame);

struct FOWPlayerState
{
	bool valid;
	bool alive;
	bool fake;
	int slot;
	int edict;
	int team;
	int audioLocalBits;
	int audioEventTick;
	unsigned int audioSignature;
	Vector3D origin;
	Vector3D eye;
	Vector3D velocity;
	Vector3D mins;
	Vector3D maxs;
	float latencySeconds;
	int relatedEdicts[FOW_MAX_RELATED_EDICTS];
	int relatedEdictCount;
};

struct FOWBounds
{
	Vector3D mins;
	Vector3D maxs;
};

struct FOWFilterStats
{
	int considered;
	int hidden;
	int relatedConsidered;
	int relatedHidden;
	int lastTargetSlot;
	int lastTargetEdict;
	const char* reason;
};

struct FOWPublishedSnapshot
{
	FOWPlayerState players[FOW_MAX_CLIENTS];
	bool visible[FOW_MAX_CLIENTS][FOW_MAX_CLIENTS];
	int frameTick;
	int playerCount;
	int maxClients;
	float tickInterval;
};

struct SnapshotFrame
{
	void* unknown0;
	unsigned char* state;
	void* propBits;
	void* propData;
};

char BuildDeltaSnapshot(void* thisptr, int clientSlot, void* baselineFrame, void* currentFrame, void* outFrame);
__int64 BuildFullSnapshot(void* thisptr, unsigned int clientSlot, void* currentFrame, void* outFrame);
bool FOW_CSquirrelVM_Init(CSquirrelVM* s, SQCONTEXT context, SQFloat curtime);
ScriptStatus_t FOW_Script_ExecuteFunction(CSquirrelVM* s, HSCRIPT hFunction, const ScriptVariant_t* const pArgs, unsigned int nArgs, ScriptVariant_t* const pReturn, HSCRIPT hScope);
extern CModule g_gameModule;
extern CClientEntityList* g_fowClientEntityList;

class VFOWPluginSnapshot final : public IDetour
{
public:
	virtual void GetAdr(void) const override
	{
		LogFunAdr("ServerGameEnts002::BuildDeltaSnapshot", ServerGameEnts_BuildDeltaSnapshot);
		LogFunAdr("ServerGameEnts002::BuildFullSnapshot", ServerGameEnts_BuildFullSnapshot);
	}

	virtual void GetFun(void) const override
	{
		Module_FindPattern(g_gameModule, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 49 8B F1 49 8B E8 44 8B F2").GetPtr(ServerGameEnts_BuildDeltaSnapshot);
		Module_FindPattern(g_gameModule, "40 53 55 57 41 54 41 57 48 83 EC ?? 48 8D 0D ?? ?? ?? ?? 8B EA 49 8B D9 4D 8B F8 FF 15 ?? ?? ?? ??").GetPtr(ServerGameEnts_BuildFullSnapshot);
	}

	virtual void GetVar(void) const override { }
	virtual void GetCon(void) const override { }
	virtual void Detour(const bool bAttach) const override
	{
		DetourSetup(&ServerGameEnts_BuildDeltaSnapshot, &BuildDeltaSnapshot, bAttach);
		DetourSetup(&ServerGameEnts_BuildFullSnapshot, &BuildFullSnapshot, bAttach);
	}
};

class VFOWDisconnectVScript final : public IDetour
{
public:
	virtual void GetAdr(void) const override
	{
		LogFunAdr("CSquirrelVM::Init", CSquirrelVM__Init);
		LogFunAdr("CSquirrelVM::ExecuteFunction", CSquirrelVM__ExecuteFunction);
		LogFunAdr("C_BaseEntity::GetScriptInstance", v_C_BaseEntity__GetScriptInstance);
		LogVarAdr("g_fowClientEntityList", g_fowClientEntityList);
	}

	virtual void GetFun(void) const override
	{
		Module_FindPattern(g_gameModule, "E8 ?? ?? ?? ?? 0F 28 74 24 ?? 48 89 1D ?? ?? ?? ??").FollowNearCallSelf().GetPtr(CSquirrelVM__Init);
		Module_FindPattern(g_gameModule, "E8 ?? ?? ?? ?? 83 FB 5C").FollowNearCallSelf().GetPtr(CSquirrelVM__ExecuteFunction);
		Module_FindPattern(g_gameModule, "48 89 5C 24 ?? 56 48 83 EC ?? 80 B9 ?? ?? ?? ?? ?? 48 8B F1 75").GetPtr(v_C_BaseEntity__GetScriptInstance);
	}

	virtual void GetVar(void) const override
	{
		Module_FindPattern(g_gameModule, "48 8D 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 44 89 0D").
			ResolveRelativeAddressSelf(3, 7).ResolveRelativeAddressSelf(3, 7).GetPtr(g_fowClientEntityList);
	}

	virtual void GetCon(void) const override { }
	virtual void Detour(const bool bAttach) const override
	{
		DetourSetup(&CSquirrelVM__Init, &FOW_CSquirrelVM_Init, bAttach);
		DetourSetup(&CSquirrelVM__ExecuteFunction, &FOW_Script_ExecuteFunction, bAttach);
	}
};

IPluginSystem* g_pluginSystem = nullptr;
IFactorySystem* g_factorySystem = nullptr;
CModule g_selfModule;
CModule g_sdkModule;
CModule g_gameModule;
CBaseEntityList* g_entityList = nullptr;
CEngineTraceServer* g_engineTraceServer = nullptr;
CEngineTraceServer* g_engineTraceServerVFTable = nullptr;
CIVDebugOverlay* g_debugOverlay = nullptr;
CServer* g_lastServer = nullptr;

FOWPublishedSnapshot g_fowSnapshots[2];
std::atomic<int> g_fowPublishedSnapshot{ 0 };
bool g_fowLastHidden[FOW_MAX_CLIENTS][FOW_MAX_CLIENTS];
int g_fowRevealUntilTick[FOW_MAX_CLIENTS][FOW_MAX_CLIENTS];
CThreadMutex g_fowCaptureMutex;
thread_local int g_fowLogCount;
bool g_snapshotDetoursInstalled = false;
VFOWPluginSnapshot g_snapshotDetour;
bool g_disconnectVScriptDetoursInstalled = false;
VFOWDisconnectVScript g_disconnectVScriptDetour;
CUtlVector<int> g_pendingRealDisconnectHandles;
CClientEntityList* g_fowClientEntityList = nullptr;

static ConVar fow_enable("fow_enable", "0", FCVAR_RELEASE, "Enables LOS-based server-side fog-of-war snapshot filtering.", false, 0.f, true, 1.f);
static ConVar fow_debug("fow_debug", "0", FCVAR_DEVELOPMENTONLY, "Enables fog-of-war diagnostic logging.", false, 0.f, true, 1.f);
static ConVar fow_debug_snapshots("fow_debug_snapshots", "0", FCVAR_DEVELOPMENTONLY, "Logs every fog-of-war snapshot hook call instead of throttling.", false, 0.f, true, 1.f);
static ConVar fow_trace_duration("fow_trace_duration", "5", FCVAR_DEVELOPMENTONLY, "Debug overlay lifetime for fog-of-war trace diagnostics.", true, 0.f, false, 0.f);
static ConVar fow_reveal_radius("fow_reveal_radius", "1500", FCVAR_DEVELOPMENTONLY, "Keeps enemy players visible inside this radius even without LOS. 0 disables the radius override.", true, 0.f, false, 0.f);
static ConVar fow_audio_reveal("fow_audio_reveal", "1", FCVAR_DEVELOPMENTONLY, "Keeps enemy players transmitted while their local audio is active.", false, 0.f, true, 1.f);
static ConVar fow_audio_reveal_hold("fow_audio_reveal_hold", "1.25", FCVAR_DEVELOPMENTONLY, "Keeps enemy players transmitted for this many seconds after a detected audio event.", true, 0.f, false, 0.f);
static ConVar fow_peek_enable("fow_peek_enable", "1", FCVAR_DEVELOPMENTONLY, "Enables early reveal LOS traces from nearby peek-assist positions.", false, 0.f, true, 1.f);
static ConVar fow_peek_forward("fow_peek_forward", "64", FCVAR_DEVELOPMENTONLY, "Forward peek-assist trace offset toward the target.", true, 0.f, false, 0.f);
static ConVar fow_peek_side("fow_peek_side", "32", FCVAR_DEVELOPMENTONLY, "Side peek-assist trace offset perpendicular to the target direction.", true, 0.f, false, 0.f);
static ConVar fow_peek_up("fow_peek_up", "8", FCVAR_DEVELOPMENTONLY, "Upward peek-assist trace offset.", true, 0.f, false, 0.f);

void PluginLoggerSink(LogType_t logType, LogLevel_t logLevel, eDLL_T context,
	const char* pszLogger, const char* pszFormat, va_list args,
	const UINT exitCode, const char* pszUptimeOverride)
{
	if (g_pluginSystem)
		g_pluginSystem->CoreMsgV(logType, logLevel, context, pszLogger, pszFormat, args, exitCode, pszUptimeOverride);
}

bool ResolveRuntimePointers()
{
	if (!g_debugOverlay && g_factorySystem)
		g_debugOverlay = reinterpret_cast<CIVDebugOverlay*>(g_factorySystem->GetFactory(VDEBUG_OVERLAY_INTERFACE_VERSION));

	if (!g_entityList)
	{
		g_entityList = Module_FindPattern(g_gameModule, "48 8D 0D ?? ?? ?? ?? 66 0F 7F 05 ?? ?? ?? ?? 44 89 0D")
			.ResolveRelativeAddressSelf(3, 7)
			.ResolveRelativeAddressSelf(3, 7)
			.RCast<CBaseEntityList*>();
	}

	if (!g_engineTraceServer)
	{
		g_engineTraceServerVFTable = g_gameModule.GetVirtualMethodTable(".?AVCEngineTraceServer@@").RCast<CEngineTraceServer*>();
		if (g_engineTraceServerVFTable)
			g_engineTraceServer = reinterpret_cast<CEngineTraceServer*>(&g_engineTraceServerVFTable);
	}

	return g_entityList != nullptr;
}

void ClearPendingRealDisconnects()
{
	g_pendingRealDisconnectHandles.Purge();
}

bool TryGetScriptIntArg(const ScriptVariant_t& arg, int& value)
{
	switch (arg.m_type)
	{
	case FIELD_INTEGER:
	case FIELD_EHANDLE:
		value = arg.m_int;
		return true;
	default:
		return false;
	}
}

bool HasScriptStringArg(const ScriptVariant_t& arg)
{
	return (arg.m_type == FIELD_CSTRING || arg.m_type == FIELD_OSTRING) &&
		arg.m_pszString && arg.m_pszString[0] != '\0';
}

void RememberRealDisconnectHandle(const int handle)
{
	if (handle == INVALID_EHANDLE_INDEX)
		return;

	if (g_pendingRealDisconnectHandles.Find(handle) == g_pendingRealDisconnectHandles.InvalidIndex())
		g_pendingRealDisconnectHandles.AddToTail(handle);
}

bool ConsumePendingRealDisconnectHandle(const int handle)
{
	if (handle != INVALID_EHANDLE_INDEX)
	{
		const int index = g_pendingRealDisconnectHandles.Find(handle);
		if (index != g_pendingRealDisconnectHandles.InvalidIndex())
		{
			g_pendingRealDisconnectHandles.FastRemove(index);
			return true;
		}
	}

	if (g_pendingRealDisconnectHandles.Count() > 0)
	{
		g_pendingRealDisconnectHandles.FastRemove(0);
		return true;
	}

	return false;
}

int FindClientEntityHandleFromScriptInstance(const HSCRIPT scriptInstance)
{
	if (!g_fowClientEntityList || !scriptInstance || scriptInstance == INVALID_HSCRIPT || !v_C_BaseEntity__GetScriptInstance)
		return INVALID_EHANDLE_INDEX;

	for (int index = 0; index < NUM_ENT_ENTRIES; ++index)
	{
		const C_EntInfo* const info = g_fowClientEntityList->GetEntInfoPtrByIndex(index);
		if (!info || !info->m_pEntity)
			continue;

		IClientUnknown* const unk = static_cast<IClientUnknown*>(info->m_pEntity);
		C_BaseEntity* const entity = unk ? unk->GetBaseEntity() : nullptr;
		if (!entity)
			continue;

		if (v_C_BaseEntity__GetScriptInstance(entity) == scriptInstance)
			return CBaseHandle(index, info->m_SerialNumber).ToInt();
	}

	return INVALID_EHANDLE_INDEX;
}

int TryResolveDisconnectHandle(const ScriptVariant_t& arg)
{
	int handle = INVALID_EHANDLE_INDEX;
	if (TryGetScriptIntArg(arg, handle))
		return handle;

	if (arg.m_type == FIELD_HSCRIPT)
		return FindClientEntityHandleFromScriptInstance(arg.m_hScript);

	return INVALID_EHANDLE_INDEX;
}

void MaybeTrackRealDisconnectCallback(const char* const functionName,
	const ScriptVariant_t* const pArgs, const unsigned int nArgs)
{
	if (!functionName || strcmp(functionName, "ServerCallback_PlayerConnectedOrDisconnected") != 0)
		return;

	if (!pArgs || nArgs < 2)
		return;

	int handle = INVALID_EHANDLE_INDEX;
	int state = -1;
	if (!TryGetScriptIntArg(pArgs[0], handle) || !TryGetScriptIntArg(pArgs[1], state))
		return;

	if (state == 0)
		RememberRealDisconnectHandle(handle);
}

bool ShouldSuppressLocalDisconnectCallback(const char* const functionName,
	const ScriptVariant_t* const pArgs, const unsigned int nArgs)
{
	if (!functionName || strcmp(functionName, "ClientCodeCallback_PlayerDisconnected") != 0)
		return false;

	if (!pArgs || nArgs < 2)
		return false;

	const int handle = TryResolveDisconnectHandle(pArgs[0]);
	if (ConsumePendingRealDisconnectHandle(handle))
		return false;

	return HasScriptStringArg(pArgs[1]);
}

bool FOW_CSquirrelVM_Init(CSquirrelVM* s, SQCONTEXT context, SQFloat curtime)
{
	const bool result = CSquirrelVM__Init(s, context, curtime);
	if (result && context == SQCONTEXT::CLIENT)
		ClearPendingRealDisconnects();

	return result;
}

ScriptStatus_t FOW_Script_ExecuteFunction(CSquirrelVM* s, HSCRIPT hFunction, const ScriptVariant_t* const pArgs, unsigned int nArgs, ScriptVariant_t* const pReturn, HSCRIPT hScope)
{
	if (s && hFunction && s->GetContext() == SQCONTEXT::CLIENT)
	{
		const SQObjectPtr* const f = reinterpret_cast<SQObjectPtr*>(hFunction);
		const SQClosure* const closure = _closure(*f);
		const SQFunctionProto* const fp = closure ? _funcproto(closure->_function) : nullptr;
		const char* const functionName = fp ? _stringval(fp->_funcname) : nullptr;

		if (functionName)
		{
			MaybeTrackRealDisconnectCallback(functionName, pArgs, nArgs);
			if (ShouldSuppressLocalDisconnectCallback(functionName, pArgs, nArgs))
				return SCRIPT_DONE;
		}
	}

	return CSquirrelVM__ExecuteFunction(s, hFunction, pArgs, nArgs, pReturn, hScope);
}

bool SetDisconnectVScriptDetours(const bool attach)
{
	if (attach)
	{
		g_disconnectVScriptDetour.GetFun();
		g_disconnectVScriptDetour.GetVar();
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	g_disconnectVScriptDetour.Detour(attach);
	const LONG result = DetourTransactionCommit();
	if (result != NO_ERROR)
		return false;

	g_disconnectVScriptDetoursInstalled = attach;
	return true;
}

int GetMaxClientSlots(CServer* const server)
{
	if (!server)
		return 0;

	return Min(server->GetMaxClients(), FOW_MAX_CLIENTS);
}

float GetServerTickInterval(CServer* const server)
{
	if (!server)
		return FOW_DEFAULT_TICK_INTERVAL;

	const int tick = server->GetTick();
	const float time = server->GetTime();
	if (tick > 0 && time > 0.0f)
		return Max(time / static_cast<float>(tick), 0.0f);

	return FOW_DEFAULT_TICK_INTERVAL;
}

float GetClientLatencySeconds(CClient* const client)
{
	if (!client || client->IsFakeClient())
		return 0.0f;

	const CNetChan* const netChan = client->GetNetChan();
	if (!netChan)
		return 0.0f;

	const float outgoing = Max(0.0f, netChan->GetAvgLatency(FLOW_OUTGOING));
	const float incoming = Max(0.0f, netChan->GetAvgLatency(FLOW_INCOMING));
	return outgoing + incoming;
}

int GetRevealHoldTicks(const FOWPublishedSnapshot& snapshot)
{
	if (snapshot.tickInterval <= 0.0f)
		return 1;

	return Max(1, static_cast<int>(ceilf(FOW_REVEAL_HOLD_SECONDS / snapshot.tickInterval)));
}

int GetAudioRevealHoldTicks(const FOWPublishedSnapshot& snapshot)
{
	if (snapshot.tickInterval <= 0.0f)
		return 1;

	return Max(1, static_cast<int>(ceilf(fow_audio_reveal_hold.GetFloat() / snapshot.tickInterval)));
}

void HashAudioBytes(unsigned int& hash, const void* const data, const size_t size)
{
	const unsigned char* bytes = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < size; ++i)
	{
		hash ^= bytes[i];
		hash *= 16777619u;
	}
}

unsigned int BuildAudioSignature(CPlayer* const player)
{
	if (!player)
		return 0u;

	unsigned int hash = 2166136261u;
	HashAudioBytes(hash, player->GetLastBodySound3P(), 32);
	HashAudioBytes(hash, player->GetLastFinishSound3P(), 32);
	HashAudioBytes(hash, player->GetPrimedSound3P(), 32);
	HashAudioBytes(hash, player->GetReplayImportantSoundIDs(), sizeof(int) * 4);
	HashAudioBytes(hash, player->GetReplayImportantSoundBeginTimes(), sizeof(float) * 4);
	const float lastDamaged = player->GetLastTimeDamagedByOtherPlayer();
	HashAudioBytes(hash, &lastDamaged, sizeof(lastDamaged));
	return hash;
}

float GetEffectiveLookaheadSeconds(const FOWPlayerState& observer, const float tickInterval)
{
	const float wanted = Max(observer.latencySeconds + Max(tickInterval, 0.0f), FOW_MIN_LOOKAHEAD_SECONDS);
	return Min(wanted, FOW_MAX_LOOKAHEAD_SECONDS);
}

Vector3D GetPredictionOffset(const Vector3D& velocity, const float lookaheadSeconds)
{
	if (lookaheadSeconds <= 0.0f)
		return vec3_origin;

	const float speedSqr = (velocity.x * velocity.x) + (velocity.y * velocity.y);
	if (speedSqr <= (FOW_MIN_PREDICTION_SPEED * FOW_MIN_PREDICTION_SPEED))
		return vec3_origin;

	const float speed = sqrtf(speedSqr);
	const float cappedSpeed = Min(speed, FOW_MAX_PREDICTION_SPEED);
	const float distance = Max(cappedSpeed * lookaheadSeconds, FOW_PEEK_MARGIN_UNITS);
	const float scale = distance / speed;

	return Vector3D(velocity.x * scale, velocity.y * scale, 0.0f);
}

Vector3D GetBoundsCenter(const FOWBounds& bounds)
{
	return Vector3D(
		(bounds.mins.x + bounds.maxs.x) * 0.5f,
		(bounds.mins.y + bounds.maxs.y) * 0.5f,
		(bounds.mins.z + bounds.maxs.z) * 0.5f);
}

FOWBounds BuildPlayerBounds(const FOWPlayerState& player, const Vector3D& origin)
{
	return {
		Vector3D(origin.x + player.mins.x - FOW_BOUNDS_INFLATE,
			origin.y + player.mins.y - FOW_BOUNDS_INFLATE,
			origin.z + player.mins.z - FOW_BOUNDS_INFLATE),
		Vector3D(origin.x + player.maxs.x + FOW_BOUNDS_INFLATE,
			origin.y + player.maxs.y + FOW_BOUNDS_INFLATE,
			origin.z + player.maxs.z + FOW_BOUNDS_INFLATE)
	};
}

FOWBounds MergeBounds(const FOWBounds& lhs, const FOWBounds& rhs)
{
	return {
		Vector3D(Min(lhs.mins.x, rhs.mins.x), Min(lhs.mins.y, rhs.mins.y), Min(lhs.mins.z, rhs.mins.z)),
		Vector3D(Max(lhs.maxs.x, rhs.maxs.x), Max(lhs.maxs.y, rhs.maxs.y), Max(lhs.maxs.z, rhs.maxs.z))
	};
}

int HandleToEdict(const CBaseHandle& handle)
{
	if (!handle.IsValid())
		return -1;

	const int edict = handle.ToInt() & ENT_ENTRY_MASK;
	return edict > 0 && edict < FOW_MAX_EDICTS ? edict : -1;
}

CBaseEntity* ResolveHandleEntity(const CBaseHandle& handle)
{
	if (!g_entityList)
		return nullptr;

	return static_cast<CBaseEntity*>(g_entityList->LookupEntity(handle));
}

const CCollisionProperty* GetCollisionProperty(CBaseEntity* const entity)
{
	if (!entity)
		return nullptr;

	return reinterpret_cast<const CCollisionProperty*>(reinterpret_cast<const unsigned char*>(entity) + FOW_COLLISION_PROP_OFFSET);
}

CPlayer* GetPlayerByEdict(const int edict)
{
	if (!g_entityList || edict <= 0 || edict >= FOW_MAX_EDICTS)
		return nullptr;

	return static_cast<CPlayer*>(g_entityList->LookupEntityByNetworkIndex(edict));
}

const CEntInfo* GetEntInfo(const int edict)
{
	if (!g_entityList || edict <= 0 || edict >= FOW_MAX_EDICTS)
		return nullptr;

	return g_entityList->GetEntInfoPtrByIndex(edict);
}

int GetEdictFromEntInfo(const CEntInfo* const info)
{
	if (!g_entityList || !info)
		return -1;

	const CEntInfo* const base = g_entityList->GetEntInfoPtrByIndex(0);
	const ptrdiff_t index = info - base;
	return index > 0 && index < FOW_MAX_EDICTS ? static_cast<int>(index) : -1;
}

const char* GetEntityClassName(const int edict)
{
	const CEntInfo* const info = GetEntInfo(edict);
	return info ? STRING(info->m_iClassName) : "";
}

const char* GetEntityModelName(CBaseEntity* const entity)
{
	return entity ? STRING(entity->GetModelName()) : "";
}

bool StringMatchesTokenList(const char* const tokenList, const char* const value)
{
	if (!tokenList || !*tokenList || !value || !*value)
		return false;

	const char* token = tokenList;
	while (*token)
	{
		while (*token == ' ' || *token == '\t' || *token == ',')
			++token;

		if (!*token)
			break;

		const char* tokenEnd = token;
		while (*tokenEnd && *tokenEnd != ',')
			++tokenEnd;

		char pattern[128];
		int length = static_cast<int>(tokenEnd - token);
		while (length > 0 && (token[length - 1] == ' ' || token[length - 1] == '\t'))
			--length;

		if (length > 0)
		{
			const int copyLength = Min(length, static_cast<int>(sizeof(pattern)) - 1);
			memcpy(pattern, token, copyLength);
			pattern[copyLength] = '\0';
			if (V_stristr(value, pattern))
				return true;
		}

		token = tokenEnd;
		if (*token == ',')
			++token;
	}

	return false;
}

void ResetVisibility(FOWPublishedSnapshot& snapshot)
{
	for (int observer = 0; observer < FOW_MAX_CLIENTS; ++observer)
	{
		for (int target = 0; target < FOW_MAX_CLIENTS; ++target)
			snapshot.visible[observer][target] = true;
	}
}

void ClearPlayerStates(FOWPublishedSnapshot& snapshot)
{
	snapshot.playerCount = 0;
	snapshot.frameTick = -1;
	snapshot.maxClients = 0;
	snapshot.tickInterval = FOW_DEFAULT_TICK_INTERVAL;
	for (int i = 0; i < FOW_MAX_CLIENTS; ++i)
		snapshot.players[i] = {};
}

void AddRelatedEdict(FOWPlayerState& state, const int edict)
{
	if (edict <= 0 || edict >= FOW_MAX_EDICTS || edict == state.edict)
		return;

	for (int i = 0; i < state.relatedEdictCount; ++i)
	{
		if (state.relatedEdicts[i] == edict)
			return;
	}

	if (state.relatedEdictCount >= static_cast<int>(SDK_ARRAYSIZE(state.relatedEdicts)))
		return;

	state.relatedEdicts[state.relatedEdictCount++] = edict;
}

void AddRelatedEntityChildren(FOWPlayerState& state, CBaseEntity* const entity, const int depth)
{
	if (!entity || depth > 2)
		return;

	CBaseHandle childHandle = entity->GetMoveChildHandle();
	while (childHandle.IsValid())
	{
		CBaseEntity* const child = ResolveHandleEntity(childHandle);
		const int childEdict = HandleToEdict(childHandle);
		const int previousCount = state.relatedEdictCount;
		AddRelatedEdict(state, childEdict);
		if (child && state.relatedEdictCount > previousCount)
			AddRelatedEntityChildren(state, child, depth + 1);

		childHandle = child ? CBaseHandle(child->GetMovePeerHandle()) : CBaseHandle();
	}
}

void AddRelatedHandle(FOWPlayerState& state, const CBaseHandle& handle)
{
	const int edict = HandleToEdict(handle);
	if (edict <= 0)
		return;

	const int previousCount = state.relatedEdictCount;
	AddRelatedEdict(state, edict);
	if (state.relatedEdictCount <= previousCount)
		return;

	AddRelatedEntityChildren(state, ResolveHandleEntity(handle), 0);
}

bool IsTrackedEntityEdict(const FOWPlayerState& state, const int edict)
{
	if (edict <= 0)
		return false;

	if (edict == state.edict)
		return true;

	for (int i = 0; i < state.relatedEdictCount; ++i)
	{
		if (state.relatedEdicts[i] == edict)
			return true;
	}

	return false;
}

void CaptureOwnerVisualCandidates(FOWPlayerState& state)
{
	if (!g_entityList)
		return;

	const char* const classFilter = FOW_OWNER_VISUAL_CLASS_FILTER;
	if (!classFilter || !*classFilter)
		return;

	for (const CEntInfo* info = g_entityList->FirstEntInfo(); info; info = g_entityList->NextEntInfo(info))
	{
		CBaseEntity* const entity = static_cast<CBaseEntity*>(info->m_pEntity);
		if (!entity)
			continue;

		const int edict = GetEdictFromEntInfo(info);
		if (edict <= 0 || edict >= FOW_MAX_EDICTS)
			continue;

		if (IsTrackedEntityEdict(state, edict))
			continue;

		const int ownerEdict = HandleToEdict(entity->GetOwnerEntityHandle());
		const int moveParentEdict = HandleToEdict(entity->GetMoveParentHandle());
		if (ownerEdict != state.edict && moveParentEdict != state.edict)
			continue;

		const char* const className = GetEntityClassName(edict);
		if (!StringMatchesTokenList(classFilter, className))
			continue;

		const int previousCount = state.relatedEdictCount;
		AddRelatedEdict(state, edict);
		if (state.relatedEdictCount > previousCount)
			AddRelatedEntityChildren(state, entity, 0);
	}
}

void CaptureRelatedEntities(FOWPlayerState& state, CPlayer* const player)
{
	if (!player)
		return;

	AddRelatedHandle(state, player->GetThirdPersonEnt());

	const EHANDLE* const viewModels = player->GetViewModels();
	for (int i = 0; i < 3; ++i)
		AddRelatedHandle(state, viewModels[i]);

	const WeaponInventory& inventory = player->GetWeaponInventory();
	for (int i = 0; i < 9; ++i)
		AddRelatedHandle(state, inventory.weapons[i]);
	for (int i = 0; i < 6; ++i)
		AddRelatedHandle(state, inventory.offhandWeapons[i]);
	for (int i = 0; i < 3; ++i)
		AddRelatedHandle(state, inventory.activeWeapons[i]);

	const EHANDLE* const latestPrimary = player->GetLatestPrimaryWeapons();
	for (int i = 0; i < 2; ++i)
		AddRelatedHandle(state, latestPrimary[i]);

	const EHANDLE* const latest3p = player->GetLatest3pWeapons();
	for (int i = 0; i < 2; ++i)
		AddRelatedHandle(state, latest3p[i]);

	const EHANDLE* const switching = player->GetSwitchingWeapons();
	for (int i = 0; i < 2; ++i)
		AddRelatedHandle(state, switching[i]);

	CaptureOwnerVisualCandidates(state);
}

bool IsCullablePair(const FOWPublishedSnapshot& snapshot, const int observerSlot, const int targetSlot, const char** const reason)
{
	if (observerSlot < 0 || targetSlot < 0 || observerSlot >= snapshot.maxClients || targetSlot >= snapshot.maxClients)
	{
		*reason = "bad_slot";
		return false;
	}

	const FOWPlayerState& observer = snapshot.players[observerSlot];
	const FOWPlayerState& target = snapshot.players[targetSlot];
	if (!observer.valid || !target.valid)
	{
		*reason = "invalid";
		return false;
	}

	if (!observer.alive || !target.alive)
	{
		*reason = "dead";
		return false;
	}

	if (observer.edict == target.edict)
	{
		*reason = "self";
		return false;
	}

	if (observer.team == target.team)
	{
		*reason = "teammate";
		return false;
	}

	*reason = "matrix";
	return true;
}

bool TraceSegment(const Vector3D& start, const Vector3D& end, const unsigned int mask, trace_t* const trace)
{
	if (!g_engineTraceServer || !trace)
		return false;

	Ray_t ray(start, end);
	*trace = {};
	g_engineTraceServer->TraceRay(ray, mask, trace);
	return true;
}

bool IsTraceClear(const trace_t& trace)
{
	return !trace.startsolid && !trace.allsolid && trace.fraction >= 0.99f;
}

bool IsTransparentTraceHit(const trace_t& trace)
{
	if (trace.startsolid || trace.allsolid)
		return false;

	switch (trace.surface.surfaceProp)
	{
	case FOW_SURFACEPROP_METALGRATE:
	case FOW_SURFACEPROP_GLASS:
	case FOW_SURFACEPROP_GLASS_BREAKABLE:
	case FOW_SURFACEPROP_BROKENGLASS:
		return true;
	default:
		break;
	}

	if (trace.contents & (CONTENTS_WINDOW | CONTENTS_WINDOW_NOCOLLIDE | CONTENTS_TRANSLUCENT))
		return true;

	if (trace.contents & CONTENTS_GRATE)
		return true;

	return (trace.surface.flags & SURF_TRANS) != 0;
}

Vector3D AdvancePastTraceHit(const Vector3D& start, const Vector3D& end, const trace_t& trace)
{
	Vector3D direction = end - start;
	const float lengthSqr = direction.LengthSqr();
	if (lengthSqr <= 0.0f)
		return trace.endpos;

	const float invLength = 1.0f / sqrtf(lengthSqr);
	direction *= invLength;
	return trace.endpos + (direction * 1.0f);
}

void DrawTraceDebug(const Vector3D& start, const Vector3D& end, const trace_t& trace, const bool clear)
{
	if (!g_debugOverlay || fow_trace_duration.GetFloat() <= 0.0f)
		return;

	const float duration = fow_trace_duration.GetFloat();
	if (clear)
	{
		g_debugOverlay->AddLineOverlay(start, end, 0, 255, 0, true, duration);
		return;
	}

	g_debugOverlay->AddLineOverlay(start, trace.endpos, 255, 255, 0, true, duration);
	g_debugOverlay->AddLineOverlay(trace.endpos, end, 255, 0, 0, true, duration);
	g_debugOverlay->AddSphereOverlay(trace.endpos, 8.0f, 8, 6, 255, 0, 0, 180, true, duration);
}

bool IsSampleVisible(const Vector3D& observerEye, const Vector3D& targetPoint, const unsigned int mask)
{
	Vector3D traceStart = observerEye;
	constexpr int maxTransparentSkips = 4;

	for (int attempt = 0; attempt <= maxTransparentSkips; ++attempt)
	{
		trace_t trace;
		if (!TraceSegment(traceStart, targetPoint, mask, &trace))
			return true;

		if (IsTraceClear(trace))
			return true;

		if (!IsTransparentTraceHit(trace) || attempt == maxTransparentSkips)
			return false;

		const Vector3D nextStart = AdvancePastTraceHit(traceStart, targetPoint, trace);
		if ((nextStart - traceStart).LengthSqr() <= 0.0001f)
			return false;

		traceStart = nextStart;
	}

	return false;
}

bool IsOriginReachable(const Vector3D& eye, const Vector3D& candidate, const unsigned int mask)
{
	if ((candidate - eye).LengthSqr() <= 0.0001f)
		return true;

	return IsSampleVisible(eye, candidate, mask);
}

int BuildObserverOrigins(const FOWPlayerState& observer, const float tickInterval, Vector3D* const outOrigins, const int maxOrigins)
{
	if (!outOrigins || maxOrigins <= 0)
		return 0;

	int count = 0;
	outOrigins[count++] = observer.eye;

	if (!fow_peek_enable.GetBool() || count >= maxOrigins)
		return count;

	const float lookaheadSeconds = GetEffectiveLookaheadSeconds(observer, tickInterval);
	const Vector3D predictedOffset = GetPredictionOffset(observer.velocity, lookaheadSeconds);
	const Vector3D origins[] = {
		observer.eye + predictedOffset,
		Vector3D(observer.eye.x, observer.eye.y, observer.eye.z + FOW_ORIGIN_VERTICAL_OFFSET),
		Vector3D(observer.eye.x, observer.eye.y, observer.eye.z - FOW_ORIGIN_VERTICAL_OFFSET),
		Vector3D(observer.eye.x + predictedOffset.x, observer.eye.y + predictedOffset.y, observer.eye.z + FOW_ORIGIN_VERTICAL_OFFSET),
		Vector3D(observer.eye.x + predictedOffset.x, observer.eye.y + predictedOffset.y, observer.eye.z - FOW_ORIGIN_VERTICAL_OFFSET)
	};

	for (const Vector3D& origin : origins)
	{
		if (count >= maxOrigins)
			break;

		outOrigins[count++] = IsOriginReachable(observer.eye, origin, FOW_TRACE_MASK) ? origin : observer.eye;
	}

	return count;
}

int BuildTargetSamples(const FOWPlayerState& target, const float lookaheadSeconds, Vector3D* const outSamples, const int maxSamples)
{
	if (!outSamples || maxSamples < 16)
		return 0;

	FOWBounds bounds = BuildPlayerBounds(target, target.origin);
	const Vector3D targetOffset = GetPredictionOffset(target.velocity, lookaheadSeconds);
	if (targetOffset.LengthSqr() > 0.0001f)
		bounds = MergeBounds(bounds, BuildPlayerBounds(target, target.origin + targetOffset));

	const Vector3D middle = GetBoundsCenter(bounds);
	const Vector3D upper(middle.x, middle.y, bounds.mins.z + ((bounds.maxs.z - bounds.mins.z) * 0.75f));

	outSamples[0] = Vector3D(bounds.mins.x, bounds.mins.y, bounds.mins.z);
	outSamples[1] = Vector3D(bounds.maxs.x, bounds.mins.y, bounds.mins.z);
	outSamples[2] = Vector3D(bounds.mins.x, bounds.maxs.y, bounds.mins.z);
	outSamples[3] = Vector3D(bounds.maxs.x, bounds.maxs.y, bounds.mins.z);
	outSamples[4] = Vector3D(bounds.mins.x, bounds.mins.y, bounds.maxs.z);
	outSamples[5] = Vector3D(bounds.maxs.x, bounds.mins.y, bounds.maxs.z);
	outSamples[6] = Vector3D(bounds.mins.x, bounds.maxs.y, bounds.maxs.z);
	outSamples[7] = Vector3D(bounds.maxs.x, bounds.maxs.y, bounds.maxs.z);
	outSamples[8] = Vector3D(bounds.mins.x, middle.y, middle.z);
	outSamples[9] = Vector3D(bounds.maxs.x, middle.y, middle.z);
	outSamples[10] = Vector3D(middle.x, bounds.mins.y, middle.z);
	outSamples[11] = Vector3D(middle.x, bounds.maxs.y, middle.z);
	outSamples[12] = Vector3D(middle.x, middle.y, bounds.mins.z);
	outSamples[13] = Vector3D(middle.x, middle.y, bounds.maxs.z);
	outSamples[14] = middle;
	outSamples[15] = upper;
	return 16;
}

bool IsTargetVisibleFromEye(const Vector3D& observerEye, const FOWPlayerState& observer, const FOWPlayerState& target, const float tickInterval)
{
	Vector3D samples[16];
	const int sampleCount = BuildTargetSamples(target, GetEffectiveLookaheadSeconds(observer, tickInterval), samples, SDK_ARRAYSIZE(samples));
	for (int i = 0; i < sampleCount; ++i)
	{
		if (IsSampleVisible(observerEye, samples[i], FOW_TRACE_MASK))
			return true;
	}

	return false;
}

bool PlayerHasLineOfSight(const FOWPlayerState& observer, const FOWPlayerState& target, const float tickInterval)
{
	Vector3D origins[6];
	const int originCount = BuildObserverOrigins(observer, tickInterval, origins, SDK_ARRAYSIZE(origins));
	for (int i = 0; i < originCount; ++i)
	{
		if (IsTargetVisibleFromEye(origins[i], observer, target, tickInterval))
			return true;
	}

	if (!fow_peek_enable.GetBool())
		return false;

	Vector3D toward = target.eye - observer.eye;
	toward.z = 0.0f;

	const float lengthSqr = toward.x * toward.x + toward.y * toward.y;
	if (lengthSqr <= 1.0f)
		return false;

	const float invLength = 1.0f / sqrtf(lengthSqr);
	toward.x *= invLength;
	toward.y *= invLength;

	const Vector3D side(-toward.y, toward.x, 0.0f);
	const float forward = fow_peek_forward.GetFloat();
	const float sideOffset = fow_peek_side.GetFloat();
	const float up = fow_peek_up.GetFloat();

	Vector3D sample = observer.eye;
	sample.x += toward.x * forward;
	sample.y += toward.y * forward;
	sample.z += up;
	if (IsTargetVisibleFromEye(sample, observer, target, tickInterval))
		return true;

	sample = observer.eye;
	sample.x += side.x * sideOffset;
	sample.y += side.y * sideOffset;
	sample.z += up;
	if (IsTargetVisibleFromEye(sample, observer, target, tickInterval))
		return true;

	sample = observer.eye;
	sample.x -= side.x * sideOffset;
	sample.y -= side.y * sideOffset;
	sample.z += up;
	return IsTargetVisibleFromEye(sample, observer, target, tickInterval);
}

bool IsWithinRevealRadius(const FOWPlayerState& observer, const FOWPlayerState& target)
{
	const float radius = fow_reveal_radius.GetFloat();
	if (radius <= 0.0f)
		return false;

	const float radiusSqr = radius * radius;
	return (target.eye - observer.eye).LengthSqr() <= radiusSqr;
}

void BuildVisibilityMatrixRaw(FOWPublishedSnapshot& snapshot)
{
	ResetVisibility(snapshot);

	if (!fow_enable.GetBool())
		return;

	for (int observerSlot = 0; observerSlot < snapshot.maxClients; ++observerSlot)
	{
		const FOWPlayerState& observer = snapshot.players[observerSlot];
		if (!observer.valid || !observer.alive)
			continue;

		for (int targetSlot = 0; targetSlot < snapshot.maxClients; ++targetSlot)
		{
			const char* reason = nullptr;
			if (!IsCullablePair(snapshot, observerSlot, targetSlot, &reason))
				continue;

			const FOWPlayerState& target = snapshot.players[targetSlot];
			const bool inRevealRadius = IsWithinRevealRadius(observer, target);
			const bool hasLos = inRevealRadius || PlayerHasLineOfSight(observer, target, snapshot.tickInterval);
			if (!hasLos)
				snapshot.visible[observerSlot][targetSlot] = false;
		}
	}
}

void BuildVisibilityMatrixWorker(void* snapshotData)
{
	if (!snapshotData)
		return;

	BuildVisibilityMatrixRaw(*static_cast<FOWPublishedSnapshot*>(snapshotData));
}

FOWPublishedSnapshot& GetBuildSnapshot()
{
	const int published = g_fowPublishedSnapshot.load(std::memory_order_relaxed);
	return g_fowSnapshots[published ^ 1];
}

const FOWPublishedSnapshot& GetPublishedSnapshot()
{
	return g_fowSnapshots[g_fowPublishedSnapshot.load(std::memory_order_acquire)];
}

void PublishCurrentBuildSnapshot()
{
	const int published = g_fowPublishedSnapshot.load(std::memory_order_relaxed);
	g_fowPublishedSnapshot.store(published ^ 1, std::memory_order_release);
}

void CopyAndPublishSnapshot(const FOWPublishedSnapshot& source)
{
	FOWPublishedSnapshot& build = GetBuildSnapshot();
	build = source;
	PublishCurrentBuildSnapshot();
}

void SubmitAsyncSnapshot(const FOWPublishedSnapshot& snapshot)
{
	WorkerSubmit(&snapshot, sizeof(snapshot), BuildVisibilityMatrixWorker);
}

bool TryPublishAsyncResultLocked()
{
	FOWPublishedSnapshot snapshot;
	if (!WorkerTryConsumeResult(&snapshot, sizeof(snapshot)))
		return false;

	CopyAndPublishSnapshot(snapshot);
	return true;
}

void CapturePlayersLocked(CServer* const server)
{
	const FOWPublishedSnapshot& previous = GetPublishedSnapshot();
	FOWPublishedSnapshot& snapshot = GetBuildSnapshot();
	ClearPlayerStates(snapshot);
	ResetVisibility(snapshot);

	if (!server || !server->IsActive() || !ResolveRuntimePointers())
		return;

	const int maxClients = GetMaxClientSlots(server);
	snapshot.maxClients = maxClients;
	snapshot.tickInterval = GetServerTickInterval(server);

	for (int slot = 0; slot < maxClients; ++slot)
	{
		CClient* const client = server->GetClient(slot);
		if (!client || !client->IsConnected())
			continue;

		const edict_t edict = client->GetHandle();
		if (edict <= 0 || edict >= FOW_MAX_EDICTS)
			continue;

		CPlayer* const player = GetPlayerByEdict(edict);
		if (!player)
			continue;

		FOWPlayerState& state = snapshot.players[slot];
		state.valid = true;
		state.alive = player->GetLifeState() == 0;
		state.fake = client->IsFakeClient();
		state.slot = slot;
		state.edict = edict;
		state.team = player->GetTeamNum();
		state.audioLocalBits = player->GetLocalData().GetAudioParams().localBits;
		state.audioSignature = BuildAudioSignature(player);
		state.audioEventTick = 0;
		if (slot < previous.maxClients && previous.players[slot].valid)
		{
			state.audioEventTick = previous.players[slot].audioEventTick;
			if (previous.players[slot].audioSignature != state.audioSignature)
				state.audioEventTick = server->GetTick();
		}
		else if (state.audioSignature != 0u)
		{
			state.audioEventTick = server->GetTick();
		}
		if (state.audioLocalBits != 0)
			state.audioEventTick = server->GetTick();
		state.origin = player->GetAbsOrigin();
		state.eye = state.origin;
		state.eye.x += player->GetViewOffset().x;
		state.eye.y += player->GetViewOffset().y;
		state.eye.z += player->GetViewOffset().z;
		state.velocity = player->GetAbsVelocity();
		const CCollisionProperty* const collision = GetCollisionProperty(player);
		if (collision)
		{
			state.mins = collision->OBBMins();
			state.maxs = collision->OBBMaxs();
		}
		state.latencySeconds = GetClientLatencySeconds(client);
		CaptureRelatedEntities(state, player);
		++snapshot.playerCount;
	}

	snapshot.frameTick = server->GetTick();
	SubmitAsyncSnapshot(snapshot);
	TryPublishAsyncResultLocked();
}

void CapturePlayers(CServer* const server)
{
	AUTO_LOCK(g_fowCaptureMutex);
	CapturePlayersLocked(server);
}

bool ShouldRunForClient(const unsigned int clientSlot)
{
	return clientSlot < FOW_MAX_CLIENTS;
}

bool ClearTransmitBit(unsigned char* const state, const unsigned int clientSlot, const int edictIndex)
{
	if (!state || clientSlot >= FOW_MAX_CLIENTS || edictIndex <= 0 || edictIndex >= FOW_MAX_EDICTS)
		return false;

	unsigned char* const record = state + FOW_SNAPSHOT_RECORD_BASE + (static_cast<size_t>(edictIndex) * FOW_SNAPSHOT_RECORD_SIZE);
	uint64_t* const word = reinterpret_cast<uint64_t*>(record + (sizeof(uint64_t) * (clientSlot >> 6)));
	const uint64_t bit = 1ull << (clientSlot & 0x3F);
	const bool wasSet = (*word & bit) != 0;

	*word &= ~bit;
	return wasSet;
}

bool ShouldPreserveRelatedEdict(const int edict)
{
	return false;
}

bool HasActiveAudioReveal(const FOWPublishedSnapshot& snapshot, const FOWPlayerState& target)
{
	if (!fow_audio_reveal.GetBool())
		return false;

	if (target.audioLocalBits != 0)
		return true;

	return target.audioEventTick > 0 && (snapshot.frameTick - target.audioEventTick) <= GetAudioRevealHoldTicks(snapshot);
}

bool ShouldHidePlayer(const FOWPublishedSnapshot& snapshot, const unsigned int observerSlot, const int targetSlot, int* const targetEdict, const char** const reason)
{
	if (observerSlot >= FOW_MAX_CLIENTS || targetSlot < 0 || targetSlot >= snapshot.maxClients)
	{
		*reason = "bad_slot";
		return false;
	}

	if (!IsCullablePair(snapshot, static_cast<int>(observerSlot), targetSlot, reason))
	{
		g_fowLastHidden[observerSlot][targetSlot] = false;
		return false;
	}

	const bool rawVisible = snapshot.visible[observerSlot][targetSlot];
	const bool audioReveal = HasActiveAudioReveal(snapshot, snapshot.players[targetSlot]);
	const int revealHoldTicks = GetRevealHoldTicks(snapshot);
	if (rawVisible || audioReveal)
	{
		g_fowRevealUntilTick[observerSlot][targetSlot] = snapshot.frameTick + revealHoldTicks;
		if (fow_debug.GetBool() && g_fowLastHidden[observerSlot][targetSlot])
		{
			Msg(eDLL_T::SERVER,
				"FOW transition: tick=%d observer=%u target=%d hidden=0 held=0 reason=%s\n",
				snapshot.frameTick,
				observerSlot,
				targetSlot,
				audioReveal ? "audio" : "visible");
		}
		g_fowLastHidden[observerSlot][targetSlot] = false;
		*reason = audioReveal ? "audio" : "visible";
		return false;
	}

	const bool heldVisible = snapshot.frameTick <= g_fowRevealUntilTick[observerSlot][targetSlot];
	if (heldVisible)
	{
		if (fow_debug.GetBool() && g_fowLastHidden[observerSlot][targetSlot])
		{
			Msg(eDLL_T::SERVER,
				"FOW transition: tick=%d observer=%u target=%d hidden=0 held=1\n",
				snapshot.frameTick,
				observerSlot,
				targetSlot);
		}
		g_fowLastHidden[observerSlot][targetSlot] = false;
		*reason = "hold";
		return false;
	}

	*targetEdict = snapshot.players[targetSlot].edict;
	const bool hide = *targetEdict > 0;
	if (hide && fow_debug.GetBool() && !g_fowLastHidden[observerSlot][targetSlot])
	{
		Msg(eDLL_T::SERVER,
			"FOW transition: tick=%d observer=%u target=%d hidden=1 held=0\n",
			snapshot.frameTick,
			observerSlot,
			targetSlot);
	}
	g_fowLastHidden[observerSlot][targetSlot] = hide;
	return hide;
}

FOWFilterStats ApplyVisibility(unsigned char* const state, const unsigned int clientSlot, const bool hideTargetEdict)
{
	const FOWPublishedSnapshot& snapshot = GetPublishedSnapshot();
	FOWFilterStats stats = {};
	stats.lastTargetSlot = -1;
	stats.lastTargetEdict = -1;
	stats.reason = "none";

	for (int targetSlot = 0; targetSlot < snapshot.maxClients; ++targetSlot)
	{
		int targetEdict = -1;
		const char* reason = "visible";
		if (!ShouldHidePlayer(snapshot, clientSlot, targetSlot, &targetEdict, &reason))
			continue;

		++stats.considered;
		stats.lastTargetSlot = targetSlot;
		stats.lastTargetEdict = targetEdict;
		stats.reason = reason;

		if (hideTargetEdict && ClearTransmitBit(state, clientSlot, targetEdict))
			++stats.hidden;

		const FOWPlayerState& target = snapshot.players[targetSlot];
		for (int i = 0; i < target.relatedEdictCount; ++i)
		{
			if (ShouldPreserveRelatedEdict(target.relatedEdicts[i]))
				continue;

			++stats.relatedConsidered;
			if (ClearTransmitBit(state, clientSlot, target.relatedEdicts[i]))
				++stats.relatedHidden;
		}
	}

	return stats;
}

bool ShouldLogSnapshot(const FOWFilterStats& stats)
{
	if (!fow_debug.GetBool())
		return false;

	if (fow_debug_snapshots.GetBool())
		return true;

	++g_fowLogCount;
	if (g_fowLogCount <= 8 || (g_fowLogCount % 64) == 0)
		return true;

	return stats.hidden > 0 || (stats.considered > 0 && stats.hidden == 0);
}

void RefreshForSnapshot(CServer* const server)
{
	if (!server || !server->IsActive())
		return;

	const int serverTick = server->GetTick();
	AUTO_LOCK(g_fowCaptureMutex);
	TryPublishAsyncResultLocked();
	if (GetPublishedSnapshot().frameTick != serverTick)
		CapturePlayersLocked(server);
}

unsigned int ParseMaskArg(const char* const value, const unsigned int fallback)
{
	if (!value || !*value)
		return fallback;

	char* end = nullptr;
	const unsigned long parsed = strtoul(value, &end, 0);
	if (end == value)
		return fallback;

	return static_cast<unsigned int>(parsed);
}

void OnServerFrame(CServer* const server)
{
	g_lastServer = server;
	if (!fow_enable.GetBool())
		return;

	CapturePlayers(server);
}

void FilterSnapshot(CServer* const server, const unsigned int clientSlot, const bool isDelta, const char* const source, void* const frame, unsigned char* const state)
{
	g_lastServer = server;
	if (!fow_enable.GetBool() || !frame || !state || !ShouldRunForClient(clientSlot))
		return;

	RefreshForSnapshot(server);

	const bool hideTargetEdict = isDelta;
	const FOWFilterStats stats = ApplyVisibility(state, clientSlot, hideTargetEdict);
	const FOWPublishedSnapshot& snapshot = GetPublishedSnapshot();

	if (ShouldLogSnapshot(stats))
	{
		Msg(eDLL_T::SERVER,
			"FOW %s: client=%u tick=%d players=%d frame=%p state=%p considered=%d hidden=%d related=%d relatedHidden=%d targetSlot=%d targetEdict=%d reason=%s\n",
			source ? source : "snapshot",
			clientSlot,
			snapshot.frameTick,
			snapshot.playerCount,
			frame,
			state,
			stats.considered,
			stats.hidden,
			stats.relatedConsidered,
			stats.relatedHidden,
			stats.lastTargetSlot,
			stats.lastTargetEdict,
			stats.reason ? stats.reason : "unknown");
	}
}

char BuildDeltaSnapshot(void* thisptr, int clientSlot, void* baselineFrame, void* currentFrame, void* outFrame)
{
	SnapshotFrame* const frame = reinterpret_cast<SnapshotFrame*>(currentFrame);
	if (frame)
		FilterSnapshot(g_lastServer, static_cast<unsigned int>(clientSlot), true, "delta", currentFrame, frame->state);

	return ServerGameEnts_BuildDeltaSnapshot(thisptr, clientSlot, baselineFrame, currentFrame, outFrame);
}

__int64 BuildFullSnapshot(void* thisptr, unsigned int clientSlot, void* currentFrame, void* outFrame)
{
	SnapshotFrame* const frame = reinterpret_cast<SnapshotFrame*>(currentFrame);
	if (frame)
		FilterSnapshot(g_lastServer, clientSlot, false, "full", currentFrame, frame->state);

	return ServerGameEnts_BuildFullSnapshot(thisptr, clientSlot, currentFrame, outFrame);
}

bool SetSnapshotDetours(const bool attach)
{
	if (attach)
		g_snapshotDetour.GetFun();

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	g_snapshotDetour.Detour(attach);
	const LONG result = DetourTransactionCommit();
	if (result != NO_ERROR)
		return false;

	g_snapshotDetoursInstalled = attach;
	return true;
}

static void Status_f(const CCommand& args)
{
	const FOWPublishedSnapshot& snapshot = GetPublishedSnapshot();
	const FOWWorkerStats workerStats = WorkerGetStats();
	Msg(eDLL_T::SERVER,
		"FOW: enable=%d worker_started=%d worker_pending=%d worker_ready=%d submits=%llu completed=%llu consumed=%llu debug=%d debug_snapshots=%d tick=%d players=%d trace_mask=0x%08X reveal_radius=%.1f peek=%d forward=%.1f side=%.1f up=%.1f owner_visual_class=%s\n",
		fow_enable.GetInt(),
		workerStats.started ? 1 : 0,
		workerStats.pending ? 1 : 0,
		workerStats.ready ? 1 : 0,
		workerStats.submitted,
		workerStats.completed,
		workerStats.consumed,
		fow_debug.GetInt(),
		fow_debug_snapshots.GetInt(),
		snapshot.frameTick,
		snapshot.playerCount,
		FOW_TRACE_MASK,
		fow_reveal_radius.GetFloat(),
		fow_peek_enable.GetInt(),
		fow_peek_forward.GetFloat(),
		fow_peek_side.GetFloat(),
		fow_peek_up.GetFloat(),
		FOW_OWNER_VISUAL_CLASS_FILTER);
}

static ConCommand fow_status("fow_status", Status_f, "Prints fog-of-war runtime state.", FCVAR_RELEASE);

static void ListPlayers_f(const CCommand& args)
{
	if (g_lastServer)
		CapturePlayers(g_lastServer);

	const FOWPublishedSnapshot& snapshot = GetPublishedSnapshot();
	Msg(eDLL_T::SERVER, "FOW players: tick=%d count=%d maxClients=%d\n", snapshot.frameTick, snapshot.playerCount, snapshot.maxClients);

	for (int slot = 0; slot < snapshot.maxClients; ++slot)
	{
		const FOWPlayerState& player = snapshot.players[slot];
		if (!player.valid)
			continue;

		Msg(eDLL_T::SERVER,
			"  slot=%d edict=%d team=%d alive=%d fake=%d related=%d latency=%.3f velocity=(%.1f %.1f %.1f) origin=(%.1f %.1f %.1f) eye=(%.1f %.1f %.1f)\n",
			player.slot,
			player.edict,
			player.team,
			player.alive ? 1 : 0,
			player.fake ? 1 : 0,
			player.relatedEdictCount,
			player.latencySeconds,
			player.velocity.x,
			player.velocity.y,
			player.velocity.z,
			player.origin.x,
			player.origin.y,
			player.origin.z,
			player.eye.x,
			player.eye.y,
			player.eye.z);
	}
}

static ConCommand fow_list_players("fow_list_players", ListPlayers_f, "Lists captured player state for fog-of-war testing.", FCVAR_RELEASE);

static void DumpRelated_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::SERVER, "usage: fow_dump_related <slot>\n");
		return;
	}

	if (g_lastServer)
		CapturePlayers(g_lastServer);

	const FOWPublishedSnapshot& snapshot = GetPublishedSnapshot();
	const int slot = atoi(args.Arg(1));
	if (slot < 0 || slot >= snapshot.maxClients)
	{
		Msg(eDLL_T::SERVER, "FOW related: invalid slot=%d\n", slot);
		return;
	}

	const FOWPlayerState& player = snapshot.players[slot];
	if (!player.valid)
	{
		Msg(eDLL_T::SERVER, "FOW related: slot=%d is not valid in current snapshot\n", slot);
		return;
	}

	Msg(eDLL_T::SERVER,
		"FOW related: slot=%d edict=%d team=%d alive=%d related=%d\n",
		player.slot,
		player.edict,
		player.team,
		player.alive ? 1 : 0,
		player.relatedEdictCount);

	for (int i = 0; i < player.relatedEdictCount; ++i)
	{
		const int edict = player.relatedEdicts[i];
		CBaseEntity* const entity = static_cast<CBaseEntity*>(g_entityList ? g_entityList->LookupEntityByNetworkIndex(edict) : nullptr);
		const int ownerEdict = entity ? HandleToEdict(entity->GetOwnerEntityHandle()) : -1;
		const int moveParentEdict = entity ? HandleToEdict(entity->GetMoveParentHandle()) : -1;
		Msg(eDLL_T::SERVER,
			"  tracked edict=%d class=%s model=%s owner=%d parent=%d\n",
			edict,
			GetEntityClassName(edict),
			GetEntityModelName(entity),
			ownerEdict,
			moveParentEdict);
	}
}

static ConCommand fow_dump_related("fow_dump_related", DumpRelated_f, "Dumps tracked entities for one player slot.", FCVAR_RELEASE);

static void DumpMatrix_f(const CCommand& args)
{
	if (g_lastServer)
		CapturePlayers(g_lastServer);

	const FOWPublishedSnapshot& snapshot = GetPublishedSnapshot();
	Msg(eDLL_T::SERVER, "FOW matrix: tick=%d players=%d\n", snapshot.frameTick, snapshot.playerCount);
	for (int observer = 0; observer < snapshot.maxClients; ++observer)
	{
		if (!snapshot.players[observer].valid)
			continue;

		for (int target = 0; target < snapshot.maxClients; ++target)
		{
			if (!snapshot.players[target].valid || snapshot.visible[observer][target])
				continue;

			Msg(eDLL_T::SERVER,
				"  hide observerSlot=%d observerEdict=%d targetSlot=%d targetEdict=%d\n",
				observer,
				snapshot.players[observer].edict,
				target,
				snapshot.players[target].edict);
		}
	}
}

static ConCommand fow_dump_matrix("fow_dump_matrix", DumpMatrix_f, "Prints hidden observer/target pairs in the fog-of-war visibility matrix.", FCVAR_RELEASE);

static void TracePair_f(const CCommand& args)
{
	if (args.ArgC() < 3)
	{
		Msg(eDLL_T::SERVER, "usage: fow_trace_pair <observerSlot> <targetSlot> [mask]\n");
		Msg(eDLL_T::SERVER, "FOW trace masks: visible=%u npcworldstatic=%u blocklos=%u shot_brushonly=%u\n",
			TRACE_MASK_VISIBLE,
			TRACE_MASK_NPCWORLDSTATIC,
			TRACE_MASK_BLOCKLOS,
			TRACE_MASK_SHOT_BRUSHONLY);
		return;
	}

	if (g_lastServer)
		CapturePlayers(g_lastServer);

	const FOWPublishedSnapshot& snapshot = GetPublishedSnapshot();
	const int observerSlot = atoi(args.Arg(1));
	const int targetSlot = atoi(args.Arg(2));
	const unsigned int mask = args.ArgC() >= 4 ? ParseMaskArg(args.Arg(3), FOW_TRACE_MASK) : FOW_TRACE_MASK;

	const char* pairReason = nullptr;
	if (!IsCullablePair(snapshot, observerSlot, targetSlot, &pairReason))
	{
		Msg(eDLL_T::SERVER, "FOW trace: invalid pair observer=%d target=%d reason=%s\n", observerSlot, targetSlot, pairReason ? pairReason : "unknown");
		return;
	}

	trace_t eyeTrace;
	const FOWPlayerState& observer = snapshot.players[observerSlot];
	const FOWPlayerState& target = snapshot.players[targetSlot];
	if (!TraceSegment(observer.eye, target.eye, mask, &eyeTrace))
	{
		Msg(eDLL_T::SERVER, "FOW trace: TraceRay unavailable g_engineTraceServer=%p\n", g_engineTraceServer);
		return;
	}

	const bool clear = IsTraceClear(eyeTrace);
	const bool transparent = IsTransparentTraceHit(eyeTrace);
	DrawTraceDebug(observer.eye, target.eye, eyeTrace, clear);

	Msg(eDLL_T::SERVER,
		"FOW trace: observer=%d target=%d mask=0x%08X clear=%d transparent=%d fraction=%.4f startsolid=%d allsolid=%d contents=0x%08X surfProp=%u surfFlags=0x%04X surfacePtr=%p hitEnt=%p start=(%.1f %.1f %.1f) end=(%.1f %.1f %.1f) hit=(%.1f %.1f %.1f)\n",
		observerSlot,
		targetSlot,
		mask,
		clear ? 1 : 0,
		transparent ? 1 : 0,
		eyeTrace.fraction,
		eyeTrace.startsolid ? 1 : 0,
		eyeTrace.allsolid ? 1 : 0,
		eyeTrace.contents,
		eyeTrace.surface.surfaceProp,
		eyeTrace.surface.flags,
		eyeTrace.surface.name,
		eyeTrace.hit_entity,
		observer.eye.x,
		observer.eye.y,
		observer.eye.z,
		target.eye.x,
		target.eye.y,
		target.eye.z,
		eyeTrace.endpos.x,
		eyeTrace.endpos.y,
		eyeTrace.endpos.z);
}

static ConCommand fow_trace_pair("fow_trace_pair", TracePair_f, "Traces map visibility between two captured player slots.", FCVAR_RELEASE);

void InstallCallback(const PluginOperation_s::PluginCallback_e callbackId, void* const function, const char* const name)
{
	PluginOperation_s pio = {};
	pio.callbackId = callbackId;
	pio.commandId = PluginOperation_s::PluginCommand_e::PLUGIN_INSTALL_CALLBACK;
	pio.name = name;
	pio.function = function;
	g_pluginSystem->RunOperation(&pio);
}

void RemoveCallback(const PluginOperation_s::PluginCallback_e callbackId, void* const function, const char* const name)
{
	PluginOperation_s pio = {};
	pio.callbackId = callbackId;
	pio.commandId = PluginOperation_s::PluginCommand_e::PLUGIN_REMOVE_CALLBACK;
	pio.name = name;
	pio.function = function;
	g_pluginSystem->RunOperation(&pio);
}
}

bool FOWPlugin_OnLoad(const char* pszSelfModule, const char* pszSDKModule)
{
	g_selfModule.InitFromName(pszSelfModule);
	g_sdkModule.InitFromName(pszSDKModule);
	g_gameModule.InitFromBase(CModule::GetProcessEnvironmentBlock()->ImageBaseAddress);

	InstantiateInterfaceFn factorySystem = g_sdkModule.GetExportedSymbol("GetFactorySystem").RCast<InstantiateInterfaceFn>();
	if (!factorySystem)
		return false;

	g_factorySystem = reinterpret_cast<IFactorySystem*>(factorySystem());
	if (!g_factorySystem)
		return false;

	g_pluginSystem = reinterpret_cast<IPluginSystem*>(g_factorySystem->GetFactory(INTERFACEVERSION_PLUGINSYSTEM));
	if (!g_pluginSystem)
		return false;

	g_CoreMsgVCallback = &PluginLoggerSink;
	g_pCVar = reinterpret_cast<CCvar*>(g_factorySystem->GetFactory(CVAR_INTERFACE_VERSION));
	if (!g_pCVar)
		return false;

	ConVar_Register();
	ResolveRuntimePointers();

	InstallCallback(PluginOperation_s::PluginCallback_e::CServer_RunFrame, reinterpret_cast<void*>(&OnServerFrame), "FOW_OnServerFrame");
	if (!SetSnapshotDetours(true))
	{
		RemoveCallback(PluginOperation_s::PluginCallback_e::CServer_RunFrame, reinterpret_cast<void*>(&OnServerFrame), "FOW_OnServerFrame");
		ConVar_Unregister();
		g_pluginSystem = nullptr;
		g_factorySystem = nullptr;
		return false;
	}

	if (!SetDisconnectVScriptDetours(true))
	{
		SetSnapshotDetours(false);
		RemoveCallback(PluginOperation_s::PluginCallback_e::CServer_RunFrame, reinterpret_cast<void*>(&OnServerFrame), "FOW_OnServerFrame");
		ConVar_Unregister();
		g_pluginSystem = nullptr;
		g_factorySystem = nullptr;
		return false;
	}

	return true;
}

bool FOWPlugin_OnUnload()
{
	if (!g_pluginSystem)
		return false;

	if (g_disconnectVScriptDetoursInstalled)
		SetDisconnectVScriptDetours(false);

	if (g_snapshotDetoursInstalled)
		SetSnapshotDetours(false);

	RemoveCallback(PluginOperation_s::PluginCallback_e::CServer_RunFrame, reinterpret_cast<void*>(&OnServerFrame), "FOW_OnServerFrame");
	WorkerStop();
	ConVar_Unregister();
	ClearPendingRealDisconnects();
	g_pluginSystem = nullptr;
	g_factorySystem = nullptr;
	g_entityList = nullptr;
	g_fowClientEntityList = nullptr;
	g_engineTraceServer = nullptr;
	g_debugOverlay = nullptr;
	g_lastServer = nullptr;
	return true;
}