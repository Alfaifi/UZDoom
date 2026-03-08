/*
** d_net.cpp
**
** DOOM Network game communication and protocol, all OS independent parts.
**
**---------------------------------------------------------------------------
**
** Copyright 1993-1996 id Software
** Copyright 1999-2016 Marisa Heit
** Copyright 2002-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include <stddef.h>
#include <stdio.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "a_keys.h"
#include "a_sharedglobal.h"
#include "actorinlines.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "d_eventbase.h"
#include "d_main.h"
#include "d_net.h"
#include "doomstat.h"
#include "d_netinf.h"
#include "events.h"
#include "g_game.h"
#include "g_levellocals.h"
#include "g_mapinfo.h"
#include "gameconfigfile.h"
#include "gi.h"
#include "gstrings.h"
#include "i_interface.h"
#include "i_net.h"
#include "i_system.h"
#include "i_time.h"
#include "m_argv.h"
#include "m_cheat.h"
#include "menu.h"
#include "p_conversation.h"
#include "p_enemy.h"
#include "p_lnspec.h"
#include "p_local.h"
#include "p_spec.h"
#include "p_trace.h"
#include "r_utility.h"
#include "s_music.h"
#include "savegamemanager.h"
#include "sbar.h"
#include "screenjob.h"
#include "serializer.h"
#include "version.h"
#include "vm.h"

void P_RunClientSideLogic();

EXTERN_CVAR (Int, disableautosave)
EXTERN_CVAR (Int, autosavecount)
EXTERN_CVAR (Bool, cl_capfps)
EXTERN_CVAR (Bool, vid_vsync)
EXTERN_CVAR (Int, vid_maxfps)
EXTERN_CVAR (Bool, cl_noprediction)
EXTERN_CVAR (String, net_password)

EXTERN_FARG(loadgame);

FARG(extratic, "Multiplayer", "Sends backup commands over the network", "",
	"Causes " GAMENAME " to send a backup copy of every movement command across the network.");

FVerificationError Net_VerifyEngine(uint8_t*& stream, size_t& offset);

extern uint8_t		*demo_p;		// [RH] Special "ticcmds" get recorded in demos
extern FString	savedescription;
extern FString	savegamefile;

extern bool AppActive;

void P_ClearLevelInterpolation();
static bool IsMapLoaded();

// Big-endian read/write helpers for network packet serialization.
static inline void WriteBE32(uint8_t* p, uint32_t v)
{
	p[0] = (v >> 24) & 0xFF;
	p[1] = (v >> 16) & 0xFF;
	p[2] = (v >> 8) & 0xFF;
	p[3] = v & 0xFF;
}

static inline void WriteBE16(uint8_t* p, uint16_t v)
{
	p[0] = (v >> 8) & 0xFF;
	p[1] = v & 0xFF;
}

static inline uint32_t ReadBE32(const uint8_t* p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t ReadBE16(const uint8_t* p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

enum ELevelStartStatus
{
	LST_READY,
	LST_HOST,
	LST_WAITING,
};

enum EReadyType
{
	RT_VOTE,
	RT_ANYONE,
	RT_HOST_ONLY,
};

enum ELagType
{
	LAG_NONE,
	LAG_PREDICTING,
	LAG_WAITING,
	LAG_SKIPPING,
};

// NETWORKING
//
// gametic is the tic about to (or currently being) run.
// ClientTic is the tick the client is currently on and building a command for.
//
// A world tick cannot be ran until CurrentSequence >= gametic for all clients.

int 				ClientTic = 0;
usercmd_t			LocalCmds[LOCALCMDTICS] = {};
int					LastSentConsistency = 0;		// Last consistency we sent out. If < CurrentConsistency, send them out.
int					CurrentConsistency = 0;			// Last consistency we generated.
FClientNetState		ClientStates[MAXPLAYERS] = {};

// Try and stabilize uneven connections by checking for spikes in available
// sequences. If they're found, try and average out a buffer to prioritize
// making the experience smoother over very stop and go heavy.
static int			StabilityBuffer = 0;
static int			PrevAvailableDiff = 0;
static size_t		CurStabilityTic = 0u;
static int			StabilityTics[STABILITYTICS] = {};

// If we're sending a packet to ourselves, store it here instead. This is the simplest way to execute
// playback as it means in the world running code itself all player commands are built the exact same way
// instead of having to rely on pulling from the correct local buffers. It also ensures all commands are
// executed over the net at the exact same tick.
static size_t	LocalNetBufferSize = 0;
static uint8_t	LocalNetBuffer[MAX_MSGLEN] = {};

static uint8_t	CurrentLobbyID = 0u;	// Ignore commands not from this lobby (useful when transitioning levels).
static int		LastGameUpdate = 0;		// Track the last time the game actually ran the world.
static uint64_t	MutedClients = 0u;		// Ignore messages from these clients.

static int CutsceneCountdown = 0;	// If enough people are ready, count down the timer. This won't reset between unreadies, only on intermission entrance.
static uint64_t CutsceneReady = 0u; // If in a cutscene, check if we're ready to move to move past it.

static int  LevelStartDebug = 0;
static int	LevelStartDelay = 0; // While this is > 0, don't start generating packets yet.
static ELevelStartStatus LevelStartStatus = LST_READY; // Listen for when to actually start making tics.
static uint64_t	LevelStartAck = 0u; // Used by the host to determine if everyone has loaded in.

static int FullLatencyCycle = MAXSENDTICS * 3;	// Give ~3 seconds to gather latency info about clients on boot up.
static int LastLatencyUpdate = 0;				// Update average latency every ~1 second.

static ELagType	LagState = LAG_NONE;	// What kind of lag the game is currently getting.
static int 	EnterTic = 0;
static int	LastEnterTic = 0;
static bool bCommandsReset = false;		// If true, commands were recently cleared. Don't generate any more tics.

static int	CommandsAhead = 0;		// If too far ahead of the host, slow down to remove built-up latency.
static int	SkipCommandTimer = 0;	// Tracker for when to check for skipping commands. ~0.5 seconds in a row of being ahead will start skipping.
static int	SkipCommandAmount = 0;	// Amount of commands to skip. Try and batch skip them all at once since we won't be able to get an update until the full RTT.

// Disconnect handshake state.
static bool		bDisconnecting = false;		// Are we in the process of a graceful disconnect?
static uint64_t	DisconnectTimestamp = 0;		// When we last sent a disconnect request/notify.
static int		DisconnectRetries = 0;		// How many times we've retried the disconnect.
static uint64_t	DisconnectConfirmMask = 0;	// Bitmask of clients that have confirmed our disconnect notification (host only).
static bool		bMigrating = false;			// Is host migration in progress?
static FMigrationState PendingMigrationState;	// State received from departing host during migration.
static bool		bHasPendingMigration = false;	// True if we received migration state and are becoming the new host.

constexpr int		DISCONNECT_TIMEOUT_MS = 500;	// Retry disconnect request/notify after this many ms.
constexpr int		DISCONNECT_MAX_RETRIES = 8;		// Give up after this many retries (4 seconds total).
constexpr uint64_t	CLIENT_TIMEOUT_MS = 10000;		// Treat client as disconnected after 10 seconds of silence.

// Client-side: set when the host is a dedicated server (slot 0 is ghost).
bool				hostIsDedicated = false;

void D_ProcessEvents(void); 
void G_BuildTiccmd(usercmd_t *cmd);
void D_DoAdvanceDemo(void);

static void RunScript(TArrayView<uint8_t>& stream, AActor *pawn, int snum, int argn, int always);

extern	bool	 advancedemo;

CVAR(Bool, vid_dontdowait, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR(Bool, vid_lowerinbackground, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)

CVAR(Bool, net_ticbalance, true, CVAR_SERVERINFO | CVAR_NOSAVE)
CVAR(Bool, net_extratic, false, CVAR_SERVERINFO | CVAR_NOSAVE)
CVAR(Bool, net_limitsaves, true, CVAR_SERVERINFO | CVAR_NOSAVE)
CVAR(Bool, net_repeatableactioncooldown, true, CVAR_SERVERINFO | CVAR_NOSAVE)
CVAR(Bool, net_limitconversations, false, CVAR_SERVERINFO | CVAR_NOSAVE)
CUSTOM_CVAR(Int, net_disablepause, 0, CVAR_SERVERINFO | CVAR_NOSAVE)
{
	if (self < 0)
		self = 0;
	else if (self > 2)
		self = 2;
}
CUSTOM_CVAR(Int, net_cutscenereadytype, RT_VOTE, CVAR_SERVERINFO | CVAR_NOSAVE)
{
	if (self < RT_VOTE)
		self = RT_VOTE;
	else if (self > RT_HOST_ONLY)
		self = RT_HOST_ONLY;
}
CUSTOM_CVAR(Float, net_cutscenereadypercent, 0.5f, CVAR_SERVERINFO | CVAR_NOSAVE)
{
	if (self < 0.0f)
		self = 0.0f;
	else if (self > 1.0f)
		self = 1.0f;
}
CVAR(Float, net_cutscenecountdown, 30.0f, CVAR_SERVERINFO | CVAR_NOSAVE)

CVAR(Bool, cl_noboldchat, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, cl_nochatsound, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, cl_showchat, CHAT_GLOBAL, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < CHAT_DISABLED)
		self = CHAT_DISABLED;
	else if (self > CHAT_GLOBAL)
		self = CHAT_GLOBAL;
}

CUSTOM_CVAR(Int, cl_debugprediction, 0, CVAR_CHEAT)
{
	if (self < 0)
		self = 0;
	else if (self > BACKUPTICS - 1)
		self = BACKUPTICS - 1;
}
CVAR(Bool, net_dedicatedhibernate, true, CVAR_SERVERINFO | CVAR_NOSAVE)

bool Net_IsGhostPlayer(int player)
{
	return (dedicatedServer || hostIsDedicated) && player == 0;
}

static bool IsDedicatedControlStream(int player)
{
	return Net_IsGhostPlayer(player);
}

static bool IsRealGameplayPlayer(int player)
{
	return player >= 0 && player < (int)MAXPLAYERS && playeringame[player] && !Net_IsGhostPlayer(player);
}

static int CountRealGameplayPlayers()
{
	int total = 0;
	for (int i = 0; i < (int)MAXPLAYERS; ++i)
	{
		if (IsRealGameplayPlayer(i))
			++total;
	}

	return total;
}

static int CountNonGhostNetworkClients(bool includeQuitters)
{
	int total = 0;
	for (auto client : NetworkClients)
	{
		if (Net_IsGhostPlayer(client))
			continue;
		if (!includeQuitters && (ClientStates[client].Flags & CF_QUIT))
			continue;
		++total;
	}

	return total;
}

static bool HasNonGhostNetworkClient()
{
	return CountNonGhostNetworkClients(false) > 0;
}

static bool ShouldHibernateDedicatedServer()
{
	return dedicatedServer
		&& net_dedicatedhibernate
		&& !demoplayback
		&& gamestate == GS_LEVEL
		&& gameaction == ga_nothing
		&& LevelStartStatus == LST_READY
		&& IsMapLoaded()
		&& !HasNonGhostNetworkClient()
		&& CountRealGameplayPlayers() == 0;
}

static void ResetNetLagHistories();

// Used to write out all network events that occured leading up to the next tick.
static struct NetEventData
{
	struct FStream {
		uint8_t* Stream;
		size_t Used = 0;

		FStream()
		{
			Grow(256);
		}

		~FStream()
		{
			if (Stream != nullptr)
				M_Free(Stream);
		}

		void Grow(size_t size)
		{
			Stream = (uint8_t*)M_Realloc(Stream, size);
		}
	} Streams[BACKUPTICS];

private:
	size_t CurrentSize = 0;
	size_t MaxSize = 256;
	int CurrentClientTic = 0;

	// Make more room for special Command.
	void GetMoreBytes(size_t newSize)
	{
		MaxSize = max<size_t>(MaxSize * 2, newSize + 30);

		DPrintf(DMSG_NOTIFY, "Expanding special size to %zu\n", MaxSize);

		for (auto& stream : Streams)
			stream.Grow(MaxSize);

		CurrentStream = Streams[CurrentClientTic % BACKUPTICS].Stream + CurrentSize;
	}

	void AddBytes(size_t bytes)
	{
		if (CurrentSize + bytes >= MaxSize)
			GetMoreBytes(CurrentSize + bytes);

		CurrentSize += bytes;
	}

public:
	uint8_t* CurrentStream = nullptr;

	// Boot up does some faux network events so we need to wait until after
	// everything is initialized to actually set up the network stream.
	void InitializeEventData()
	{
		CurrentStream = Streams[0].Stream;
		CurrentSize = 0;
	}

	void ResetStream()
	{
		CurrentClientTic = ClientTic / TicDup;
		CurrentStream = Streams[CurrentClientTic % BACKUPTICS].Stream;
		CurrentSize = 0;
	}

	void NewClientTic()
	{
		const int tic = ClientTic / TicDup;
		if (CurrentClientTic == tic)
			return;

		Streams[CurrentClientTic % BACKUPTICS].Used = CurrentSize;
		
		CurrentClientTic = tic;
		CurrentStream = Streams[tic % BACKUPTICS].Stream;
		CurrentSize = 0;
	}

	NetEventData& operator<<(uint8_t it)
	{
		if (CurrentStream != nullptr)
		{
			AddBytes(1);
			UncheckedWriteInt8(it, &CurrentStream);
		}
		return *this;
	}

	NetEventData& operator<<(int16_t it)
	{
		if (CurrentStream != nullptr)
		{
			AddBytes(2);
			UncheckedWriteInt16(it, &CurrentStream);
		}
		return *this;
	}

	NetEventData& operator<<(int32_t it)
	{
		if (CurrentStream != nullptr)
		{
			AddBytes(4);
			UncheckedWriteInt32(it, &CurrentStream);
		}
		return *this;
	}

	NetEventData& operator<<(int64_t it)
	{
		if (CurrentStream != nullptr)
		{
			AddBytes(8);
			UncheckedWriteInt64(it, &CurrentStream);
		}
		return *this;
	}

	NetEventData& operator<<(float it)
	{
		if (CurrentStream != nullptr)
		{
			AddBytes(4);
			UncheckedWriteFloat(it, &CurrentStream);
		}
		return *this;
	}

	NetEventData& operator<<(double it)
	{
		if (CurrentStream != nullptr)
		{
			AddBytes(8);
			UncheckedWriteDouble(it, &CurrentStream);
		}
		return *this;
	}

	NetEventData& operator<<(const char *it)
	{
		if (CurrentStream != nullptr)
		{
			AddBytes(strlen(it) + 1);
			UncheckedWriteString(it, &CurrentStream);
		}
		return *this;
	}
} NetEvents;

void P_ClearPredictionData();

void Net_ClearBuffers()
{
	CloseNetwork();

	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		playeringame[i] = false;
		players[i].waiting = players[i].inconsistant = false;

		auto& state = ClientStates[i];
		state.AverageLatency = state.CurrentLatency = 0u;
		memset(state.SentTime, 0, sizeof(state.SentTime));
		memset(state.RecvTime, 0, sizeof(state.RecvTime));
		state.bNewLatency = true;

		state.ResendID = state.StabilityBuffer = 0u;
		state.CurrentNetConsistency = state.LastVerifiedConsistency = state.ConsistencyAck = state.ResendConsistencyFrom = -1;
		state.CurrentSequence = state.SequenceAck = state.ResendSequenceFrom = -1;
		state.Flags = 0;

		for (int j = 0; j < BACKUPTICS; ++j)
			state.Tics[j].Data.SetData(nullptr, 0);
	}

	P_ClearPredictionData();
	NetBufferLength = 0u;
	RemoteClient = -1;
	MaxClients = TicDup = 1u;
	consoleplayer = 0;
	LocalNetBufferSize = 0u;
	Net_Arbitrator = 0;

	LagState = LAG_NONE;
	MutedClients = 0u;
	CurrentLobbyID = 0u;
	hostIsDedicated = false;
	NetworkClients.Clear();
	netgame = multiplayer = false;
	LastSentConsistency = CurrentConsistency = 0;
	LastEnterTic = LastGameUpdate = EnterTic;
	gametic = ClientTic = 0;
	SkipCommandTimer = SkipCommandAmount = CommandsAhead = 0;
	StabilityBuffer = PrevAvailableDiff = 0;
	CurStabilityTic = 0u;
	memset(StabilityTics, 0, sizeof(StabilityTics));
	NetEvents.ResetStream();
	
	CutsceneReady = 0u;
	CutsceneCountdown = 0;
	bCommandsReset = false;

	LevelStartAck = 0u;
	LevelStartDelay = LevelStartDebug = 0;
	LevelStartStatus = LST_READY;

	FullLatencyCycle = MAXSENDTICS * 3;
	LastLatencyUpdate = 0;
	ResetNetLagHistories();

	playeringame[0] = true;
	NetworkClients += 0;
}

bool Net_IsPlayerReady(int player)
{
	if (demoplayback || net_cutscenereadytype != RT_VOTE)
		return false;

	if (cutscene.runner)
	{
		int type = ST_VOTE;
		IFVM(ScreenJobRunner, GetSkipType)
			type = VMCallSingle<int>(func, cutscene.runner);

		if (type == ST_UNSKIPPABLE)
			return false;
	}

	// Ghost player on dedicated servers is always ready (no human to press buttons).
	if (Net_IsGhostPlayer(player))
		return true;

	return players[player].Bot != nullptr || (CutsceneReady & ((uint64_t)1u << player));
}

// Check if every client is ready to move on from the current cutscene.
void Net_PlayerReadiedUp(int player)
{
	if (!netgame || demoplayback)
		return;

	// Allow unreadying in case a player needs to leave momentarily.
	if (Net_IsPlayerReady(player))
		CutsceneReady &= ~((uint64_t)1u << player);
	else
		CutsceneReady |= (uint64_t)1u << player;
}

void Net_StartCutscene()
{
	CutsceneCountdown = netgame && !demoplayback && net_cutscenecountdown > 0.0f ? static_cast<int>(ceil(net_cutscenecountdown * TICRATE)) : 0;
}

// Allow the game to automatically start after a set amount of time.
bool Net_CheckCutsceneReady()
{
	if (!cutscene.runner)
		return false;

	int type = ST_VOTE;
	IFVM(ScreenJobRunner, GetSkipType)
		type = VMCallSingle<int>(func, cutscene.runner);

	if (type == ST_UNSKIPPABLE)
		return false;

	if (net_cutscenereadytype == RT_ANYONE)
		return CutsceneReady != 0;

	if (net_cutscenereadytype == RT_HOST_ONLY)
		return (CutsceneReady & ((uint64_t)1u << Net_Arbitrator));

	uint64_t mask = 0u;
	int totalReady = 0;
	int totalPlayers = 0;
	// Bots will be automatically assumed to be ready, so we don't include them.
	for (auto client : NetworkClients)
	{
		if (Net_IsGhostPlayer(client))
			continue;

		mask |= (uint64_t)1u << client;
		totalReady += Net_IsPlayerReady(client);
		++totalPlayers;
	}

	if (totalPlayers <= 0)
		return false;

	if ((CutsceneReady & mask) == mask)
		return true;

	if ((float)totalReady / totalPlayers < net_cutscenereadypercent)
		return false;

	if (CutsceneCountdown <= 0)
		return true;

	--CutsceneCountdown;
	return false;
}

void Net_AdvanceCutscene()
{
	CutsceneReady = 0u;
	CutsceneCountdown = 0;
	if (consoleplayer == Net_Arbitrator)
		Net_WriteInt8(DEM_ENDSCREENJOB);
}

bool Net_IsWaiting()
{
	return LagState == LAG_WAITING;
}

// This is needed for handling PSprite bobbing specifically since it's predicted.
double Net_ModifyFrac(double ticFrac)
{
	return LagState < LAG_WAITING ? ticFrac : 1.0;
}

double Net_ModifyObjectFrac(DObject* obj, double ticFrac)
{
	return LagState == LAG_NONE || LagState == LAG_SKIPPING || obj->IsClientSide() ? ticFrac : 1.0;
}

double Net_ModifyParticleFrac(particle_t* part, double ticFrac)
{
	return LagState == LAG_NONE || LagState == LAG_SKIPPING ? ticFrac : 0.0;
}

void Net_ResetCommands(bool midTic)
{
	bCommandsReset = midTic;
	++CurrentLobbyID;
	SkipCommandTimer = SkipCommandAmount = CommandsAhead = 0;
	StabilityBuffer = PrevAvailableDiff = 0;
	CurStabilityTic = 0u;
	memset(StabilityTics, 0, sizeof(StabilityTics));

	int tic = gametic / TicDup;
	if (midTic)
	{
		// If we're mid ticdup cycle, make sure we immediately enter the next one after
		// the current tic we're in finishes.
		ClientTic = (tic + 1) * TicDup;
		gametic = (tic * TicDup) + (TicDup - 1);
	}
	else
	{
		ClientTic = gametic = tic * TicDup;
		--tic;
	}
	
	for (auto client : NetworkClients)
	{
		auto& state = ClientStates[client];
		state.Flags &= (CF_QUIT | CF_JOINING | CF_AWAITING_STATE | CF_AWAITING_ACTIVE);
		state.StabilityBuffer = 0u;
		state.CurrentSequence = min<int>(state.CurrentSequence, tic);
		state.SequenceAck = min<int>(state.SequenceAck, tic);
		if (state.ResendSequenceFrom >= tic)
			state.ResendSequenceFrom = -1;
		
		// Make sure not to run its current command either.
		auto& curTic = state.Tics[tic % BACKUPTICS];
		const int running = (curTic.Command.buttons & BT_RUN); // This isn't delta'd so needs to be kept.
		memset(&curTic.Command, 0, sizeof(curTic.Command));
		curTic.Command.buttons |= running;
	}

	NetEvents.ResetStream();
}

void Net_SetWaiting()
{
	if (netgame && !demoplayback && NetworkClients.Size() > 1)
		LevelStartStatus = LST_WAITING;
}

// [RH] Rewritten to properly calculate the packet size
//		with our variable length Command.
static size_t GetNetBufferSize()
{
	if (NetBuffer[0] & NCMD_EXIT)
		return 1 + (RemoteClient == Net_Arbitrator);
	// TODO: Need a skipper for this.
	if (NetBuffer[0] & NCMD_SETUP)
		return NetBufferLength;
	if (NetBuffer[0] & (NCMD_LATENCY | NCMD_LATENCYACK))
		return 2;

	if (NetBuffer[0] & NCMD_LEVELREADY)
	{
		int bytes = 2;
		if (RemoteClient == Net_Arbitrator)
			bytes += 2;

		return bytes;
	}

	// Header info
	unsigned int totalBytes = 10;
	if (NetBuffer[0] & NCMD_QUITTERS)
		totalBytes += NetBuffer[totalBytes] + 1;

	const int playerCount = NetBuffer[totalBytes++];
	const int numTics = NetBuffer[totalBytes++];
	if (numTics > 0)
		totalBytes += 4;
	const int ranTics = NetBuffer[totalBytes++];
	if (ranTics > 0)
		totalBytes += 4;
	// Stability buffer/commands ahead
	++totalBytes;

	// Minimum additional packet size per player:
	// 1 byte for player number
	// If from the host, 2 bytes for the latency to the host
	int padding = 1;
	if (RemoteClient == Net_Arbitrator)
		padding += 2;
	if (NetBufferLength < totalBytes + playerCount * padding)
		return totalBytes + playerCount * padding;

	TArrayView<uint8_t> skipper = TArrayView(&NetBuffer[totalBytes], MAX_MSGLEN - totalBytes);
	for (int p = 0; p < playerCount; ++p)
	{
		AdvanceStream(skipper, 1);
		if (RemoteClient == Net_Arbitrator)
			AdvanceStream(skipper, 2);

		for (int i = 0; i < ranTics; ++i)
			AdvanceStream(skipper, 3);

		for (int i = 0; i < numTics; ++i)
		{
			AdvanceStream(skipper, 1);
			SkipUserCmdMessage(skipper);
		}
	}

	return int(skipper.Data() - NetBuffer);
}

//
// HSendPacket
//
static void HSendPacket(int client, size_t size)
{
	// This data already exists locally in the demo file, so don't write it out.
	if (demoplayback)
		return;

	RemoteClient = client;
	NetBufferLength = size;
	if (client == consoleplayer)
	{
		memcpy(LocalNetBuffer, NetBuffer, size);
		LocalNetBufferSize = size;
		return;
	}

	if (!netgame)
		I_Error("Tried to send a packet to a client while offline");

	I_NetCmd(CMD_SEND);
}

// HGetPacket
// Returns false if no packet is waiting
static bool HGetPacket()
{
	if (demoplayback)
		return false;

	if (LocalNetBufferSize)
	{
		memcpy(NetBuffer, LocalNetBuffer, LocalNetBufferSize);
		NetBufferLength = LocalNetBufferSize;
		RemoteClient = consoleplayer;
		LocalNetBufferSize = 0u;
		return true;
	}

	if (!netgame)
		return false;

	I_NetCmd(CMD_GET);
	if (RemoteClient == -1)
	{
		// Allow NCMD_SETUP packets from unknown clients (mid-game join).
		if (NetBufferLength > 0 && (NetBuffer[0] & NCMD_SETUP))
			return true;
		return false;
	}

	size_t sizeCheck = GetNetBufferSize();
	if (NetBufferLength != sizeCheck)
	{
		Printf("Incorrect packet size %zu (expected %zu)\n", NetBufferLength, sizeCheck);
		return false;
	}

	return true;
}

static void ClientConnecting(int client)
{
	if (consoleplayer != Net_Arbitrator)
		return;

	// Initialize network state for the joining client.
	auto& state = ClientStates[client];
	memset(&state, 0, sizeof(FClientNetState));
	const int lastSeq = max(gametic / TicDup - 1, -1);
	const int lastCon = max(CurrentConsistency - 1, -1);
	state.CurrentSequence = lastSeq;
	state.SequenceAck = lastSeq;
	state.CurrentNetConsistency = lastCon;
	state.ConsistencyAck = lastCon;
	state.LastVerifiedConsistency = lastCon;
	state.Flags = CF_JOINING | CF_AWAITING_STATE;
	state.LastPacketReceivedTime = I_msTime();

	Printf("Client %d is joining mid-game\n", client);
}

static void DisconnectClient(int clientNum)
{
	NetworkClients -= clientNum;
	const uint64_t mask = ~((uint64_t)1u << clientNum);
	MutedClients &= mask;
	CutsceneReady &= mask;
	LevelStartAck &= mask;
	I_ClearClient(clientNum);
	// Capture the pawn leaving in the next world tick.
	players[clientNum].playerstate = PST_GONE;
}

static void SetArbitrator(int clientNum)
{
	Net_Arbitrator = clientNum;
	players[Net_Arbitrator].settings_controller = true;
	Printf("%s is the new host\n", players[Net_Arbitrator].userinfo.GetName());

	for (auto client : NetworkClients)
		ClientStates[client].AverageLatency = 0u;
	Net_ResetCommands(false);
	Net_SetWaiting();
}

// Mark a client for disconnect and inject DEM_PLAYERDISCONNECT into the
// lockstep command stream. All nodes will process the actual game state
// change (PST_GONE, actor destruction) at the same gametic, preventing
// desync between the host and remaining clients.
static void InitiatePlayerDisconnect(int clientNum)
{
	if (ClientStates[clientNum].Flags & CF_QUIT)
		return; // Already pending disconnect.
	ClientStates[clientNum].Flags |= CF_QUIT;
	Net_WriteInt8(DEM_PLAYERDISCONNECT);
	Net_WriteInt8(static_cast<uint8_t>(clientNum));
}

static void ClientQuit(int clientNum, int newHost)
{
	if (!NetworkClients.InGame(clientNum))
		return;

	// This will get caught in the main loop and send it out to everyone as one big packet. The only
	// exception is the host who will leave instantly and send out any needed data.
	if (clientNum != Net_Arbitrator)
	{
		if (consoleplayer != Net_Arbitrator)
			DPrintf(DMSG_WARNING, "Received disconnect packet from client %d erroneously\n", clientNum);
		else
			InitiatePlayerDisconnect(clientNum);

		return;
	}

	DisconnectClient(clientNum);
	if (clientNum == Net_Arbitrator)
		SetArbitrator((newHost >= 0 && newHost < (int)MAXPLAYERS && NetworkClients.InGame(newHost))
			? newHost : NetworkClients[0]);

	if (demorecording)
		G_CheckDemoStatus();
}

// ---------------------------------------------------------------------------
// Disconnect handshake helpers
// ---------------------------------------------------------------------------

static void SendSetupPacketToClient(int client, uint8_t subtype, const uint8_t* extra = nullptr, size_t extraSize = 0)
{
	if (extraSize > MAX_MSGLEN - 2)
		return;
	uint8_t buf[MAX_MSGLEN];
	buf[0] = NCMD_SETUP;
	buf[1] = subtype;
	size_t size = 2;
	if (extra && extraSize > 0)
	{
		memcpy(&buf[2], extra, extraSize);
		size += extraSize;
	}
	I_SendSetupPacket(client, buf, size);
}

static void SendSetupPacketToAll(uint8_t subtype, const uint8_t* extra = nullptr, size_t extraSize = 0, int excludeClient = -1)
{
	if (extraSize > MAX_MSGLEN - 2)
		return;
	uint8_t buf[MAX_MSGLEN];
	buf[0] = NCMD_SETUP;
	buf[1] = subtype;
	size_t size = 2;
	if (extra && extraSize > 0)
	{
		memcpy(&buf[2], extra, extraSize);
		size += extraSize;
	}
	for (auto client : NetworkClients)
	{
		if (client != consoleplayer && client != excludeClient)
			I_SendSetupPacket(client, buf, size);
	}
}

static void SendDisconnectRequest()
{
	uint8_t extra[1] = { static_cast<uint8_t>(consoleplayer) };
	SendSetupPacketToClient(Net_Arbitrator, PRE_DISCONNECT_REQUEST, extra, 1);
}

// ---------------------------------------------------------------------------
// Phase 1: Disconnect handshake handlers
// ---------------------------------------------------------------------------

// Host receives: a client wants to disconnect.
void HandleDisconnectRequest()
{
	if (consoleplayer != Net_Arbitrator)
		return;

	const int clientNum = RemoteClient;
	if (!NetworkClients.InGame(clientNum))
		return;

	DPrintf(DMSG_NOTIFY, "Received disconnect request from client %d\n", clientNum);

	// Mark for disconnect and inject DEM so all nodes remove at the same gametic.
	InitiatePlayerDisconnect(clientNum);

	// Send acknowledgment back to the departing client.
	SendSetupPacketToClient(clientNum, PRE_DISCONNECT_ACK);
}

// Client receives: host acknowledged our disconnect request.
void HandleDisconnectAck()
{
	if (RemoteClient != Net_Arbitrator)
		return;

	DPrintf(DMSG_NOTIFY, "Received disconnect ACK from host\n");
	bDisconnecting = false;
}

// Client receives: the host is leaving, a new host is designated.
void HandleDisconnectNotify()
{
	if (RemoteClient != Net_Arbitrator)
		return;

	if (NetBufferLength < 3)
		return;

	const int nextHost = NetBuffer[2];
	DPrintf(DMSG_NOTIFY, "Host is leaving, new host is client %d\n", nextHost);

	DisconnectClient(RemoteClient);
	SetArbitrator((nextHost >= 0 && nextHost < (int)MAXPLAYERS && NetworkClients.InGame(nextHost))
		? nextHost : NetworkClients[0]);

	if (demorecording)
		G_CheckDemoStatus();

	// Send confirmation back to the departing host.
	// Note: we send to the OLD host address. Since they're still listening
	// for confirms, they can receive this even though Net_Arbitrator changed.
	SendSetupPacketToClient(RemoteClient, PRE_DISCONNECT_CONFIRM);
}

// Departing host receives: a client confirmed our disconnect notification.
void HandleDisconnectConfirm()
{
	if (!bDisconnecting)
		return;

	const int clientNum = RemoteClient;
	if (clientNum >= 0 && clientNum < (int)MAXPLAYERS)
	{
		DisconnectConfirmMask |= ((uint64_t)1u << clientNum);
		DPrintf(DMSG_NOTIFY, "Client %d confirmed disconnect notification\n", clientNum);
	}
}

// ---------------------------------------------------------------------------
// Phase 2: Host migration handlers
// ---------------------------------------------------------------------------

static void SerializeMigrationState(FMigrationState& state)
{
	state.currentConsistency = CurrentConsistency;
	state.lastSentConsistency = LastSentConsistency;
	state.currentLobbyID = CurrentLobbyID;
	state.mutedClients = MutedClients;
	state.cutsceneReady = CutsceneReady;
	state.clientCount = 0;

	for (auto client : NetworkClients)
	{
		if (client == consoleplayer)
			continue;

		auto& src = ClientStates[client];
		auto& dst = state.clients[state.clientCount];
		dst.clientNum = client;
		dst.currentSequence = src.CurrentSequence;
		dst.sequenceAck = src.SequenceAck;
		dst.currentNetConsistency = src.CurrentNetConsistency;
		dst.consistencyAck = src.ConsistencyAck;
		dst.lastVerifiedConsistency = src.LastVerifiedConsistency;
		dst.flags = src.Flags;
		dst.averageLatency = src.AverageLatency;
		state.clientCount++;
	}
}

static void ApplyMigrationState(const FMigrationState& state)
{
	CurrentConsistency = state.currentConsistency;
	LastSentConsistency = state.lastSentConsistency;
	CurrentLobbyID = state.currentLobbyID;
	MutedClients = state.mutedClients;
	CutsceneReady = state.cutsceneReady;

	for (int i = 0; i < state.clientCount; ++i)
	{
		const auto& src = state.clients[i];
		if (src.clientNum < 0 || src.clientNum >= (int)MAXPLAYERS)
			continue;

		auto& dst = ClientStates[src.clientNum];
		dst.CurrentSequence = src.currentSequence;
		dst.SequenceAck = src.sequenceAck;
		dst.CurrentNetConsistency = src.currentNetConsistency;
		dst.ConsistencyAck = src.consistencyAck;
		dst.LastVerifiedConsistency = src.lastVerifiedConsistency;
		dst.Flags = src.flags;
		dst.AverageLatency = src.averageLatency;
	}
}

// New host receives: old host is beginning migration.
void HandleMigrationBegin()
{
	if (RemoteClient != Net_Arbitrator)
		return;

	DPrintf(DMSG_NOTIFY, "Received migration begin from host\n");
	bMigrating = true;
}

// New host receives: serialized host state data.
void HandleMigrationState()
{
	if (RemoteClient != Net_Arbitrator || !bMigrating)
		return;

	DPrintf(DMSG_NOTIFY, "Received migration state from host (%zu bytes)\n", NetBufferLength);

	// Deserialize the migration state from NetBuffer[2..].
	if (NetBufferLength >= 2 + sizeof(FMigrationState))
	{
		memcpy(&PendingMigrationState, &NetBuffer[2], sizeof(FMigrationState));
		bHasPendingMigration = true;
	}

	// ACK back to old host.
	SendSetupPacketToClient(RemoteClient, PRE_MIGRATION_STATE_ACK);
}

// Old host receives: new host got the state.
void HandleMigrationStateAck()
{
	if (!bDisconnecting)
		return;

	DPrintf(DMSG_NOTIFY, "New host acknowledged migration state\n");
	// The departing host can now mark migration as complete.
	// The actual disconnect continues in the D_QuitNetGame loop.
	bMigrating = false;
}

// Non-host client receives: a new host has taken over.
void HandleMigrationComplete()
{
	const int newHost = NetBuffer[2];
	DPrintf(DMSG_NOTIFY, "Migration complete, new host is client %d\n", newHost);

	bMigrating = false;

	if (newHost != Net_Arbitrator)
		SetArbitrator(newHost);

	// Send ready to the new host.
	SendSetupPacketToClient(newHost, PRE_MIGRATION_READY);
}

// New host receives: a client is ready after migration.
void HandleMigrationReady()
{
	if (consoleplayer != Net_Arbitrator)
		return;

	DPrintf(DMSG_NOTIFY, "Client %d is ready after migration\n", RemoteClient);
	// All clients will naturally resync via Net_ResetCommands + Net_SetWaiting
	// which was called in SetArbitrator().
}

// ---------------------------------------------------------------------------
// Phase 3: Mid-game join handlers (dedicated server + state transfer)
// ---------------------------------------------------------------------------

CUSTOM_CVAR(Bool, net_allowjoin, false, CVAR_SERVERINFO | CVAR_NOSAVE)
{
	// No special handling needed.
}

// Forward declarations for state transfer and user info (defined later in this file).
static FStateTransferSend PendingStateTransfer;
FStateTransferRecv IncomingStateTransfer;
static void AbortStateTransfer();
static void BeginStateTransfer(int client);
static int16_t CalculateConsistency(int client, uint32_t seed);
static void ResetNetLagHistories();
static int16_t CalculatePendingJoinConsistency(int client, uint32_t seed);

static bool IsWaitingForMidgameSpawn()
{
	return consoleplayer >= 0
		&& consoleplayer != Net_Arbitrator
		&& !playeringame[consoleplayer]
		&& IncomingStateTransfer.loadedSent
		&& !IncomingStateTransfer.activeSent;
}

static bool IsAwaitingMidgameActivation()
{
	return consoleplayer >= 0
		&& consoleplayer != Net_Arbitrator
		&& !playeringame[consoleplayer]
		&& IncomingStateTransfer.activeSent;
}

constexpr int NETLAG_HISTORY_SAMPLES = 350;

struct FNetLagHistory
{
	int Values[NETLAG_HISTORY_SAMPLES] = {};
	int Index = 0;
	int Count = 0;
	int Sum = 0;
	int Max = 0;

	void Reset()
	{
		memset(Values, 0, sizeof(Values));
		Index = Count = Sum = Max = 0;
	}

	void Push(int value)
	{
		value = max(value, 0);
		if (Count < NETLAG_HISTORY_SAMPLES)
		{
			Values[Index] = value;
			Sum += value;
			Max = max(Max, value);
			Index = (Index + 1) % NETLAG_HISTORY_SAMPLES;
			++Count;
			return;
		}

		const int old = Values[Index];
		Values[Index] = value;
		Sum += value - old;
		Index = (Index + 1) % NETLAG_HISTORY_SAMPLES;

		if (value >= Max)
		{
			Max = value;
		}
		else if (old == Max)
		{
			Max = 0;
			for (int i = 0; i < Count; ++i)
				Max = max(Max, Values[i]);
		}
	}

	int Current() const
	{
		if (Count <= 0)
			return 0;
		const int cur = (Index + NETLAG_HISTORY_SAMPLES - 1) % NETLAG_HISTORY_SAMPLES;
		return Values[cur];
	}

	int Average() const
	{
		return Count > 0 ? (Sum + Count / 2) / Count : 0;
	}
};

static FNetLagHistory LocalInputLagHistory = {};
static FNetLagHistory StreamBehindHistory[MAXPLAYERS] = {};
static int LastLagHistorySample = INT_MIN;
static int LastLagDumpSecond = INT_MIN;

CVAR(Bool, net_lagdump, false, CVAR_NOSAVE)

static void ResetNetLagDumpFile();
static void DumpNetLagHistoriesIfNeeded();

static int SeqTicsToMs(int tics)
{
	return min<int>(xs_RoundToInt(tics * TicDup * 1000.0 / TICRATE), 9999);
}

static FString GetNetLagDumpPath()
{
	if (consoleplayer == Net_Arbitrator)
		return "/tmp/uzdoom-netlag-host.log";
	if (consoleplayer >= 0 && consoleplayer < (int)MAXPLAYERS)
		return FStringf("/tmp/uzdoom-netlag-client%d.log", consoleplayer + 1);
	return "/tmp/uzdoom-netlag-client-unknown.log";
}

static bool IsTrackableClientForLagStats(int client)
{
	if (client < 0 || client >= (int)MAXPLAYERS || !NetworkClients.InGame(client))
		return false;
	if (ClientStates[client].Flags & (CF_JOINING | CF_AWAITING_STATE | CF_QUIT))
		return false;
	return true;
}

static int GetDisplayedClientSequenceForLagStats(int client, int newestSequence)
{
	int seq = ClientStates[client].CurrentSequence;
	if (consoleplayer != Net_Arbitrator && client == consoleplayer)
		seq = max(seq, newestSequence - 1);
	return seq;
}

static void ResetNetLagHistories()
{
	LocalInputLagHistory.Reset();
	for (auto& history : StreamBehindHistory)
		history.Reset();
	LastLagHistorySample = INT_MIN;
	LastLagDumpSecond = INT_MIN;
	if (net_lagdump)
		ResetNetLagDumpFile();
}

static void ResetNetLagDumpFile()
{
	const FString path = GetNetLagDumpPath();
	if (FILE* file = fopen(path.GetChars(), "w"))
	{
		fprintf(file, "# UZDoom network lag dump\n");
		fclose(file);
	}
}

static void SampleNetLagHistories()
{
	if (!netgame || demoplayback || LevelStartStatus != LST_READY)
		return;

	const int sampleSeq = gametic / TicDup;
	if (sampleSeq == LastLagHistorySample)
		return;
	LastLagHistorySample = sampleSeq;

	LocalInputLagHistory.Push(max<int>((ClientTic - gametic) / TicDup, 0));

	const int newestSequence = ClientTic / TicDup;
	for (int client = 0; client < (int)MAXPLAYERS; ++client)
	{
		if (!IsTrackableClientForLagStats(client))
		{
			if (!NetworkClients.InGame(client))
				StreamBehindHistory[client].Reset();
			continue;
		}

		const int seq = GetDisplayedClientSequenceForLagStats(client, newestSequence);
		StreamBehindHistory[client].Push(max(newestSequence - seq, 0));
	}

	DumpNetLagHistoriesIfNeeded();
}

static void DumpNetLagHistoriesIfNeeded()
{
	if (!net_lagdump || !netgame || demoplayback || LevelStartStatus != LST_READY)
		return;

	const int dumpSecond = gametic / TICRATE;
	if (dumpSecond == LastLagDumpSecond)
		return;
	LastLagDumpSecond = dumpSecond;

	const FString path = GetNetLagDumpPath();
	FILE* file = fopen(path.GetChars(), "a");
	if (file == nullptr)
		return;

	const int newestSeq = ClientTic / TicDup;
	int lowestSeq = newestSeq;
	for (auto client : NetworkClients)
	{
		if (client == consoleplayer || !IsTrackableClientForLagStats(client))
			continue;
		lowestSeq = min(lowestSeq, GetDisplayedClientSequenceForLagStats(client, newestSeq));
	}

	fprintf(file,
		"g=%d seq=%d con=%d local_now=%d local_avg=%d local_max=%d local_ms=%d/%d/%d buffer=%d buffer_ms=%d\n",
		gametic,
		newestSeq,
		CurrentConsistency,
		LocalInputLagHistory.Current(),
		LocalInputLagHistory.Average(),
		LocalInputLagHistory.Max,
		SeqTicsToMs(LocalInputLagHistory.Current()),
		SeqTicsToMs(LocalInputLagHistory.Average()),
		SeqTicsToMs(LocalInputLagHistory.Max),
		max(StabilityBuffer, 0),
		SeqTicsToMs(max(StabilityBuffer, 0)));

	for (auto client : NetworkClients)
	{
		if (client == consoleplayer)
			continue;

		const bool tracked = IsTrackableClientForLagStats(client);
		const int seq = tracked ? GetDisplayedClientSequenceForLagStats(client, newestSeq) : ClientStates[client].CurrentSequence;
		fprintf(file,
			"  p%d%s seq=%d ack=%d con=%d behind_now=%d behind_avg=%d behind_max=%d behind_ms=%d/%d/%d latency=%u flags=0x%x\n",
			client + 1,
			(tracked && seq == lowestSeq) ? " gate" : "",
			seq,
			ClientStates[client].SequenceAck,
			ClientStates[client].CurrentNetConsistency,
			tracked ? StreamBehindHistory[client].Current() : 0,
			tracked ? StreamBehindHistory[client].Average() : 0,
			tracked ? StreamBehindHistory[client].Max : 0,
			tracked ? SeqTicsToMs(StreamBehindHistory[client].Current()) : 0,
			tracked ? SeqTicsToMs(StreamBehindHistory[client].Average()) : 0,
			tracked ? SeqTicsToMs(StreamBehindHistory[client].Max) : 0,
			ClientStates[client].AverageLatency,
			ClientStates[client].Flags);
	}

	fflush(file);
	fclose(file);
}

static int16_t CalculatePendingJoinConsistency(int client, uint32_t seed)
{
	uint32_t value = seed ^ (0x45d9f3bu * static_cast<uint32_t>(client + 1));
	value ^= (value >> 16);
	int16_t consistency = static_cast<int16_t>(value & 0x7fff);
	return consistency != 0 ? consistency : 1;
}

static bool ShouldSuppressMidgameJoinLocalCommands()
{
	return consoleplayer >= 0
		&& consoleplayer != Net_Arbitrator
		&& (IsWaitingForMidgameSpawn() || IsAwaitingMidgameActivation());
}

bool Net_ShouldSuppressMidgameJoinInput()
{
	return ShouldSuppressMidgameJoinLocalCommands();
}

static void SendMidgameStateActive()
{
	if (consoleplayer < 0 || consoleplayer == Net_Arbitrator)
		return;

	const auto& player = players[consoleplayer];
	const uint8_t upPitch = static_cast<uint8_t>(clamp<int>(xs_RoundToInt(-player.MinPitch.Degrees()), 0, 127));
	const uint8_t downPitch = static_cast<uint8_t>(clamp<int>(xs_RoundToInt(player.MaxPitch.Degrees()), 0, 127));
	uint8_t buf[4] = { NCMD_SETUP, PRE_MIDGAME_STATE_ACTIVE, upPitch, downPitch };
	I_SendSetupPacket(Net_Arbitrator, buf, 4);
}

static void ResendMidgameStateActiveIfNeeded()
{
	if (!IncomingStateTransfer.activeSent)
		return;

	SendMidgameStateActive();
}

static void ConfirmMidgameJoinActive(int joinerSlot)
{
	if (consoleplayer != Net_Arbitrator)
		return;
	if (joinerSlot < 0 || joinerSlot >= (int)MAXPLAYERS)
		return;
	if (!(ClientStates[joinerSlot].Flags & CF_JOINING)
		|| !(ClientStates[joinerSlot].Flags & CF_AWAITING_ACTIVE))
		return;

	const int lastSeq = max(gametic / TicDup - 1, ClientStates[joinerSlot].CurrentSequence);
	const int lastCon = max(CurrentConsistency - 1, ClientStates[joinerSlot].CurrentNetConsistency);
	ClientStates[joinerSlot].CurrentSequence = lastSeq;
	ClientStates[joinerSlot].SequenceAck = lastSeq;
	ClientStates[joinerSlot].CurrentNetConsistency = lastCon;
	ClientStates[joinerSlot].ConsistencyAck = lastCon;
	ClientStates[joinerSlot].LastVerifiedConsistency = lastCon;
	ClientStates[joinerSlot].Flags &= ~CF_AWAITING_ACTIVE;

	const uint8_t upPitch = static_cast<uint8_t>(clamp<int>(xs_RoundToInt(-players[joinerSlot].MinPitch.Degrees()), 0, 127));
	const uint8_t downPitch = static_cast<uint8_t>(clamp<int>(xs_RoundToInt(players[joinerSlot].MaxPitch.Degrees()), 0, 127));
	Net_WriteInt8(DEM_MIDGAMEACTIVE);
	Net_WriteInt8(static_cast<uint8_t>(joinerSlot));
	Net_WriteInt8(upPitch);
	Net_WriteInt8(downPitch);
}

static bool IsMapLoaded();
void Net_SetupUserInfo();
static uint32_t StaticSumSeeds();

static bool CanAcceptMidgameJoinNow()
{
	return gamestate == GS_LEVEL
		&& IsMapLoaded()
		&& gameaction == ga_nothing
		&& LevelStartStatus == LST_READY
		&& !bMigrating
		&& !PendingStateTransfer.active;
}

static void RejectIncomingMidgameJoin(EMidgameRejectReason reason)
{
	uint8_t buf[3] = { NCMD_SETUP, PRE_MIDGAME_REJECT, static_cast<uint8_t>(reason) };
	I_SendSetupPacketToAddress(buf, 3);
}


// Host receives: an unknown client wants to join mid-game.
void HandleMidgameConnect()
{
	if (consoleplayer != Net_Arbitrator)
		return;

	// Mid-game join is allowed on dedicated servers by default.
	// P2P hosts can enable it via net_allowjoin, but it may cause
	// lockstep desync issues with 3+ players.
	if (!dedicatedServer && !net_allowjoin)
	{
		RejectIncomingMidgameJoin(REJECT_DISABLED);
		return;
	}

	// Only allow late joins while the host is safely inside active level play.
	// Everything else should be treated as a transient retry so we do not race
	// level transitions or partially initialize a join that cannot complete.
	if (!CanAcceptMidgameJoinNow())
	{
		RejectIncomingMidgameJoin(REJECT_IN_TRANSITION);
		return;
	}

	// Reject if another joiner is already in the pipeline — either actively
	// transferring state or still initializing (V_Init2/shaders) before
	// requesting state. Without this, multiple clients can be accepted and
	// all but the first deadlock when their STATE_READY is silently dropped.
	for (auto c : NetworkClients)
	{
		if (ClientStates[c].Flags & (CF_JOINING | CF_AWAITING_STATE))
		{
			RejectIncomingMidgameJoin(REJECT_IN_TRANSITION);
			return;
		}
	}
	
	if (I_IsAddressBanned())
	{
		RejectIncomingMidgameJoin(REJECT_BANNED);
		return;
	}

	// Find a free player slot (skip slot 0 on dedicated server — ghost player).
	int freeSlot = -1;
	for (int i = (dedicatedServer ? 1 : 0); i < MaxClients; ++i)
	{
		if (!NetworkClients.InGame(i))
		{
			freeSlot = i;
			break;
		}
	}

	if (freeSlot < 0)
	{
		RejectIncomingMidgameJoin(REJECT_FULL);
		return;
	}

	// Validate engine version and loaded files.
	uint8_t* engineInfo = &NetBuffer[2];
	size_t passwordOffset = 0u;
	FVerificationError error = Net_VerifyEngine(engineInfo, passwordOffset);
	if (error.Error != FVerificationError::VE_NONE)
	{
		RejectIncomingMidgameJoin(REJECT_VERIFICATION);
		return;
	}

	// Validate password.
	const bool hasPassword = strlen(net_password) > 0;
	if (hasPassword && (2u + passwordOffset >= (size_t)NetBufferLength
		|| memchr(&NetBuffer[2u + passwordOffset], '\0', NetBufferLength - 2u - passwordOffset) == nullptr
		|| strcmp(net_password, (const char*)&NetBuffer[2u + passwordOffset])))
	{
		RejectIncomingMidgameJoin(REJECT_PASSWORD);
		return;
	}

	// Send extended acceptance with slot, TicDup, MaxClients, GameID, active roster, LobbyID.
	uint8_t buf[MAX_MSGLEN];
	size_t pos = 0;
	buf[pos++] = NCMD_SETUP;
	buf[pos++] = PRE_MIDGAME_ACCEPT;
	buf[pos++] = static_cast<uint8_t>(freeSlot);
	buf[pos++] = TicDup;
	buf[pos++] = static_cast<uint8_t>(MaxClients);
	I_GetGameID(&buf[pos]);
	pos += 8;
	buf[pos++] = CurrentLobbyID;
	buf[pos++] = dedicatedServer ? 1 : 0;
	// Active player roster: count + list of active client numbers.
	uint8_t rosterCount = 0;
	size_t rosterCountPos = pos++;
	for (auto client : NetworkClients)
	{
		if (client != freeSlot)
		{
			buf[pos++] = static_cast<uint8_t>(client);
			rosterCount++;
		}
	}
	buf[rosterCountPos] = rosterCount;

	// Append server CVARs (dmflags, compatflags, etc.) so the joiner
	// has the same settings as lobby joiners (via PRE_GAME_INFO).
	// Pre-check size to avoid I_Error if CVARs exceed the remaining buffer.
	FString cvarDump = C_GetMassCVarString(CVAR_SERVERINFO, true);
	const size_t cvarNeeded = cvarDump.Len() + 1; // null terminator
	if (pos + cvarNeeded <= MAX_MSGLEN)
	{
		TArrayView<uint8_t> cvarStream = TArrayView(&buf[pos], MAX_MSGLEN - pos);
		C_WriteCVars(cvarStream, CVAR_SERVERINFO, true);
		pos += cvarStream.Data() - &buf[pos];
	}
	else
	{
		Printf("HandleMidgameConnect: CVAR data too large (%zu bytes), sending without\n", cvarNeeded);
	}

	I_SendSetupPacketToAddress(buf, pos);

	// Initialize network state BEFORE adding to NetworkClients, so the
	// CF_JOINING flag is set before TryRunTics can see this client.
	I_SetClientAddress(freeSlot);
	ClientConnecting(freeSlot);
	NetworkClients += freeSlot;

	// Don't start state transfer yet — wait until the client is fully
	// initialized (V_Init2, shaders done) and requests state from D_DoomLoop.
	// This minimizes the gap between snapshot and DEM_MIDGAMESPAWN.
}

// Joining client receives: host accepted our mid-game join.
void HandleMidgameAccept()
{
	if (consoleplayer != -1)
		return; // Already have a slot.

	// Minimum: 2 header + 1 slot + 1 ticdup + 1 maxclients + 8 gameID + 1 lobbyID + 1 dedicated + 1 rosterCount = 16
	if (NetBufferLength < 16)
		return;

	size_t pos = 2;
	const int slot = NetBuffer[pos++];
	if (slot < 0 || slot >= (int)MAXPLAYERS)
		return;
	TicDup = NetBuffer[pos++];
	MaxClients = NetBuffer[pos++];
	if (MaxClients <= 0 || MaxClients > (int)MAXPLAYERS)
		return;

	// GameID (8 bytes)
	I_SetGameID(&NetBuffer[pos]);
	pos += 8;

	// LobbyID (1 byte)
	CurrentLobbyID = NetBuffer[pos++];

	// hostIsDedicated (1 byte)
	if (NetBuffer[pos++])
		hostIsDedicated = true;

	// Active player roster
	const uint8_t rosterCount = NetBuffer[pos++];
	if (NetBufferLength < pos + rosterCount)
		return;
	for (uint8_t i = 0; i < rosterCount; ++i)
	{
		const int client = NetBuffer[pos++];
		if (client >= 0 && client < (int)MAXPLAYERS)
			NetworkClients += client;
	}

	// Read server CVARs (dmflags, compatflags, etc.) appended after the roster.
	if (pos < NetBufferLength)
	{
		TArrayView<uint8_t> cvarStream = TArrayView(&NetBuffer[pos], NetBufferLength - pos);
		C_ReadCVars(cvarStream);
	}

	consoleplayer = slot;
	NetworkClients += slot;
	Net_SetupUserInfo();

	// Signal that we're joining mid-game. D_InitGame will skip normal map
	// load, and G_DoMidgameJoin will request state from D_DoomLoop.
	gameaction = ga_midgamejoin;

	Printf("Mid-game join accepted, assigned slot %d (roster: %d players)\n", slot, rosterCount);
}

// Joining client receives: host rejected our mid-game join.
void HandleMidgameReject()
{
	if (NetBufferLength < 3)
		return;
	const uint8_t reason = NetBuffer[2];
	const char* reasonStr = "unknown";
	switch (reason)
	{
	case REJECT_FULL:			reasonStr = "game is full"; break;
	case REJECT_IN_TRANSITION:	reasonStr = "server is busy (migration or state transfer in progress)"; break;
	case REJECT_BANNED:			reasonStr = "banned"; break;
	case REJECT_PASSWORD:		reasonStr = "wrong password"; break;
	case REJECT_VERIFICATION:	reasonStr = "verification failed"; break;
	case REJECT_DISABLED:		reasonStr = "mid-game joining is disabled"; break;
	}
	Printf("Cannot join: %s\n", reasonStr);
}

// Non-host client receives: a new player is joining.
void HandleMidgamePlayerJoin()
{
	if (NetBufferLength < 3)
		return;
	if (RemoteClient != Net_Arbitrator)
		return;

	const int newPlayer = NetBuffer[2];
	if (newPlayer < 0 || newPlayer >= (int)MAXPLAYERS)
		return;

	// Duplicate join notifications can carry the joiner's finalized userinfo
	// once the host has it, so always apply any appended payload first.
	if (NetBufferLength > 3)
	{
		TArrayView<uint8_t> stream = TArrayView(&NetBuffer[3], NetBufferLength - 3);
		D_ReadUserInfoStrings(newPlayer, stream, false);
	}

	if (NetworkClients.InGame(newPlayer))
	{
		// Already know about this player (retransmitted PLAYER_JOIN). Just ACK again.
		SendSetupPacketToClient(Net_Arbitrator, PRE_MIDGAME_PLAYER_ACK);
		return;
	}

	Printf("Player %d is joining mid-game\n", newPlayer);
	NetworkClients += newPlayer;

	// Initialize client state for the new player.
	auto& state = ClientStates[newPlayer];
	memset(&state, 0, sizeof(FClientNetState));
	const int lastSeq = max(gametic / TicDup - 1, -1);
	const int lastCon = max(CurrentConsistency - 1, -1);
	state.CurrentSequence = lastSeq;
	state.SequenceAck = lastSeq;
	state.CurrentNetConsistency = lastCon;
	state.ConsistencyAck = lastCon;
	state.LastVerifiedConsistency = lastCon;
	state.Flags = CF_JOINING;
	state.LastPacketReceivedTime = I_msTime();

	// ACK back to host.
	SendSetupPacketToClient(Net_Arbitrator, PRE_MIDGAME_PLAYER_ACK);
}

// Host receives: a client acknowledged the new player.
void HandleMidgamePlayerAck()
{
	if (consoleplayer != Net_Arbitrator)
		return;

	DPrintf(DMSG_NOTIFY, "Client %d acknowledged new player\n", RemoteClient);
}

// ---------------------------------------------------------------------------
// Mid-game state transfer
// ---------------------------------------------------------------------------

static void SendNextStateChunk();

static void BeginStateTransfer(int client)
{
	auto& xfer = PendingStateTransfer;
	xfer.Clear();

	xfer.clientNum = client;
	xfer.hostGametic = gametic;
	xfer.hostConsistency = CurrentConsistency;
	xfer.hostLobbyID = CurrentLobbyID;
	xfer.mapName = primaryLevel->MapName;

	// Undo prediction before snapshot so we capture true state at gametic.
	if (playeringame[consoleplayer] && !dedicatedServer)
		P_UnPredictClient();

	// Snapshot the level.
	primaryLevel->SnapshotLevel();
	auto* info = FindLevelInfo(primaryLevel->MapName.GetChars());
	if (!info || !info->Snapshot.mBuffer)
	{
		Printf("BeginStateTransfer: failed to create snapshot\n");
		if (playeringame[consoleplayer] && !dedicatedServer)
			P_PredictClient();
		return;
	}

	FSerializer globals;
	if (!globals.OpenWriter(false))
	{
		Printf("BeginStateTransfer: failed to serialize globals\n");
		if (playeringame[consoleplayer] && !dedicatedServer)
			P_PredictClient();
		return;
	}
	G_SerializeMidgameGlobalsPreInit(globals);
	G_SerializeMidgameGlobalsPostInit(globals);
	unsigned globalsSize = 0;
	const char* globalsData = globals.GetOutput(&globalsSize);

	// Build transfer buffer: [globals JSON blob] + [16-byte snapshot header] + [compressed snapshot]
	const auto& snap = info->Snapshot;
	const size_t snapshotHeaderSize = 16;
	const size_t snapshotSize = snap.mCompressedSize;
	xfer.globalsSize = globalsSize;
	xfer.data.Resize(xfer.globalsSize + snapshotHeaderSize + snapshotSize);

	// Copy globals first.
	if (xfer.globalsSize > 0)
		memcpy(xfer.data.Data(), globalsData, xfer.globalsSize);

	// Then the snapshot header + data.
	uint8_t* hdr = xfer.data.Data() + xfer.globalsSize;
	WriteBE32(&hdr[0], snap.mSize);
	WriteBE32(&hdr[4], snap.mCompressedSize);
	WriteBE32(&hdr[8], snap.mMethod);
	WriteBE32(&hdr[12], snap.mCRC32);
	memcpy(hdr + snapshotHeaderSize, snap.mBuffer, snapshotSize);

	// Clean up the snapshot from the level info (we have our own copy now).
	info->Snapshot.Clean();

	xfer.numChunks = (xfer.data.Size() + STATE_CHUNK_PAYLOAD - 1) / STATE_CHUNK_PAYLOAD;
	xfer.nextChunkToSend = 0;
	xfer.retryCount = 0;
	xfer.transferStartTime = I_msTime();
	xfer.active = true;

	// Re-predict after snapshot.
	if (playeringame[consoleplayer] && !dedicatedServer)
		P_PredictClient();

	// Send STATE_BEGIN metadata.
	uint8_t buf[128];
	size_t pos = 0;
	buf[pos++] = NCMD_SETUP;
	buf[pos++] = PRE_MIDGAME_STATE_BEGIN;
	const size_t totalSize = xfer.data.Size();
	WriteBE32(&buf[pos], (uint32_t)totalSize);	pos += 4;
	WriteBE32(&buf[pos], (uint32_t)xfer.globalsSize);	pos += 4;
	WriteBE32(&buf[pos], (uint32_t)xfer.hostGametic);	pos += 4;
	WriteBE32(&buf[pos], (uint32_t)xfer.hostConsistency);	pos += 4;
	// Lobby ID (1 byte)
	buf[pos++] = xfer.hostLobbyID;
	// Game skill (1 byte) — must match on all nodes for damage/ammo factors.
	buf[pos++] = static_cast<uint8_t>(gameskill);
	// Deathmatch mode (1 byte) — affects item respawns, scoring, etc.
	buf[pos++] = static_cast<uint8_t>(*deathmatch);
	// Map name (null-terminated string)
	const char* mapStr = xfer.mapName.GetChars();
	const size_t mapLen = strlen(mapStr) + 1;
	if (pos + mapLen > sizeof(buf))
	{
		Printf("BeginStateTransfer: map name too long (%zu)\n", mapLen);
		AbortStateTransfer();
		return;
	}
	memcpy(&buf[pos], mapStr, mapLen);
	pos += mapLen;

	I_SendSetupPacket(client, buf, pos);

	// Send first chunk.
	SendNextStateChunk();
}

static void AbortStateTransfer()
{
	if (PendingStateTransfer.active)
	{
		const int client = PendingStateTransfer.clientNum;
		Printf("State transfer to client %d aborted\n", client);

		// Notify the joiner so they can exit ga_midgamejoin, then clean up
		// the slot so it doesn't remain in a zombie state.
		if (NetworkClients.InGame(client))
		{
			SendSetupPacketToClient(client, PRE_MIDGAME_STATE_ERROR);
			ClientStates[client].Flags &= ~(CF_JOINING | CF_AWAITING_STATE | CF_AWAITING_ACTIVE);
			DisconnectClient(client);
		}

		PendingStateTransfer.Clear();
	}
}

static void SendNextStateChunk()
{
	auto& xfer = PendingStateTransfer;
	if (!xfer.active || xfer.nextChunkToSend >= xfer.numChunks)
		return;

	const size_t offset = xfer.nextChunkToSend * STATE_CHUNK_PAYLOAD;
	const size_t remaining = xfer.data.Size() - offset;
	const size_t chunkSize = min<size_t>(remaining, STATE_CHUNK_PAYLOAD);

	uint8_t buf[4 + STATE_CHUNK_PAYLOAD];
	buf[0] = NCMD_SETUP;
	buf[1] = PRE_MIDGAME_STATE_CHUNK;
	WriteBE16(&buf[2], (uint16_t)xfer.nextChunkToSend);
	memcpy(&buf[4], &xfer.data[offset], chunkSize);

	I_SendSetupPacket(xfer.clientNum, buf, 4 + chunkSize);
	xfer.lastSendTime = I_msTime();
}

void TickStateTransfer()
{
	auto& xfer = PendingStateTransfer;
	if (!xfer.active)
		return;

	// If the target client has been marked for disconnect (CF_QUIT) or is no
	// longer in the game, abort the transfer immediately.
	if ((ClientStates[xfer.clientNum].Flags & CF_QUIT) || !NetworkClients.InGame(xfer.clientNum))
	{
		Printf("State transfer aborted: client %d disconnected\n", xfer.clientNum);
		AbortStateTransfer();
		return;
	}

	const uint64_t now = I_msTime();

	// Hard timeout: if the entire transfer (including client load time) exceeds
	// the total timeout, abort. This catches clients that crash during load.
	if (now - xfer.transferStartTime >= STATE_TRANSFER_TOTAL_TIMEOUT_MS)
	{
		Printf("State transfer to client %d timed out (total elapsed %llums)\n",
			   xfer.clientNum, (unsigned long long)(now - xfer.transferStartTime));
		AbortStateTransfer();
		return;
	}

	if (now - xfer.lastSendTime >= (uint64_t)STATE_TRANSFER_TIMEOUT_MS)
	{
		if (xfer.retryCount >= STATE_TRANSFER_MAX_RETRIES)
		{
			Printf("State transfer to client %d timed out (chunk retries exhausted)\n", xfer.clientNum);
			AbortStateTransfer();
			return;
		}

		if (xfer.nextChunkToSend < xfer.numChunks)
		{
			// Resend current chunk.
			SendNextStateChunk();
			xfer.retryCount++;
		}
		else
		{
			// All chunks sent, waiting for client to load snapshot.
			// Client may take a long time (shader compilation, etc.)
			// so just periodically resend STATE_COMPLETE. The hard timeout
			// above ensures we don't wait forever if the client crashed.
			uint8_t buf[2] = { NCMD_SETUP, PRE_MIDGAME_STATE_COMPLETE };
			I_SendSetupPacket(xfer.clientNum, buf, 2);
			xfer.lastSendTime = now;
		}
	}
}

// Host receives: chunk ACK from joining client.
void HandleMidgameStateChunkAck()
{
	if (NetBufferLength < 4)
		return;
	if (consoleplayer != Net_Arbitrator || !PendingStateTransfer.active)
		return;
	if (RemoteClient != PendingStateTransfer.clientNum)
		return;

	const size_t ackChunk = ReadBE16(&NetBuffer[2]);
	auto& xfer = PendingStateTransfer;

	if ((int)ackChunk != (int)xfer.nextChunkToSend)
		return; // Out of order ACK, ignore.

	xfer.nextChunkToSend++;
	xfer.retryCount = 0;

	if (xfer.nextChunkToSend >= xfer.numChunks)
	{
		// All chunks sent and ACKed. Send completion signal.
		uint8_t buf[2] = { NCMD_SETUP, PRE_MIDGAME_STATE_COMPLETE };
		I_SendSetupPacket(xfer.clientNum, buf, 2);
		// Reset timer — now waiting for STATE_LOADED from the client.
		xfer.lastSendTime = I_msTime();
		xfer.retryCount = 0;
	}
	else
	{
		// Send next chunk.
		SendNextStateChunk();
	}
}

// Joiner receives: state transfer metadata from host.
void HandleMidgameStateBegin()
{
	auto& xfer = IncomingStateTransfer;
	if (xfer.active)
		return; // Already receiving.

	// Minimum size: 2 (header) + 4+4+4+4+1+1+1 (fields) + 1 (map name null) = 22 bytes.
	if (NetBufferLength < 22)
		return;

	uint8_t* p = &NetBuffer[2];
	xfer.totalSize = ReadBE32(p);	p += 4;
	xfer.globalsSize = ReadBE32(p);	p += 4;
	xfer.hostGametic = (int)ReadBE32(p);	p += 4;
	xfer.hostConsistency = (int)ReadBE32(p);	p += 4;
	xfer.hostLobbyID = p[0];
	p += 1;
	// Synchronize game skill and deathmatch mode from the host.
	// These CVAR_LATCH CVARs are NOT included in the snapshot — they're
	// applied by G_InitNew via UnlatchCVars(). Without this, joiners use
	// their own defaults, causing damage/ammo factor divergence.
	gameskill = (int)p[0];
	p += 1;
	deathmatch = (int)p[0];
	p += 1;

	// Validate that globalsSize fits within totalSize.
	if (xfer.globalsSize > xfer.totalSize)
		return;

	// Validate that mapName is null-terminated within the remaining buffer.
	const size_t remaining = NetBufferLength - (p - &NetBuffer[0]);
	if (memchr(p, '\0', remaining) == nullptr)
		return;
	xfer.mapName = (const char*)p;

	// Sanity-check totalSize to prevent malicious packets from causing
	// excessive memory allocation. 64MB is far beyond any real snapshot.
	constexpr size_t MAX_STATE_SIZE = 64u * 1024u * 1024u;
	if (xfer.totalSize == 0 || xfer.totalSize > MAX_STATE_SIZE)
		return;

	xfer.numChunks = (xfer.totalSize + STATE_CHUNK_PAYLOAD - 1) / STATE_CHUNK_PAYLOAD;
	xfer.data.Resize(xfer.totalSize);
	memset(xfer.data.Data(), 0, xfer.totalSize);
	xfer.nextExpectedChunk = 0;
	xfer.active = true;
}

// Joiner receives: one chunk of state data.
void HandleMidgameStateChunk()
{
	if (NetBufferLength < 5)
		return; // Minimum: 2 header + 2 chunk index + 1 byte data

	auto& xfer = IncomingStateTransfer;
	if (!xfer.active)
		return;

	const size_t chunkIdx = ReadBE16(&NetBuffer[2]);
	if (chunkIdx != xfer.nextExpectedChunk)
		return; // Out of order, ignore (host will retransmit).

	const size_t offset = chunkIdx * STATE_CHUNK_PAYLOAD;
	const size_t chunkSize = NetBufferLength - 4;
	if (offset + chunkSize > xfer.totalSize)
	{
		Printf("State transfer error: chunk overflows buffer\n");
		xfer.Clear();
		return;
	}

	memcpy(&xfer.data[offset], &NetBuffer[4], chunkSize);
	xfer.nextExpectedChunk++;

	// ACK this chunk.
	uint8_t buf[4] = { NCMD_SETUP, PRE_MIDGAME_STATE_CHUNK_ACK, NetBuffer[2], NetBuffer[3] };
	I_SendSetupPacket(Net_Arbitrator, buf, 4);
}

// Joiner receives: all chunks have been sent.
void HandleMidgameStateComplete()
{
	auto& xfer = IncomingStateTransfer;
	if (!xfer.active)
	{
		// We already loaded the snapshot or processed the spawn locally, but
		// the host may have missed our last setup packet. Resend so it can
		// continue the join handshake.
		if (xfer.activeSent)
		{
			SendMidgameStateActive();
		}
		else if (xfer.loadedSent)
		{
			uint8_t buf[2] = { NCMD_SETUP, PRE_MIDGAME_STATE_LOADED };
			I_SendSetupPacket(Net_Arbitrator, buf, 2);
		}
		return;
	}

	if (xfer.nextExpectedChunk < xfer.numChunks)
	{
		// Missing chunks — send error.
		uint8_t buf[2] = { NCMD_SETUP, PRE_MIDGAME_STATE_ERROR };
		I_SendSetupPacket(Net_Arbitrator, buf, 2);
		Printf("State transfer incomplete: expected %zu chunks, got %zu\n",
			   xfer.numChunks, xfer.nextExpectedChunk);
		xfer.Clear();
		gameaction = ga_fullconsole;
		return;
	}

	Printf("Game state received, loading...\n");
	xfer.active = false; // Prevent retransmitted STATE_COMPLETE from re-triggering
	// gameaction is already ga_midgamejoin (set in HandleMidgameAccept).
	// G_DoMidgameJoin will detect xfer.data.Size() > 0 and load the snapshot.
}

// Host receives: STATE_READY from joiner. Client is initialized (V_Init2 done)
// and ready to receive state. Take a fresh snapshot and start transfer.
void HandleMidgameStateReady()
{
	if (consoleplayer != Net_Arbitrator)
		return;

	const int joinerSlot = RemoteClient;
	if (joinerSlot < 0
		|| !(ClientStates[joinerSlot].Flags & CF_JOINING)
		|| !(ClientStates[joinerSlot].Flags & CF_AWAITING_STATE))
		return;

	if (!CanAcceptMidgameJoinNow())
	{
		Printf("Client %d became ready while the host was transitioning, aborting join\n", joinerSlot);
		SendSetupPacketToClient(joinerSlot, PRE_MIDGAME_STATE_ERROR);
		ClientStates[joinerSlot].Flags &= ~(CF_JOINING | CF_AWAITING_STATE | CF_AWAITING_ACTIVE);
		DisconnectClient(joinerSlot);
		return;
	}

	if (NetBufferLength > 2)
	{
		TArrayView<uint8_t> stream = TArrayView(&NetBuffer[2], NetBufferLength - 2);
		D_ReadUserInfoStrings(joinerSlot, stream, false);
	}

	Printf("Client %d ready for state, taking snapshot...\n", joinerSlot);
	BeginStateTransfer(joinerSlot);
}

// Host receives: STATE_LOADED from joiner. Client loaded the snapshot
// successfully. Broadcast PLAYER_JOIN and inject DEM_MIDGAMESPAWN.
void HandleMidgameStateLoaded()
{
	if (consoleplayer != Net_Arbitrator)
		return;

	const int joinerSlot = RemoteClient;
	if (joinerSlot < 0
		|| !(ClientStates[joinerSlot].Flags & CF_JOINING)
		|| !(ClientStates[joinerSlot].Flags & CF_AWAITING_STATE))
		return;

	// Verify this is the client we're actually transferring state to.
	if (!PendingStateTransfer.active || joinerSlot != PendingStateTransfer.clientNum)
		return;

	Printf("Client %d loaded game state, waiting for activation sync\n", joinerSlot);

	// Notify all existing clients that a new player is joining.
	uint8_t buf[MAX_MSGLEN];
	size_t pos = 0;
	buf[pos++] = NCMD_SETUP;
	buf[pos++] = PRE_MIDGAME_PLAYER_JOIN;
	buf[pos++] = static_cast<uint8_t>(joinerSlot);

	const FString userinfo = D_GetUserInfoStrings(joinerSlot, true);
	const size_t userinfoSize = userinfo.Len() + 1;
	if (pos + userinfoSize > MAX_MSGLEN)
	{
		Printf("HandleMidgameStateLoaded: userinfo too large for player %d (%zu bytes)\n", joinerSlot, userinfoSize);
	}
	else
	{
		memcpy(&buf[pos], userinfo.GetChars(), userinfoSize);
		pos += userinfoSize;
	}

	for (auto c : NetworkClients)
	{
		if (c != consoleplayer && c != joinerSlot)
			I_SendSetupPacket(c, buf, pos);
	}

	// Inject DEM_MIDGAMESPAWN into the host's event stream.
	// All nodes (host + existing clients + joiner) will process this at the
	// same gametic via the lockstep protocol, deterministically aligning the
	// new player's network state before the real spawn happens at activation.
	Net_WriteInt8(DEM_MIDGAMESPAWN);
	Net_WriteInt8(static_cast<uint8_t>(joinerSlot));

	// Proactively send commands from the snapshot gametic to the joiner.
	// The host's send loop normally starts from gametic/TicDup (current tic),
	// but the joiner's snapshot was taken at hostGametic — creating a gap
	// where the joiner needs commands that would never be sent. Without this,
	// the joiner stalls for seconds while the retransmission mechanism slowly
	// fills the gap one round-trip at a time.
	const int lastSeq = max(PendingStateTransfer.hostGametic / TicDup - 1, -1);
	const int lastCon = max(PendingStateTransfer.hostConsistency - 1, -1);
	auto& joinState = ClientStates[joinerSlot];
	joinState.CurrentSequence = lastSeq;
	joinState.SequenceAck = lastSeq;
	joinState.ResendSequenceFrom = PendingStateTransfer.hostGametic / TicDup;
	joinState.CurrentNetConsistency = lastCon;
	joinState.ConsistencyAck = lastCon;
	joinState.LastVerifiedConsistency = lastCon;
	joinState.ResendConsistencyFrom = -1;
	joinState.Flags &= ~(CF_MISSING | CF_RETRANSMIT);

	// Clear CF_AWAITING_STATE so duplicate STATE_LOADED packets won't
	// re-trigger while we wait for the joiner's first live gameplay command.
	joinState.Flags &= ~CF_AWAITING_STATE;
	joinState.Flags |= CF_AWAITING_ACTIVE;

	PendingStateTransfer.Clear();
}

// Host receives: STATE_ACTIVE from joiner. This confirms the joiner has
// processed the pre-activation sync point locally and is ready for the
// deterministic activation/spawn event from the current host tic.
void HandleMidgameStateActive()
{
	if (consoleplayer != Net_Arbitrator)
		return;

	const int joinerSlot = RemoteClient;
	if (joinerSlot < 0
		|| !(ClientStates[joinerSlot].Flags & CF_JOINING)
		|| !(ClientStates[joinerSlot].Flags & CF_AWAITING_ACTIVE))
		return;

	if (NetBufferLength >= 4)
	{
		TArrayView<uint8_t> stream = TArrayView(&NetBuffer[2], NetBufferLength - 2);
		players[joinerSlot].MinPitch = DAngle::fromDeg(-ReadInt8(stream));
		players[joinerSlot].MaxPitch = DAngle::fromDeg(ReadInt8(stream));
	}

	ConfirmMidgameJoinActive(joinerSlot);
}

// Host/Joiner receives: error during state transfer.
void HandleMidgameStateError()
{
	if (consoleplayer == Net_Arbitrator)
	{
		Printf("Client reported state transfer error, aborting\n");
		AbortStateTransfer();
	}
	else
	{
		Printf("Host reported state transfer error\n");
		G_AbortMidgameJoin();
	}
}

// Called from G_DoMidgameJoin after snapshot is loaded. Aligns the joiner's
// network state with the host's so lockstep proceeds correctly.
void Net_PrepareMidgameSync()
{
	auto& xfer = IncomingStateTransfer;

	gametic = xfer.hostGametic;
	ClientTic = xfer.hostGametic / TicDup;
	CurrentConsistency = xfer.hostConsistency;
	LastSentConsistency = xfer.hostConsistency;
	CurrentLobbyID = xfer.hostLobbyID;

	// RNG state is restored from the globals portion in G_DoMidgameJoin(),
	// which runs before this function. The joiner now has the exact same
	// RNG state as the server at the snapshot gametic.

	// Clear local commands so we don't replay stale input.
	memset(LocalCmds, 0, sizeof(LocalCmds));
	const int lastSeq = max(ClientTic - 1, -1);
	const int lastCon = max(CurrentConsistency - 1, -1);

	// Initialize our own client state.
	auto& state = ClientStates[consoleplayer];
	const int preservedFlags = state.Flags & (CF_QUIT | CF_JOINING | CF_AWAITING_STATE | CF_AWAITING_ACTIVE);
	memset(&state, 0, sizeof(FClientNetState));
	state.CurrentSequence = lastSeq;
	state.SequenceAck = lastSeq;
	state.CurrentNetConsistency = lastCon;
	state.ConsistencyAck = lastCon;
	state.LastVerifiedConsistency = lastCon;
	state.LastPacketReceivedTime = I_msTime();
	state.Flags = preservedFlags;

	// Initialize sequence and consistency tracking for all existing clients.
	// Without this, stale CurrentSequence values from the pre-game lobby
	// (e.g., seq 699) would make lowestSequence negative, freezing the
	// joiner — which then starves the host of commands, freezing everyone.
	for (auto c : NetworkClients)
	{
		if (c != consoleplayer)
		{
			ClientStates[c].CurrentSequence = lastSeq;
			ClientStates[c].SequenceAck = lastSeq;
			ClientStates[c].LastVerifiedConsistency = lastCon;
			ClientStates[c].CurrentNetConsistency = lastCon;
			ClientStates[c].ConsistencyAck = lastCon;
		}
	}

	Printf("Mid-game sync: gametic=%d, consistency=%d, lobbyID=%d\n",
		   gametic, CurrentConsistency, CurrentLobbyID);
}

// ---------------------------------------------------------------------------

static bool IsMapLoaded()
{
	return gamestate == GS_LEVEL;
}

static void CheckLevelStart(int client, int delayTics)
{
	if (LevelStartStatus != LST_WAITING)
	{
		if (consoleplayer == Net_Arbitrator && client != consoleplayer)
		{
			// Someone might've missed the previous packet, so resend it just in case.
			NetBuffer[0] = NCMD_LEVELREADY;
			NetBuffer[1] = CurrentLobbyID;
			NetBuffer[2] = 0;
			NetBuffer[3] = 0;

			HSendPacket(client, 4);
		}

		return;
	}

	if (client == Net_Arbitrator)
	{
		LevelStartAck = 0u;
		LevelStartStatus = consoleplayer == Net_Arbitrator ? LST_HOST : LST_READY;
		LevelStartDelay = LevelStartDebug = delayTics;
		LastGameUpdate = EnterTic;
		return;
	}

	uint64_t mask = 0u;
	for (auto pNum : NetworkClients)
	{
		if (pNum != Net_Arbitrator)
			mask |= (uint64_t)1u << pNum;
	}

	LevelStartAck |= (uint64_t)1u << client;
	if ((LevelStartAck & mask) == mask && IsMapLoaded())
	{
		// Beyond this point a player is likely lagging out anyway.
		constexpr uint16_t LatencyCap = 350u;

		NetBuffer[0] = NCMD_LEVELREADY;
		NetBuffer[1] = CurrentLobbyID;
		uint16_t highestAvg = 0u;
		// Wait for enough latency info to be accepted so a better average
		// can be calculated for everyone.
		if (FullLatencyCycle > 0)
			return;

		for (auto client : NetworkClients)
		{
			if (client == Net_Arbitrator)
				continue;

			const uint16_t latency = min<uint16_t>(ClientStates[client].AverageLatency, LatencyCap);
			if (latency > highestAvg)
				highestAvg = latency;
		}

		constexpr double MS2Sec = 1.0 / 1000.0;
		for (auto client : NetworkClients)
		{
			int delay = 0;
			if (client != Net_Arbitrator)
				delay = int(floor((highestAvg - min<uint16_t>(ClientStates[client].AverageLatency, LatencyCap)) * MS2Sec * TICRATE));

			NetBuffer[2] = (delay << 8);
			NetBuffer[3] = delay;

			HSendPacket(client, 4);
		}
	}
}

struct FLatencyAck
{
	int Client;
	uint8_t Seq;

	FLatencyAck(int client, uint8_t seq) : Client(client), Seq(seq) {}
};

//
// GetPackets
//
static void GetPackets()
{
	TArray<FLatencyAck> latencyAcks = {};
	while (HGetPacket())
	{
		const int clientNum = RemoteClient;

		// Unknown client with NCMD_SETUP — handle mid-game join then skip.
		if (clientNum < 0)
		{
			if (NetBuffer[0] & NCMD_SETUP)
				HandleIncomingConnection();
			continue;
		}

		auto& clientState = ClientStates[clientNum];

		// Track last valid packet time for heartbeat timeout detection.
		clientState.LastPacketReceivedTime = I_msTime();

		if (NetBuffer[0] & NCMD_EXIT)
		{
			ClientQuit(clientNum, clientNum == Net_Arbitrator ? NetBuffer[1] : -1);
			continue;
		}

		if (NetBuffer[0] & NCMD_SETUP)
		{
			HandleIncomingConnection();
			continue;
		}

		if (NetBuffer[0] & NCMD_LATENCY)
		{
			size_t i = 0u;
			for (; i < latencyAcks.Size(); ++i)
			{
				if (latencyAcks[i].Client == clientNum)
					break;
			}

			if (i >= latencyAcks.Size())
				latencyAcks.Push({ clientNum, NetBuffer[1] });

			continue;
		}

		if (NetBuffer[0] & NCMD_LATENCYACK)
		{
			if (NetBuffer[1] == clientState.CurrentLatency)
			{
				clientState.RecvTime[clientState.CurrentLatency++ % MAXSENDTICS] = I_msTime();
				clientState.bNewLatency = true;
			}

			continue;
		}

		if (NetBuffer[0] & NCMD_LEVELREADY)
		{
			if (NetBuffer[1] == CurrentLobbyID)
			{
				int delay = 0;
				if (clientNum == Net_Arbitrator)
					delay = (NetBuffer[2] << 8) | NetBuffer[3];

				CheckLevelStart(clientNum, delay);
			}

			continue;
		}

		if (NetBuffer[0] & NCMD_RETRANSMIT)
		{
			clientState.ResendID = NetBuffer[1];
			clientState.Flags |= CF_RETRANSMIT;
		}

		const bool validID = NetBuffer[1] == CurrentLobbyID;
		if (validID)
		{
			clientState.Flags |= CF_UPDATED;
			clientState.SequenceAck = (NetBuffer[2] << 24) | (NetBuffer[3] << 16) | (NetBuffer[4] << 8) | NetBuffer[5];
		}

		const int consistencyAck = (NetBuffer[6] << 24) | (NetBuffer[7] << 16) | (NetBuffer[8] << 8) | NetBuffer[9];

		int curByte = 10;
		if (NetBuffer[0] & NCMD_QUITTERS)
		{
			int numPlayers = NetBuffer[curByte++];
			for (int i = 0; i < numPlayers; ++i)
			{
				int quitNum = NetBuffer[curByte++];
				if (quitNum >= (int)MAXPLAYERS)
					continue;
				// Network bookkeeping only — remove from the client list and
				// clear tracking masks so the lockstep doesn't stall. The
				// actual game state change (PST_GONE, actor destruction) is
				// deferred to DEM_PLAYERDISCONNECT so it happens at the same
				// gametic on all nodes, preventing desync.
				NetworkClients -= quitNum;
				const uint64_t mask = ~((uint64_t)1u << quitNum);
				MutedClients &= mask;
				CutsceneReady &= mask;
				LevelStartAck &= mask;
				I_ClearClient(quitNum);
			}
		}

		const int playerCount = NetBuffer[curByte++];

		int baseSequence = -1;
		const int totalTics = NetBuffer[curByte++];
		if (totalTics > 0)
		{
			int b0 = NetBuffer[curByte++];
			int b1 = NetBuffer[curByte++];
			int b2 = NetBuffer[curByte++];
			int b3 = NetBuffer[curByte++];
			baseSequence = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
		}

		int baseConsistency = -1;
		const int ranTics = NetBuffer[curByte++];
		if (ranTics > 0)
		{
			int b0 = NetBuffer[curByte++];
			int b1 = NetBuffer[curByte++];
			int b2 = NetBuffer[curByte++];
			int b3 = NetBuffer[curByte++];
			baseConsistency = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
		}

		if (validID)
		{
			if (clientNum == Net_Arbitrator)
				CommandsAhead = NetBuffer[curByte];
			else if (consoleplayer == Net_Arbitrator)
				clientState.StabilityBuffer = NetBuffer[curByte];
		}
		++curByte;
		
		for (int p = 0; p < playerCount; ++p)
		{
			const int pNum = NetBuffer[curByte++];
			auto& pState = ClientStates[pNum];

			// This gets sent over per-player so latencies are correctly displayed.
			if (clientNum == Net_Arbitrator)
			{
				if (consoleplayer != Net_Arbitrator)
				{
					int hi = NetBuffer[curByte++];
					int lo = NetBuffer[curByte++];
					pState.AverageLatency = (hi << 8) | lo;
				}
				else
					curByte += 2;
			}

			// Make sure the host doesn't update a player's last consistency ack with their own data.
			if (consoleplayer != Net_Arbitrator
				|| pNum == Net_Arbitrator || clientNum != Net_Arbitrator)
			{
				pState.ConsistencyAck = consistencyAck;
			}

			TArray<int16_t> consistencies = {};
			for (int r = 0; r < ranTics; ++r)
			{
				int ofs = NetBuffer[curByte++];
				int hi = NetBuffer[curByte++];
				int lo = NetBuffer[curByte++];
				consistencies.Insert(ofs, (hi << 8) | lo);
			}

			for (size_t i = 0u; i < consistencies.Size(); ++i)
			{
				const int cTic = baseConsistency + int(i);
				if (cTic <= pState.CurrentNetConsistency)
					continue;

				if (cTic > pState.CurrentNetConsistency + 1 || !consistencies[i])
				{
					clientState.Flags |= CF_MISSING_CON;
					break;
				}

				pState.NetConsistency[cTic % BACKUPTICS] = consistencies[i];
				pState.CurrentNetConsistency = cTic;
			}

			// Each tic within a given packet is given a sequence number to ensure that things were put
			// back together correctly. Normally this wouldn't matter as much but since we need to keep
			// clients in lock step a misordered packet will instantly cause a desync.
			TArray<TArrayView<uint8_t>> data = {}; // each contained TArrayView represents a packet.
			for (int t = 0; t < totalTics; ++t)
			{
				// Try and reorder the tics if they're all there but end up out of order.
				const int ofs = NetBuffer[curByte++];

				TArrayView<uint8_t> skipper = TArrayView(&NetBuffer[curByte], MAX_MSGLEN - curByte);
				SkipUserCmdMessage(skipper);

				TArrayView<uint8_t> packet = TArrayView(&NetBuffer[curByte], skipper.Data() - &NetBuffer[curByte]);
				data.Insert(ofs, packet);

				curByte += skipper.Data() - &NetBuffer[curByte];
			}

			// If it's from a previous waiting period, the commands are no longer relevant.
			if (!validID)
				continue;

			for (size_t i = 0u; i < data.Size(); ++i)
			{
				const int seq = baseSequence + int(i);
				// Duplicate command, ignore it.
				if (seq <= pState.CurrentSequence)
					continue;

				// Skipped a command. Packet likely got corrupted while being put back together, so have
				// the client send over the properly ordered commands. 
				if (seq > pState.CurrentSequence + 1 || data[i] == nullptr)
				{
					clientState.Flags |= CF_MISSING_SEQ;
					break;
				}

				ReadUserCmdMessage(data[i], pNum, seq);
				// The host and clients are a bit desynced here. We don't want to update the host's latest ack with their own
				// info since they get those from the actual clients, but clients have to get them from the host since they
				// don't commincate with each other.
				if (consoleplayer != Net_Arbitrator
					|| pNum == Net_Arbitrator || clientNum != Net_Arbitrator)
				{
					pState.CurrentSequence = seq;
				}
				// A late joiner may miss the side-channel STATE_ACTIVE packet on
				// heavy maps, but once the host receives the joiner's first
				// post-spawn command stream we know the client is fully active.
				if (consoleplayer == Net_Arbitrator
					&& clientNum == pNum
					&& (clientState.Flags & CF_AWAITING_ACTIVE))
				{
					ConfirmMidgameJoinActive(clientNum);
				}
				// Update this so host switching doesn't have any hiccups.
				if (consoleplayer != Net_Arbitrator && pNum != Net_Arbitrator)
					pState.SequenceAck = seq;
			}
		}
	}

	for (const auto& ack : latencyAcks)
	{
		NetBuffer[0] = NCMD_LATENCYACK;
		NetBuffer[1] = ack.Seq;
		HSendPacket(ack.Client, 2);
	}
}

static void SendHeartbeat()
{
	// TODO: This could probably also be used to determine if there's packets
	// missing and a retransmission is needed.
	const uint64_t time = I_msTime();
	for (auto client : NetworkClients)
	{
		if (client == consoleplayer)
			continue;

		auto& state = ClientStates[client];
		if (LastLatencyUpdate >= MAXSENDTICS)
		{
			int delta = 0;
			const uint8_t startTic = state.CurrentLatency - MAXSENDTICS;
			for (int i = 0; i < MAXSENDTICS; ++i)
			{
				const int tic = (startTic + i) % MAXSENDTICS;
				const uint64_t high = state.RecvTime[tic] < state.SentTime[tic] ? time : state.RecvTime[tic];
				delta += high - state.SentTime[tic];
			}

			state.AverageLatency = delta / MAXSENDTICS;
		}

		if (state.bNewLatency)
		{
			// Use the most up-to-date time here for better accuracy.
			state.SentTime[state.CurrentLatency % MAXSENDTICS] = I_msTime();
			state.bNewLatency = false;
		}

		NetBuffer[0] = NCMD_LATENCY;
		NetBuffer[1] = state.CurrentLatency;
		HSendPacket(client, 2);
	}
}

static void CheckConsistencies()
{
	// Check consistencies retroactively to see if there was a desync at some point. We still
	// check the local client here because these could realistically desync
	// if the client's current position doesn't agree with the host.
	for (auto client : NetworkClients)
	{
		if (!playeringame[client] || IsDedicatedControlStream(client))
			continue;

		auto& clientState = ClientStates[client];
		// If previously inconsistent, always mark it as such going forward. We don't want this to
		// accidentally go away at some point since the game state is already completely broken.
		if (players[client].inconsistant)
		{
			clientState.LastVerifiedConsistency = clientState.CurrentNetConsistency;
		}
		else
		{
			// Don't check tics whose LocalConsistency buffer slot has been
			// overwritten by a newer tic (circular buffer wraparound).
			if (CurrentConsistency - clientState.LastVerifiedConsistency >= BACKUPTICS)
				clientState.LastVerifiedConsistency = CurrentConsistency - BACKUPTICS;

			// Make sure we don't check past tics we haven't even ran yet.
			const int limit = min<int>(CurrentConsistency - 1, clientState.CurrentNetConsistency);
			while (clientState.LastVerifiedConsistency < limit)
			{
				++clientState.LastVerifiedConsistency;
				const int tic = clientState.LastVerifiedConsistency % BACKUPTICS;
				if (clientState.LocalConsistency[tic] != clientState.NetConsistency[tic])
				{
					players[client].inconsistant = true;
					clientState.LastVerifiedConsistency = clientState.CurrentNetConsistency;
					break;
				}
			}
		}
	}
}

//==========================================================================
//
// FRandom :: StaticSumSeeds
//
// This function produces a uint32_t that can be used to check the consistancy
// of network games between different machines. Only a select few RNGs are
// used for the sum, because not all RNGs are important to network sync.
//
//==========================================================================

extern FRandom pr_spawnmobj;
extern FRandom pr_acs;
extern FRandom pr_chase;
extern FRandom pr_damagemobj;

static uint32_t StaticSumSeeds()
{
	return FRandom::StaticSumAllSeeds();
}

static int16_t CalculateConsistency(int client, uint32_t seed)
{
	if (IsDedicatedControlStream(client))
		return 1;

	if (players[client].mo != nullptr)
	{
		seed += int((players[client].mo->X() + players[client].mo->Y() + players[client].mo->Z()) * 257) + players[client].mo->Angles.Yaw.BAMs() + players[client].mo->Angles.Pitch.BAMs();
		seed ^= players[client].health;
	}

	// Zero value consistencies are seen as invalid, so always have a valid value.
	return (seed & 0xFFFF) ? seed : 1;
}

// Ran a tick, so prep the next consistencies to send out.
// [RH] Include some random seeds and player stuff in the consistancy
// check, not just the player's x position like BOOM.
static void MakeConsistencies()
{
	if (!netgame || demoplayback || (gametic % TicDup) || !IsMapLoaded())
		return;

	const uint32_t rngSum = StaticSumSeeds();

	for (auto client : NetworkClients)
	{
		auto& clientState = ClientStates[client];
		if (!playeringame[client])
		{
			if (clientState.Flags & (CF_JOINING | CF_AWAITING_STATE | CF_AWAITING_ACTIVE))
			{
				clientState.LocalConsistency[CurrentConsistency % BACKUPTICS] =
					CalculatePendingJoinConsistency(client, rngSum);
			}
			continue;
		}
		clientState.LocalConsistency[CurrentConsistency % BACKUPTICS] = CalculateConsistency(client, rngSum);
	}

	++CurrentConsistency;
}

static bool Net_UpdateStatus()
{
	if (!netgame || demoplayback || NetworkClients.Size() <= 1)
		return true;

	if (LevelStartStatus == LST_WAITING || LevelStartDelay > 0)
		return false;

	// Check against the previous tick in case we're recovering from a huge
	// system hiccup. If the game has taken too long to update, it's likely
	// another client is hanging up the game.
	if (LastEnterTic - LastGameUpdate >= MAXSENDTICS * TicDup)
	{
		// Try again in the next MaxDelay tics.
		LastGameUpdate = EnterTic;

		if (consoleplayer == Net_Arbitrator)
		{
			// Use a missing packet here to tell the other players to retransmit instead of simply retransmitting our
			// own data over instantly. This avoids flooding the network at a time where it's not opportune to do so.
			const int curTic = gametic / TicDup;
			for (auto client : NetworkClients)
			{
				if (client == consoleplayer)
					continue;

				if (ClientStates[client].CurrentSequence < curTic)
				{
					ClientStates[client].Flags |= CF_MISSING;
					players[client].waiting = true;
				}
				else
				{
					players[client].waiting = false;
				}
			}
		}
		else
		{
			// The client is waiting for data from the host and hasn't recieved it yet. Send
			// our data back over in case the host is waiting for us.
			ClientStates[Net_Arbitrator].Flags |= CF_MISSING;
			players[Net_Arbitrator].waiting = true;
		}
	}

	if (LevelStartStatus == LST_HOST)
		return false;

	for (auto client : NetworkClients)
	{
		if (players[client].waiting)
			return false;
	}

	// Wait for the game to stabilize a bit after launch before skipping commands.
	bool updated = false;
	int lowestDiff = INT_MAX;
	if (gametic > TICRATE * 2 && !(gametic % TicDup))
	{
		if (consoleplayer == Net_Arbitrator)
		{
			// If we're consistenty ahead of the highest sequence player, slow down.
			bool allUpdated = true;
			const int curTic = ClientTic / TicDup;
			for (auto client : NetworkClients)
			{
				if (client != Net_Arbitrator)
				{
					if (ClientStates[client].Flags & CF_UPDATED)
					{
						updated = true;
						int diff = curTic - ClientStates[client].CurrentSequence;
						if (diff < lowestDiff)
							lowestDiff = diff;
					}
					else
					{
						allUpdated = false;
					}
				}

				ClientStates[client].Flags &= ~CF_UPDATED;
			}

			if (allUpdated)
			{
				// If we're consistently ahead of the world, force a stop here as well. Likely some client
				// has fallen super far behind and needs to be reset.
				const int diff = curTic - gametic / TicDup;
				if (diff > 1)
					lowestDiff = diff;
			}
		}
		else if (ClientStates[Net_Arbitrator].Flags & CF_UPDATED)
		{
			// Check if the host is reporting that we're too far ahead of them.
			updated = true;
			lowestDiff = CommandsAhead;
			ClientStates[Net_Arbitrator].Flags &= ~CF_UPDATED;
		}
	}

	if (updated)
	{
		lowestDiff -= StabilityBuffer;
		if (lowestDiff > 0)
		{
			if (SkipCommandTimer++ > TICRATE / 2)
			{
				SkipCommandTimer = 0;
				if (SkipCommandAmount <= 0)
					SkipCommandAmount = lowestDiff * TicDup;
			}
		}
		else
		{
			SkipCommandTimer = 0;
		}
	}

	return true;
}

void NetUpdate(int tics)
{
	GetPackets();

	// State transfer processing runs regardless of tics — the joining client
	// may be lockstep-blocked (tics=0) while receiving chunks, and the host
	// needs to retransmit/timeout even between tics.
	if (netgame && !demoplayback)
		TickStateTransfer();

	if (tics <= 0)
		return;

	if (netgame && !demoplayback)
	{
		// If a tic has passed, always send out a heartbeat packet (also doubles as
		// a latency measurement tool).
		if (consoleplayer == Net_Arbitrator)
		{
			LastLatencyUpdate += tics;
			if (FullLatencyCycle > 0)
				FullLatencyCycle = max<int>(FullLatencyCycle - tics, 0);

			SendHeartbeat();

			if (LastLatencyUpdate >= MAXSENDTICS)
				LastLatencyUpdate = 0;
		}

		CheckConsistencies();

		// Heartbeat timeout: if the host hasn't heard from a client in CLIENT_TIMEOUT_MS,
		// treat them as disconnected (crashed or lost connection).
		if (consoleplayer == Net_Arbitrator)
		{
			const uint64_t now = I_msTime();
			for (auto client : NetworkClients)
			{
				if (client == consoleplayer)
					continue;

				auto& state = ClientStates[client];
				// Don't timeout joining clients via heartbeat — they send setup
				// packets (chunk ACKs) that don't update LastPacketReceivedTime.
				// TickStateTransfer() handles timeout for active transfers instead.
				if (state.Flags & (CF_JOINING | CF_AWAITING_STATE))
				{
					// Timeout joining clients if they've been waiting too long
					// without transfer starting (e.g., STATE_READY lost).
					// Active transfers are timed out by TickStateTransfer() instead.
					if ((state.Flags & CF_AWAITING_STATE)
						&& !PendingStateTransfer.active
						&& state.LastPacketReceivedTime > 0
						&& (now - state.LastPacketReceivedTime) >= STATE_TRANSFER_TOTAL_TIMEOUT_MS)
					{
						Printf("Joining client %d timed out (state transfer never started)\n", client);
						ClientStates[client].Flags &= ~(CF_JOINING | CF_AWAITING_STATE | CF_AWAITING_ACTIVE);
						DisconnectClient(client);
					}
					continue;
				}
				if (state.LastPacketReceivedTime > 0 && (now - state.LastPacketReceivedTime) >= CLIENT_TIMEOUT_MS)
				{
					Printf("Client %d timed out (no packets for %llums)\n", client, (unsigned long long)(now - state.LastPacketReceivedTime));
					InitiatePlayerDisconnect(client);
					state.LastPacketReceivedTime = 0; // Prevent repeated timeout messages.
				}
			}
		}
	}

	// Sit idle after the level has loaded until everyone is ready to go. This keeps players better
	// in sync with each other than relying on tic balancing to speed up/slow down the game and mirrors
	// how players would wait for a true server to load.
	if (LevelStartStatus != LST_READY)
	{
		if (LevelStartStatus == LST_WAITING)
		{
			if (NetworkClients.Size() == 1)
			{
				// If we got stuck in limbo waiting, force start the map.
				CheckLevelStart(Net_Arbitrator, 0);
			}
			else
			{
				if (consoleplayer != Net_Arbitrator && IsMapLoaded())
				{
					NetBuffer[0] = NCMD_LEVELREADY;
					NetBuffer[1] = CurrentLobbyID;
					HSendPacket(Net_Arbitrator, 2);
				}
			}
		}
		else if (LevelStartStatus == LST_HOST)
		{
			// If we're the host, idly wait until all packets have arrived. There's no point in predicting since we
			// know for a fact the game won't be started until everyone is accounted for.
			const int curTic = gametic / TicDup;
			int lowestSeq = curTic;
			for (auto client : NetworkClients)
			{
				if (client != Net_Arbitrator && ClientStates[client].CurrentSequence < lowestSeq)
					lowestSeq = ClientStates[client].CurrentSequence;
			}

			if (lowestSeq >= curTic)
				LevelStartStatus = LST_READY;
		}
	}
	else if (LevelStartDelay > 0)
	{
		if (LevelStartDelay < tics)
			tics -= LevelStartDelay;

		LevelStartDelay = max<int>(LevelStartDelay - tics, 0);
	}
		
	bool netGood = Net_UpdateStatus();
	const int startTic = ClientTic;
	tics = min<int>(tics, MAXSENDTICS * TicDup);
	if ((startTic + tics - gametic) / TicDup > BACKUPTICS / 2)
	{
		tics = (gametic + BACKUPTICS / 2 * TicDup) - startTic;
		if (tics <= 0)
		{
			tics = 1;
			netGood = false;
		}
	}

	for (int i = 0; i < tics; ++i)
	{
		if (!dedicatedServer)
		{
			I_StartTic();
			D_ProcessEvents();
		}
		if (pauseext || !netGood)
			break;

		if (SkipCommandAmount > 0)
		{
			--SkipCommandAmount;
			continue;
		}

			if (dedicatedServer)
			{
				// Dedicated server generates empty commands — no input, no actor.
				memset(&LocalCmds[ClientTic++ % LOCALCMDTICS], 0, sizeof(usercmd_t));
			}
			else if (ShouldSuppressMidgameJoinLocalCommands())
			{
				// Keep consuming remote tics so DEM_MIDGAMESPAWN and
				// DEM_MIDGAMEACTIVE can execute, but do not generate or send local
				// gameplay commands before the joiner is officially activated.
				continue;
			}
			else
			{
				const int localTic = ClientTic++;
				auto& built = LocalCmds[localTic % LOCALCMDTICS];
				G_BuildTiccmd(&built);
			}
		if (TicDup == 1)
		{
			Net_NewClientTic();
		}
		else
		{
			const int ticDiff = ClientTic % TicDup;
			if (ticDiff)
			{
				const int startTic = ClientTic - ticDiff;

				// Even if we're not sending out inputs, update the local commands so that the TicDup
				// is correctly played back while predicting as best as possible. This will help prevent
				// minor hitches when playing online.
				for (int j = ClientTic - 1; j > startTic; --j)
					LocalCmds[(j - 1) % LOCALCMDTICS].buttons |= LocalCmds[j % LOCALCMDTICS].buttons;
			}
			else
			{
				// Gather up the Command across the last TicDup number of tics
				// and average them out. These are propagated back to the local
				// command so that they'll be predicted correctly.
				const int lastTic = ClientTic - TicDup;
				for (int j = ClientTic - 1; j > lastTic; --j)
					LocalCmds[(j - 1) % LOCALCMDTICS].buttons |= LocalCmds[j % LOCALCMDTICS].buttons;

				int pitch = 0;
				int yaw = 0;
				int roll = 0;
				int forwardmove = 0;
				int sidemove = 0;
				int upmove = 0;

				for (int j = 0; j < TicDup; ++j)
				{
					const int mod = (lastTic + j) % LOCALCMDTICS;
					pitch += LocalCmds[mod].pitch;
					yaw += LocalCmds[mod].yaw;
					roll += LocalCmds[mod].roll;
					forwardmove += LocalCmds[mod].forwardmove;
					sidemove += LocalCmds[mod].sidemove;
					upmove += LocalCmds[mod].upmove;
				}

				pitch /= TicDup;
				yaw /= TicDup;
				roll /= TicDup;
				forwardmove /= TicDup;
				sidemove /= TicDup;
				upmove /= TicDup;

				for (int j = 0; j < TicDup; ++j)
				{
					const int mod = (lastTic + j) % LOCALCMDTICS;
					LocalCmds[mod].pitch = pitch;
					LocalCmds[mod].yaw = yaw;
					LocalCmds[mod].roll = roll;
					LocalCmds[mod].forwardmove = forwardmove;
					LocalCmds[mod].sidemove = sidemove;
					LocalCmds[mod].upmove = upmove;
				}

				Net_NewClientTic();
			}
		}
	}

	const int newestTic = ClientTic / TicDup;
	if (demoplayback)
	{
		// Don't touch net command data while playing a demo, as it'll already exist.
		for (auto client : NetworkClients)
			ClientStates[client].CurrentSequence = newestTic;

		return;
	}

	// While waiting for DEM_MIDGAMESPAWN, the late joiner should only
	// receive host data. Sending normal gameplay packets here just echoes the
	// host stream back and confuses retransmit bookkeeping.
	if (ShouldSuppressMidgameJoinLocalCommands())
	{
		GetPackets();
		return;
	}

	constexpr int MaxPlayersPerPacket = 16;

	int startSequence = startTic / TicDup;
	int endSequence = newestTic;
	int quitters = 0;
	int quitNums[MAXPLAYERS];
	unsigned players = 1u;
	int maxCommands = MAXSENDTICS;
	if (consoleplayer == Net_Arbitrator)
	{
		// Ensure the host only sends out available tics when ready instead of constantly shotgunning
		// them out as they're made locally.
		startSequence = gametic / TicDup;
		int lowestSeq = endSequence - 1;
		for (auto client : NetworkClients)
		{
			if (client == Net_Arbitrator)
				continue;

			if (ClientStates[client].Flags & CF_QUIT)
			{
				quitNums[quitters++] = client;
			}
			else if (ClientStates[client].Flags & (CF_JOINING | CF_AWAITING_STATE))
			{
				// Skip joining clients — they don't participate in lockstep yet.
			}
			else
			{
				++players;
				if (ClientStates[client].CurrentSequence < lowestSeq)
					lowestSeq = ClientStates[client].CurrentSequence;
			}
		}

		endSequence = lowestSeq + 1;

		// To avoid fragmenting, split up commands into groups of 16p with only 2 commands per packet.
		// If the average packet size with 16p is ~500b, this gives up to ~1000b per packet of data
		// with some leeway for network events and UDP header data. Most routers have an MTU of 1500b.
		// If player count is < 16, scale the number of commands by 1 per every 4 less players.
		// If player count is < 8, scale the number of commands by 1 per every 1 less player.
		// If player count is < 4, scale the number of commands by 4 per every 1 less player.
		constexpr size_t MaxTicsPerPacket = 2u;
		if (players > 1u)
		{
			maxCommands = MaxTicsPerPacket;
			if (players >= MaxPlayersPerPacket / 2 && players < MaxPlayersPerPacket)
				maxCommands = MaxTicsPerPacket + (MaxPlayersPerPacket - players) / 4;
			else if (players >= MaxPlayersPerPacket / 4 && players < MaxPlayersPerPacket / 2)
				maxCommands = MaxPlayersPerPacket / 4 + MaxPlayersPerPacket / 2 - players;
			else if (players < MaxPlayersPerPacket / 4)
				maxCommands = MaxPlayersPerPacket / 2 + (MaxPlayersPerPacket / 4 - players) * 4;
		}
	}

	const bool resendOnly = startSequence == endSequence && (ClientTic % TicDup);
	const int playerLoops = static_cast<int>(ceil((double)players / MaxPlayersPerPacket));
	for (auto client : NetworkClients)
	{
		// We don't want to send information to anyone but the host. On the other
		// hand, if we're the host we send out everyone's info to everyone else.
		if (consoleplayer != Net_Arbitrator && client != Net_Arbitrator)
			continue;

		auto& curState = ClientStates[client];
		// If we can only resend, don't send clients any information that they already have. If
		// we couldn't generate any commands because we're at the cap, instead send out a heartbeat.
		if ((curState.Flags & CF_QUIT) || (resendOnly && !(curState.Flags & (CF_RETRANSMIT | CF_MISSING))))
			continue;

		const bool isSelf = client == consoleplayer;
		NetBuffer[0] = (curState.Flags & CF_MISSING) ? NCMD_RETRANSMIT : 0;
		curState.Flags &= ~CF_MISSING;

		NetBuffer[1] = (curState.Flags & CF_RETRANSMIT_SEQ) ? curState.ResendID : CurrentLobbyID;
		int lastSeq = curState.CurrentSequence;
		int lastCon = curState.CurrentNetConsistency;
		if (consoleplayer != Net_Arbitrator)
		{
			// Make sure to get the lowest sequence of all players
			// since the host themselves might have gotten updated but someone else in the packet
			// did not. That way the host knows to send over the correct tic.
			for (auto cl : NetworkClients)
			{
				if (ClientStates[cl].CurrentSequence < lastSeq)
					lastSeq = ClientStates[cl].CurrentSequence;
				if (ClientStates[cl].CurrentNetConsistency < lastCon)
					lastCon = ClientStates[cl].CurrentNetConsistency;
			}
		}
		// Last sequence we got from this client.
		NetBuffer[2] = (lastSeq >> 24);
		NetBuffer[3] = (lastSeq >> 16);
		NetBuffer[4] = (lastSeq >> 8);
		NetBuffer[5] = lastSeq;
		// Last consistency we got from this client.
		NetBuffer[6] = (lastCon >> 24);
		NetBuffer[7] = (lastCon >> 16);
		NetBuffer[8] = (lastCon >> 8);
		NetBuffer[9] = lastCon;

		if (curState.Flags & CF_RETRANSMIT_SEQ)
		{
			curState.Flags &= ~CF_RETRANSMIT_SEQ;
			if (curState.ResendSequenceFrom < 0)
				curState.ResendSequenceFrom = curState.SequenceAck + 1;
		}

		const int sequenceNum = curState.ResendSequenceFrom >= 0 ? curState.ResendSequenceFrom : startSequence;
		const int numTics = clamp<int>(endSequence - sequenceNum, 0, MAXSENDTICS);

		if (curState.Flags & CF_RETRANSMIT_CON)
		{
			curState.Flags &= ~CF_RETRANSMIT_CON;
			if (curState.ResendConsistencyFrom < 0)
				curState.ResendConsistencyFrom = curState.ConsistencyAck + 1;
		}

		const int baseConsistency = curState.ResendConsistencyFrom >= 0 ? curState.ResendConsistencyFrom : LastSentConsistency;
		// Don't bother sending over consistencies unless you're the host.
		// Cap to BACKUPTICS so we never read overwritten circular buffer
		// slots. The old MAXSENDTICS cap caused LastSentConsistency to jump
		// past unsent values, creating permanent gaps that led to false
		// desyncs on both the host (loopback) and remote clients.
		int ran = 0;
		if (consoleplayer == Net_Arbitrator)
			ran = clamp<int>(CurrentConsistency - baseConsistency, 0, BACKUPTICS);

		int ticLoops = static_cast<int>(ceil(max<double>(numTics, ran) / maxCommands));
		if (isSelf || !ticLoops)
			ticLoops = 1;

		const int maxPlayerLoops = isSelf ? 1 : playerLoops;
		int totalQuits = quitters;
		for (int tLoops = 0, curTicOfs = 0; tLoops < ticLoops; ++tLoops, curTicOfs += maxCommands)
		{
			for (int pLoops = 0, curPlayerOfs = 0; pLoops < maxPlayerLoops; ++pLoops, curPlayerOfs += MaxPlayersPerPacket)
			{
				size_t size = 10;
				if (totalQuits > 0)
				{
					NetBuffer[0] |= NCMD_QUITTERS;
					NetBuffer[size++] = totalQuits;
					for (int i = 0; i < totalQuits; ++i)
						NetBuffer[size++] = quitNums[i];

					totalQuits = 0;
				}
				else
				{
					NetBuffer[0] &= ~NCMD_QUITTERS;
				}

				int playerNums[MAXPLAYERS];
				int playerCount = isSelf ? players : min<int>(players - curPlayerOfs, MaxPlayersPerPacket);
				NetBuffer[size++] = playerCount;
				if (players > 1)
				{
					int i = 0;
					for (auto cl : NetworkClients)
					{
						if (ClientStates[cl].Flags & (CF_QUIT | CF_JOINING | CF_AWAITING_STATE))
							continue;

						if (i >= curPlayerOfs)
							playerNums[i - curPlayerOfs] = cl;

						++i;
						if (!isSelf && i >= curPlayerOfs + MaxPlayersPerPacket)
							break;
					}
				}
				else
				{
					playerNums[0] = consoleplayer;
				}

				int sendTics = isSelf ? numTics : clamp<int>(numTics - curTicOfs, 0, maxCommands);
				if (curState.ResendSequenceFrom >= 0)
				{
					curState.ResendSequenceFrom += sendTics;
					if (curState.ResendSequenceFrom >= endSequence)
						curState.ResendSequenceFrom = -1;
				}
				NetBuffer[size++] = sendTics;
				if (sendTics > 0)
				{
					NetBuffer[size++] = (sequenceNum + curTicOfs) >> 24;
					NetBuffer[size++] = (sequenceNum + curTicOfs) >> 16;
					NetBuffer[size++] = (sequenceNum + curTicOfs) >> 8;
					NetBuffer[size++] = sequenceNum + curTicOfs;
				}

				int sendCon = isSelf ? ran : clamp<int>(ran - curTicOfs, 0, maxCommands);
				if (curState.ResendConsistencyFrom >= 0)
				{
					curState.ResendConsistencyFrom += sendCon;
					if (curState.ResendConsistencyFrom >= CurrentConsistency)
						curState.ResendConsistencyFrom = -1;
				}

				NetBuffer[size++] = sendCon;
				if (sendCon > 0)
				{
					NetBuffer[size++] = (baseConsistency + curTicOfs) >> 24;
					NetBuffer[size++] = (baseConsistency + curTicOfs) >> 16;
					NetBuffer[size++] = (baseConsistency + curTicOfs) >> 8;
					NetBuffer[size++] = baseConsistency + curTicOfs;
				}

				if (consoleplayer == Net_Arbitrator)
					NetBuffer[size++] = client == Net_Arbitrator ? 0 : max<int>(curState.CurrentSequence + curState.StabilityBuffer - newestTic, 0);
				else
					NetBuffer[size++] = max<int>(StabilityBuffer, 0);

				// Client commands.

				TArrayView<uint8_t> cmd = TArrayView(&NetBuffer[size], MAX_MSGLEN - size);
				for (int i = 0; i < playerCount; ++i)
				{
					WriteInt8(playerNums[i], cmd);

					auto& clientState = ClientStates[playerNums[i]];
					// Measured latency from client to host.
					if (consoleplayer == Net_Arbitrator)
					{
						WriteInt16(clientState.AverageLatency, cmd);
					}

					for (int r = 0; r < sendCon; ++r)
					{
						WriteInt8(r, cmd);
						const int tic = (baseConsistency + curTicOfs + r) % BACKUPTICS;
						WriteInt16(clientState.LocalConsistency[tic], cmd);
					}

					for (int t = 0; t < sendTics; ++t)
					{
						WriteInt8(t, cmd);

						int curTic = sequenceNum + curTicOfs + t, lastTic = curTic - 1;
						if (playerNums[i] == consoleplayer)
						{
							int realTic = (curTic * TicDup) % LOCALCMDTICS;
							int realLastTic = (lastTic * TicDup) % LOCALCMDTICS;
							// Write out the net events before the user commands so inputs can
							// be used as a marker for when the given command ends.
							auto& stream = NetEvents.Streams[curTic % BACKUPTICS];
							WriteBytes(TArrayView(stream.Stream, stream.Used), cmd);

							WriteUserCmdMessage(LocalCmds[realTic],
								realLastTic >= 0 ? &LocalCmds[realLastTic] : nullptr, cmd);
						}
						else
						{
							auto& netTic = clientState.Tics[curTic % BACKUPTICS];

							auto data = netTic.Data.GetTArrayView();
							WriteBytes(data, cmd);

							WriteUserCmdMessage(netTic.Command,
								lastTic >= 0 ? &clientState.Tics[lastTic % BACKUPTICS].Command : nullptr, cmd);
						}
					}
				}

				HSendPacket(client, int(cmd.Data() - NetBuffer));
				if (net_extratic && !isSelf)
					HSendPacket(client, int(cmd.Data() - NetBuffer));
			}
		}
	}

	// Update this now that all the packets have been sent out.
	if (!resendOnly)
		LastSentConsistency = CurrentConsistency;

	// Listen for other packets. This has to also come after sending so the player that sent
	// data to themselves gets it immediately (important for singleplayer, otherwise there
	// would always be a one-tic delay).
	GetPackets();
}

// These have to be here since they have game-specific data. Only the data
// from the frontend should be put in these, all backend handling should be
// done in the core files.

size_t Net_SetEngineInfo(uint8_t*& stream)
{
	stream[0] = VER_MAJOR % 256;
	stream[1] = VER_MINOR % 256;
	stream[2] = VER_REVISION % 256;

	// Send over any loaded files to ensure their checksum is correct.
	size_t numWads = 0u;
	size_t bufferIndex = 7u;
	for (int i = 0; i < fileSystem.GetNumWads(); ++i)
	{
		if (fileSystem.IsOptionalResource(i))
			continue;

		++numWads;
		const FString crc = fileSystem.GetResourceHash(i);
		memcpy(&stream[bufferIndex], crc.GetChars(), crc.Len() + 1u);
		bufferIndex += crc.Len() + 1u;
	}

	stream[3] = (numWads >> 24);
	stream[4] = (numWads >> 16);
	stream[5] = (numWads >> 8);
	stream[6] = numWads;

	return bufferIndex;
}

FVerificationError Net_VerifyEngine(uint8_t*& stream, size_t& offset)
{
	FVerificationError error = {};

	TArray<FString> crcs = {};
	TArray<FString> names = {};
	for (int i = 0; i < fileSystem.GetNumWads(); ++i)
	{
		if (!fileSystem.IsOptionalResource(i))
		{
			crcs.Push(fileSystem.GetResourceHash(i));
			names.Push(fileSystem.GetResourceFileName(i));
		}
	}

	const size_t numWads = (stream[3] << 24) | (stream[4] << 16) | (stream[5] << 8) | stream[6];
	if (numWads < crcs.Size())
		error.Error = FVerificationError::VE_FILE_MISSING;
	else if (numWads > crcs.Size())
		error.Error = FVerificationError::VE_FILE_UNKNOWN;

	TArray<size_t> unverified = {};
	for (size_t i = 0u; i < crcs.Size(); ++i)
		unverified.Push(i);

	offset = 7u;
	for (size_t i = 0u; i < numWads; ++i)
	{
		const FString netCrc = (const char*)&stream[offset];
		offset += netCrc.Len() + 1u;
		if (error.Error == FVerificationError::VE_FILE_UNKNOWN)
		{
			if (crcs.Find(netCrc) >= crcs.Size())
				error.UnknownFiles.Push(netCrc);
		}
		else if (crcs[i] != netCrc)
		{
			const size_t c = crcs.Find(netCrc);
			if (c >= crcs.Size())
			{
				error.Error = FVerificationError::VE_FILE_UNKNOWN;
				error.UnknownFiles.Push(netCrc);
			}
			else
			{
				if (error.Error == FVerificationError::VE_NONE)
					error.Error = FVerificationError::VE_FILE_ORDER;
				unverified.Delete(unverified.Find(c));
			}
		}
		else
		{
			unverified.Delete(unverified.Find(i));
		}
	}

	if (error.Error == FVerificationError::VE_FILE_MISSING)
	{
		for (auto i : unverified)
		{
			FixPathSeperator(names[i]);
			auto ar = names[i].Split('/', FString::TOK_SKIPEMPTY);
			error.MissingFiles.Push(ar.Last());
		}
	}
	else if (error.Error == FVerificationError::VE_FILE_ORDER)
	{
		error.ExpectedOrder = crcs;
		// Remove the core and iwad files.
		error.ExpectedOrder.Delete(0);
		error.ExpectedOrder.Delete(0);
	}

	// Intentionally do this last to avoid messing with the above loop.
	if (stream[0] != (VER_MAJOR % 256) || stream[1] != (VER_MINOR % 256) || stream[2] != (VER_REVISION % 256))
	{
		error.Error = FVerificationError::VE_ENGINE;
		error.Major = VER_MAJOR % 256;
		error.Minor = VER_MINOR % 256;
		error.Revision = VER_REVISION % 256;
		error.NetMajor = stream[0];
		error.NetMinor = stream[1];
		error.NetRevision = stream[2];
	}

	return error;
}

void Net_SetupUserInfo()
{
	D_SetupUserInfo();
}

const char* Net_GetClientName(int client, unsigned int charLimit = 0u)
{
	return players[client].userinfo.GetName(charLimit);
}

void Net_SetUserInfo(int client, TArrayView<uint8_t>& stream)
{
	auto str = D_GetUserInfoStrings(client, true);
	WriteFString(str, stream);
}

void Net_ReadUserInfo(int client, TArrayView<uint8_t>& stream)
{
	D_ReadUserInfoStrings(client, stream, false);
}

void Net_SetGameInfo(TArrayView<uint8_t>& stream)
{
	WriteFString(startmap, stream);
	WriteInt32(rngseed, stream);
	C_WriteCVars(stream, CVAR_SERVERINFO, true);

	auto load = Args->CheckValue(FArg_loadgame);
	if (load != nullptr)
	{
		WriteInt8(1, stream);
		WriteString(load, stream);
	}
	else
	{
		WriteInt8(0, stream);
	}

	// Tell clients whether the host is a dedicated server (ghost player 0).
	WriteInt8(dedicatedServer ? 1 : 0, stream);
}


void Net_ReadGameInfo(TArrayView<uint8_t>& stream)
{
	startmap = ReadStringConst(stream);
	rngseed = ReadInt32(stream);
	C_ReadCVars(stream);

	if (ReadInt8(stream))
	{
		auto load = ReadString(stream);
		// Don't override the existing argument in case they need to use
		// a custom savefile name.
		if (!Args->CheckParm(FArg_loadgame))
		{
			Args->AppendArg(FArg_loadgame);
			Args->AppendRawArg(load);
		}
	}

	// Check if the host is a dedicated server — if so, slot 0 is not a real player.
	if (ReadInt8(stream))
		hostIsDedicated = true;

	// Reset this immediately so any further RNG calls the engine has to make will be synced.
	FRandom::StaticClearRandom();
}

// Connects players to each other if needed.
bool D_CheckNetGame()
{
	if (!I_InitNetwork())
		return false;

	if (Args->CheckParm(FArg_extratic))
		net_extratic = true;

	players[Net_Arbitrator].settings_controller = true;
	const uint64_t startTime = I_msTime();
	for (auto client : NetworkClients)
	{
		// Mid-game joiner: skip our own slot — we enter via DEM_MIDGAMEACTIVE later.
		if (gameaction == ga_midgamejoin && client == consoleplayer)
			continue;

		playeringame[client] = true;
		ClientStates[client].LastPacketReceivedTime = startTime;
	}

	if ((unsigned)MaxClients > 1u)
	{
		if (dedicatedServer)
			Printf("Dedicated server hosting %d players\n", MaxClients);
		else
			Printf("Player %d of %d\n", consoleplayer + 1, MaxClients);
	}

	return true;
}

//
// D_QuitNetGame
// Called before quitting to leave a net game
// without hanging the other players
//
void D_QuitNetGame()
{
	if (!netgame || !usergame || consoleplayer == -1 || demoplayback || NetworkClients.Size() == 1)
		return;

	bDisconnecting = true;
	DisconnectTimestamp = I_msTime();
	DisconnectRetries = 0;

	if (dedicatedServer)
	{
		// Dedicated server: no migration, just tell everyone the server is shutting down.
		Printf("Dedicated server shutting down, disconnecting all clients\n");

		// Send disconnect notification (no next host — 0xFF means server is gone).
		uint8_t noNextHost = 0xFF;
		SendSetupPacketToAll(PRE_DISCONNECT_NOTIFY, &noNextHost, 1);

		// Wait briefly for confirmations.
		uint64_t expectedMask = 0;
		for (auto client : NetworkClients)
		{
			if (client != consoleplayer)
				expectedMask |= ((uint64_t)1u << client);
		}

		DisconnectConfirmMask = 0;
		DisconnectTimestamp = I_msTime();
		DisconnectRetries = 0;
		while ((DisconnectConfirmMask & expectedMask) != expectedMask && DisconnectRetries < DISCONNECT_MAX_RETRIES)
		{
			while (HGetPacket())
			{
				if (NetBuffer[0] & NCMD_SETUP)
					HandleIncomingConnection();
			}

			const uint64_t now = I_msTime();
			if (now - DisconnectTimestamp >= (uint64_t)DISCONNECT_TIMEOUT_MS)
			{
				for (auto client : NetworkClients)
				{
					if (client != consoleplayer && !(DisconnectConfirmMask & ((uint64_t)1u << client)))
						SendSetupPacketToClient(client, PRE_DISCONNECT_NOTIFY, &noNextHost, 1);
				}
				DisconnectTimestamp = now;
				DisconnectRetries++;
			}

			I_WaitVBL(1);
		}

		bDisconnecting = false;
		return;
	}

	if (consoleplayer == Net_Arbitrator)
	{
		// Host path: transfer state to the next host, then notify everyone.
		int nextHost = -1;
		for (auto client : NetworkClients)
		{
			if (client != Net_Arbitrator)
			{
				nextHost = client;
				break;
			}
		}

		if (nextHost < 0)
		{
			bDisconnecting = false;
			return;
		}

		// Phase 2: Serialize migration state and send to new host.
		SendSetupPacketToClient(nextHost, PRE_MIGRATION_BEGIN);

		FMigrationState migState = {};
		SerializeMigrationState(migState);
		uint8_t migBuf[2 + sizeof(FMigrationState)];
		migBuf[0] = NCMD_SETUP;
		migBuf[1] = PRE_MIGRATION_STATE;
		memcpy(&migBuf[2], &migState, sizeof(FMigrationState));
		I_SendSetupPacket(nextHost, migBuf, sizeof(migBuf));

		// Wait for the new host to acknowledge the migration state.
		bMigrating = true;
		DisconnectTimestamp = I_msTime();
		DisconnectRetries = 0;
		while (bMigrating && DisconnectRetries < DISCONNECT_MAX_RETRIES)
		{
			while (HGetPacket())
			{
				if (NetBuffer[0] & NCMD_SETUP)
					HandleIncomingConnection();
			}

			const uint64_t now = I_msTime();
			if (now - DisconnectTimestamp >= (uint64_t)DISCONNECT_TIMEOUT_MS)
			{
				I_SendSetupPacket(nextHost, migBuf, sizeof(migBuf));
				DisconnectTimestamp = now;
				DisconnectRetries++;
			}

			I_WaitVBL(1);
		}

		// Phase 1: Notify all clients that the host is leaving.
		uint8_t nextHostByte = static_cast<uint8_t>(nextHost);
		SendSetupPacketToAll(PRE_DISCONNECT_NOTIFY, &nextHostByte, 1);

		// Build expected confirmation mask from all remaining clients.
		uint64_t expectedMask = 0;
		for (auto client : NetworkClients)
		{
			if (client != consoleplayer)
				expectedMask |= ((uint64_t)1u << client);
		}

		DisconnectConfirmMask = 0;
		DisconnectTimestamp = I_msTime();
		DisconnectRetries = 0;
		while ((DisconnectConfirmMask & expectedMask) != expectedMask && DisconnectRetries < DISCONNECT_MAX_RETRIES)
		{
			while (HGetPacket())
			{
				if (NetBuffer[0] & NCMD_SETUP)
					HandleIncomingConnection();
			}

			const uint64_t now = I_msTime();
			if (now - DisconnectTimestamp >= (uint64_t)DISCONNECT_TIMEOUT_MS)
			{
				// Resend to clients that haven't confirmed yet.
				for (auto client : NetworkClients)
				{
					if (client != consoleplayer && !(DisconnectConfirmMask & ((uint64_t)1u << client)))
						SendSetupPacketToClient(client, PRE_DISCONNECT_NOTIFY, &nextHostByte, 1);
				}

				DisconnectTimestamp = now;
				DisconnectRetries++;
			}

			I_WaitVBL(1);
		}
	}
	else
	{
		// Client path: send disconnect request, wait for ACK from host.
		SendDisconnectRequest();
		DisconnectTimestamp = I_msTime();
		DisconnectRetries = 0;

		while (bDisconnecting && DisconnectRetries < DISCONNECT_MAX_RETRIES)
		{
			while (HGetPacket())
			{
				if (NetBuffer[0] & NCMD_SETUP)
					HandleIncomingConnection();
			}

			const uint64_t now = I_msTime();
			if (now - DisconnectTimestamp >= (uint64_t)DISCONNECT_TIMEOUT_MS)
			{
				SendDisconnectRequest();
				DisconnectTimestamp = now;
				DisconnectRetries++;
			}

			I_WaitVBL(1);
		}

		// If the handshake timed out, fall back to the legacy fire-and-forget method.
		if (bDisconnecting)
		{
			DPrintf(DMSG_WARNING, "Disconnect handshake timed out, falling back to legacy\n");
			NetBuffer[0] = NCMD_EXIT;
			for (int i = 0; i < 4; ++i)
			{
				HSendPacket(Net_Arbitrator, 1);
				I_WaitVBL(1);
			}
		}
	}

	bDisconnecting = false;
}

ADD_STAT(network)
{
	FString out = {};
	if (!netgame || demoplayback)
	{
		out.AppendFormat("No network stats available.");
		return out;
	}

	out.AppendFormat("Max players: %d\tTic dup: %d",
		MaxClients,
		TicDup);

	if (net_extratic)
		out.AppendFormat("\tExtra tic enabled");

	out.AppendFormat("\nWorld tic: %06d (sequence %06d)", gametic, gametic / TicDup);
	if (consoleplayer != Net_Arbitrator)
		out.AppendFormat("\tStart tics delay: %d", LevelStartDebug);

	const int delay = max<int>((ClientTic - gametic) / TicDup, 0);
	const int msDelay = min<int>(delay * TicDup * 1000.0 / TICRATE, 999);
	const int buffer = max<int>(StabilityBuffer, 0);
	const int msBuffer = min<int>(buffer * 1000.0 / TICRATE, 999);
	out.AppendFormat("\nLocal\n\tIs arbitrator: %d\tDelay: %02d (%03dms)\tStability Buffer: %02d (%03dms)",
		consoleplayer == Net_Arbitrator,
		delay, msDelay,
		buffer, msBuffer);
	out.AppendFormat("\tInput lag now/avg/max: %02d/%02d/%02d (%03d/%03d/%03dms)",
		LocalInputLagHistory.Current(),
		LocalInputLagHistory.Average(),
		LocalInputLagHistory.Max,
		SeqTicsToMs(LocalInputLagHistory.Current()),
		SeqTicsToMs(LocalInputLagHistory.Average()),
		SeqTicsToMs(LocalInputLagHistory.Max));

	if (consoleplayer != Net_Arbitrator)
		out.AppendFormat("\tAvg latency: %03ums", min<unsigned int>(ClientStates[consoleplayer].AverageLatency, 999u));

	if (LevelStartStatus != LST_READY)
	{
		if (LevelStartStatus == LST_HOST)
			out.AppendFormat("\tWaiting for packets");
		else if (consoleplayer == Net_Arbitrator)
			out.AppendFormat("\tWaiting for acks");
		else
			out.AppendFormat("\tWaiting for arbitrator");
	}

	const int newestSeq = ClientTic / TicDup;
	int lowestSeq = newestSeq;
	for (auto client : NetworkClients)
	{
		if (client == consoleplayer)
			continue;

		auto& state = ClientStates[client];
		if (!IsTrackableClientForLagStats(client))
			continue;
		const int seq = GetDisplayedClientSequenceForLagStats(client, newestSeq);
		if (seq < lowestSeq)
			lowestSeq = seq;
	}

	for (auto client : NetworkClients)
	{
		if (client == consoleplayer)
			continue;

		auto& state = ClientStates[client];
		const bool tracked = IsTrackableClientForLagStats(client);
		const int seq = tracked ? GetDisplayedClientSequenceForLagStats(client, newestSeq) : state.CurrentSequence;

		out.AppendFormat("\n%s", players[client].userinfo.GetName(12));
		if (client == Net_Arbitrator)
			out.AppendFormat(IsDedicatedControlStream(client) ? "\t(Host Ctrl)" : "\t(Host)");
		if (tracked && seq == lowestSeq)
			out.AppendFormat("\t(Gate)");

		if ((state.Flags & CF_RETRANSMIT) == CF_RETRANSMIT)
			out.AppendFormat("\t(RT)");
		else if (state.Flags & CF_RETRANSMIT_SEQ)
			out.AppendFormat("\t(RT SEQ)");
		else if (state.Flags & CF_RETRANSMIT_CON)
			out.AppendFormat("\t(RT CON)");

		if ((state.Flags & CF_MISSING) == CF_MISSING)
			out.AppendFormat("\t(MISS)");
		else if (state.Flags & CF_MISSING_SEQ)
			out.AppendFormat("\t(MISS SEQ)");
		else if (state.Flags & CF_MISSING_CON)
			out.AppendFormat("\t(MISS CON)");

		out.AppendFormat("\n");
		
		out.AppendFormat("\tSeq: %06d\tAck: %06d\tConsistency: %06d",
			seq, state.SequenceAck, state.ConsistencyAck);
		if (tracked)
		{
			out.AppendFormat("\tBehind now/avg/max: %02d/%02d/%02d (%03d/%03d/%03dms)",
				StreamBehindHistory[client].Current(),
				StreamBehindHistory[client].Average(),
				StreamBehindHistory[client].Max,
				SeqTicsToMs(StreamBehindHistory[client].Current()),
				SeqTicsToMs(StreamBehindHistory[client].Average()),
				SeqTicsToMs(StreamBehindHistory[client].Max));
		}
		if (client != Net_Arbitrator)
			out.AppendFormat("\tAvg latency: %03ums", min<unsigned int>(state.AverageLatency, 999u));
	}

	if (consoleplayer == Net_Arbitrator)
		out.AppendFormat("\nAvailable tics: %03d", max<int>(lowestSeq - (gametic / TicDup), 0));
	return out;
}

// Forces playsim processing time to be consistent across frames.
// This improves interpolation for frames in between tics.
//
// With this cvar off the mods with a high playsim processing time will appear
// less smooth as the measured time used for interpolation will vary.

CVAR(Bool, r_ticstability, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static uint64_t stabilityticduration = 0;
static uint64_t stabilitystarttime = 0;

static void TicStabilityWait()
{
	using namespace std::chrono;
	using namespace std::this_thread;

	if (!r_ticstability)
		return;

	uint64_t start = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
	while (true)
	{
		uint64_t cur = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
		if (cur - start > stabilityticduration)
			break;
	}
}

static void TicStabilityBegin()
{
	using namespace std::chrono;
	stabilitystarttime = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

static void TicStabilityEnd()
{
	using namespace std::chrono;
	uint64_t stabilityendtime = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
	stabilityticduration = min(stabilityendtime - stabilitystarttime, (uint64_t)1'000'000);
}

// Don't stabilize tics that are going to have incredibly long pauses in them.
static bool ShouldStabilizeTick()
{
	return gameaction != ga_recordgame && gameaction != ga_newgame && gameaction != ga_newgame2
			&& gameaction != ga_loadgame && gameaction != ga_loadgamehidecon && gameaction != ga_autoloadgame && gameaction != ga_loadgameplaydemo
			&& gameaction != ga_savegame && gameaction != ga_autosave && gameaction != ga_quicksave
			&& gameaction != ga_worlddone && gameaction != ga_completed && gameaction != ga_screenshot && gameaction != ga_fullconsole;
}

// If the connection has been unstable then let the game lag behind for a little bit
// while we wait for it to stabilize, otherwise everything will appear to jitter around.
static void CalculateNetStabilityBuffer(int diff)
{
	if (!netgame || demoplayback)
	{
		StabilityBuffer = 0;
		return;
	}

	if (diff < 0)
		diff = 0;

	if (!(gametic % TicDup))
	{
		StabilityTics[CurStabilityTic++ % STABILITYTICS] = diff > PrevAvailableDiff ? diff : 0;
		PrevAvailableDiff = diff;
	}

	// If we're not balancing latency, just give an extra tic for padding
	// and nothing else.
	if (!net_ticbalance)
	{
		StabilityBuffer = 1;
		return;
	}

	double total = 0.0;
	int unstableCount = 0;
	for (int t : StabilityTics)
	{
		if (t > 0)
		{
			++unstableCount;
			total += t;
		}
	}

	StabilityBuffer = unstableCount > 0 ? static_cast<int>(ceil(total / unstableCount)) : 0;
}

//
// TryRunTics
//
void TryRunTics()
{
	GC::CheckGC();

	auto shouldHibernateNow = [&]()
	{
		return ShouldHibernateDedicatedServer();
	};

	if (ToggleFullscreen)
	{
		ToggleFullscreen = false;
		AddCommandString("toggle vid_fullscreen");
	}
	
	bool doWait = (cl_capfps || pauseext || (!netgame && r_NoInterpolate && !M_IsAnimated()));
	if (vid_dontdowait && (vid_maxfps > 0 || vid_vsync))
		doWait = false;
	if (!netgame && !AppActive && vid_lowerinbackground)
		doWait = true;
	if (dedicatedServer)
		doWait = true;

	// Get the full number of tics the client can run.
	if (doWait)
		EnterTic = I_WaitForTic(LastEnterTic);
	else
		EnterTic = I_GetTime();

	const int startCommand = ClientTic;
	int totalTics = EnterTic - LastEnterTic;
	if (totalTics > 1 && singletics)
		totalTics = 1;

	// If the joiner already spawned locally but the host has not yet confirmed
	// activation, keep nudging the host with STATE_ACTIVE. The confirmation
	// comes back as DEM_MIDGAMEACTIVE through the normal lockstep stream.
	ResendMidgameStateActiveIfNeeded();

	// A dedicated server with no real players should stay packet-responsive
	// but must not advance local command generation while hibernating.
	if (shouldHibernateNow())
	{
		NetUpdate(0);
		LastEnterTic = EnterTic;

		if (pauseext)
			return;

		if (shouldHibernateNow())
		{
			LastGameUpdate = EnterTic;
			LagState = LAG_NONE;
			for (auto client : NetworkClients)
				players[client].waiting = false;
			return;
		}
	}

	// While the snapshot is still being transferred/loaded, the joiner cannot
	// participate in the lockstep yet.
	if (gameaction == ga_midgamejoin)
	{
		NetUpdate(0);

		G_DoMidgameJoin();

		LastEnterTic = EnterTic;
		return;
	}

	// Listen for other clients and send out data as needed. This is also
	// needed for singleplayer! But is instead handled entirely through local
	// buffers. This has a limit of one seconds worth of commands that can be
	// generated in advanced from the last time the game updated.
	NetUpdate(totalTics);

	LastEnterTic = EnterTic;

	// If the game is paused, everything we need to update has already done so.
	if (pauseext)
		return;

	if (shouldHibernateNow())
	{
		LastGameUpdate = EnterTic;
		LagState = LAG_NONE;
		for (auto client : NetworkClients)
			players[client].waiting = false;
		return;
	}

	// Get the amount of tics the client can actually run. This accounts for waiting for other
	// players over the network.
	int lowestSequence = INT_MAX;
	for (auto client : NetworkClients)
	{
		// A waiting late-joiner has no local gameplay command stream yet.
		// If we include its own slot here, lowestSequence gets pinned to the
		// snapshot baseline and the client never advances far enough to
		// execute DEM_MIDGAMESPAWN from the host event stream.
		if (ShouldSuppressMidgameJoinLocalCommands() && client == consoleplayer)
			continue;

		// Skip joining clients — they have no commands yet and would block the lockstep.
		// Skip quitting clients — they stopped sending commands and will be removed by
		// NCMD_QUITTERS (network) + DEM_PLAYERDISCONNECT (game state) shortly.
		if (ClientStates[client].Flags & (CF_JOINING | CF_AWAITING_STATE | CF_QUIT))
			continue;
		int seq = ClientStates[client].CurrentSequence;
		// Non-host clients have their own commands in the local buffer already.
		// Use the local command count instead of waiting for the host echo, which
		// requires a round-trip per tic. Without this, a mid-game joiner (whose
		// command buffer starts at zero depth) freezes while its own
		// CurrentSequence trickles up one round-trip at a time.
		if (consoleplayer != Net_Arbitrator && client == consoleplayer)
			seq = max(seq, ClientTic / TicDup - 1);
		if (seq < lowestSequence)
			lowestSequence = seq;
	}

	// Test player prediction code in singleplayer by pretending there is another player
	// that is running exactly x ticks behind us, emulating having a specific amount of ping
	if (cl_debugprediction > 0
		&& !netgame && !demoplayback) // would probably function, but there's no reason to
	{
		if (lowestSequence > cl_debugprediction)
		{
			lowestSequence -= cl_debugprediction;
		}
		else
		{
			lowestSequence = 0;
		}
	}

	// If the lowest confirmed tic matches the server gametic or greater, allow the client
	// to run some of them.
	const int availableTics = (lowestSequence - gametic / TicDup) + 1;

	// If the amount of tics to run is falling behind the amount of available tics,
	// speed the playsim up a bit to help catch up.
	int runTics = min<int>(totalTics, availableTics);
	if (!singletics && totalTics > 0)
	{
		CalculateNetStabilityBuffer(availableTics - totalTics);
		if (totalTics < availableTics - StabilityBuffer)
			++runTics;
	}

	SampleNetLagHistories();

	const int worldTimer = primaryLevel->LocalWorldTimer;
	// If there are no tics to run, check for possible stall conditions and new
	// commands to predict.
	if (runTics <= 0)
	{
		// If we're in between a tic, try and balance things out.
		if (totalTics <= 0)
		{
			TicStabilityWait();
		}
		else
		{
			if (!dedicatedServer)
				P_ClearLevelInterpolation();
			LagState = LAG_WAITING;
		}

		// If we actually advanced a command, update the player's position (even if a
		// tic passes this isn't guaranteed to happen since it's capped to 35 in advance).
		if (ClientTic > startCommand && playeringame[consoleplayer] && !cl_noprediction && !dedicatedServer)
		{
			LagState = LAG_PREDICTING;
			P_PredictClient();
		}

		// If we actually did have some tics available, make sure the UI
		// still has a chance to run.
		if (!dedicatedServer)
		{
			for (int i = 0; i < totalTics; ++i)
				P_RunClientSideLogic();
		}

		if (totalTics > 0)
		{
			if (!dedicatedServer && playeringame[consoleplayer])
				S_UpdateSounds(players[consoleplayer].camera, primaryLevel->LocalWorldTimer - min<int>(primaryLevel->LocalWorldTimer, worldTimer));
			if (!dedicatedServer)
				NetworkEntityManager::VerifyPredictedEntities();
		}

		return;
	}

	LagState = ClientTic > startCommand ? LAG_NONE : LAG_SKIPPING;
	for (auto client : NetworkClients)
		players[client].waiting = false;

	// Update the last time the game tic'd.
	LastGameUpdate = EnterTic;

	// Run the available tics.
	if (playeringame[consoleplayer] && !cl_noprediction && !dedicatedServer)
		P_UnPredictClient();

	while (runTics--)
	{
		const bool stabilize = !dedicatedServer && ShouldStabilizeTick();
		if (stabilize)
			TicStabilityBegin();

		if (advancedemo)
			D_DoAdvanceDemo();

		G_Ticker();
		MakeConsistencies();
		++gametic;

		if (stabilize)
			TicStabilityEnd();

		if (bCommandsReset)
		{
			bCommandsReset = false;
			break;
		}
	}
	if (playeringame[consoleplayer] && !cl_noprediction && !dedicatedServer)
		P_PredictClient();

	// These should use the actual tics since they're not actually tied to the gameplay logic.
	// Make sure it always comes after so the HUD has the correct game state when updating.
	if (!dedicatedServer)
	{
		for (int i = 0; i < totalTics; ++i)
			P_RunClientSideLogic();
	}

	// Since the level could get reset mid-tick, make sure the smaller of the two values is used
	// since it should only go up otherwise.
	if (!dedicatedServer && playeringame[consoleplayer])
		S_UpdateSounds(players[consoleplayer].camera, primaryLevel->LocalWorldTimer - min<int>(primaryLevel->LocalWorldTimer, worldTimer));
	if (!dedicatedServer)
		NetworkEntityManager::VerifyPredictedEntities();
}

void Net_NewClientTic()
{
	NetEvents.NewClientTic();
}

void Net_Initialize()
{
	NetEvents.InitializeEventData();
}

void Net_WriteInt8(uint8_t it)
{
	NetEvents << it;
}

void Net_WriteInt16(int16_t it)
{
	NetEvents << it;
}

void Net_WriteInt32(int32_t it)
{
	NetEvents << it;
}

void Net_WriteInt64(int64_t it)
{
	NetEvents << it;
}

void Net_WriteFloat(float it)
{
	NetEvents << it;
}

void Net_WriteDouble(double it)
{
	NetEvents << it;
}

void Net_WriteString(const char *it)
{
	NetEvents << it;
}

void Net_WriteBytes(const uint8_t *block, int len)
{
	while (len--)
		NetEvents << *block++;
}

//==========================================================================
//
// Dynamic buffer interface
//
//==========================================================================

FDynamicBuffer::FDynamicBuffer()
{
	m_Data = nullptr;
	m_Len = m_BufferLen = 0;
}

FDynamicBuffer::~FDynamicBuffer()
{
	if (m_Data != nullptr)
	{
		M_Free(m_Data);
		m_Data = nullptr;
	}
	m_Len = m_BufferLen = 0;
}

void FDynamicBuffer::SetData(const uint8_t *data, int len)
{
	if (len > m_BufferLen)
	{
		m_BufferLen = (len + 255) & ~255;
		m_Data = (uint8_t *)M_Realloc(m_Data, m_BufferLen);
	}

	if (data != nullptr)
	{
		m_Len = len;
		memcpy(m_Data, data, len);
	}
	else 
	{
		m_Len = 0;
	}
}

uint8_t *FDynamicBuffer::GetData(int *len)
{
	if (len != nullptr)
		*len = m_Len;
	return m_Len ? m_Data : nullptr;
}

TArrayView<uint8_t> FDynamicBuffer::GetTArrayView()
{
	return TArrayView(m_Data, m_Len);
}

static int RemoveClass(FLevelLocals *Level, const PClass *cls)
{
	AActor *actor;
	int removecount = 0;
	bool player = false;
	auto iterator = Level->GetThinkerIterator<AActor>(cls->TypeName);
	while ((actor = iterator.Next()))
	{
		if (actor->IsA(cls))
		{
			// [MC]Do not remove LIVE players.
			if (actor->player != nullptr)
			{
				player = true;
				continue;
			}
			// [SP] Don't remove owned inventory objects.
			if (!actor->IsMapActor())
				continue;

			removecount++; 
			actor->ClearCounters();
			actor->Destroy();
		}
	}

	if (player)
		Printf("Cannot remove live players!\n");

	return removecount;

}

EXTERN_CVAR(Int, displaynametags)
EXTERN_CVAR(Int, nametagcolor)

static void SelectWeapon(int player, int slot)
{
	auto mo = players[player].mo;
	if (mo == nullptr || gamestate != GS_LEVEL || paused
		|| players[player].playerstate != PST_LIVE)
	{
		return;
	}

	AActor* weap = nullptr;
	if (slot >= 0 && slot < NUM_WEAPON_SLOTS)
	{
		IFVIRTUALPTRNAME(mo, NAME_PlayerPawn, PickWeapon)
			weap = CallVM<AActor*>(func, mo, slot, (int)!(dmflags2 & DF2_DONTCHECKAMMO));
	}
	else if (slot == WST_NEXT)
	{
		IFVIRTUALPTRNAME(mo, NAME_PlayerPawn, PickNextWeapon)
			weap = CallVM<AActor*>(func, mo);
	}
	else if (slot == WST_PREV)
	{
		IFVIRTUALPTRNAME(mo, NAME_PlayerPawn, PickPrevWeapon)
			weap = CallVM<AActor*>(func, mo);
	}

	if (weap == nullptr)
		return;

	// Make sure the returned weapon actually exists in that player's inventory.
	const unsigned id = weap->InventoryID;
	AActor* invItem = mo->Inventory;
	for (; invItem != nullptr; invItem = invItem->Inventory)
	{
		if (invItem->InventoryID == id)
			break;
	}

	if (invItem != weap)
		return;

	if (player == consoleplayer)
	{
		if (weap != players[player].ReadyWeapon)
			S_Sound(mo, CHAN_AUTO, 0, "misc/weaponchange", 1.0, ATTN_NONE);

		// [Nash] Option to display the name of the weapon being switched to.
		if ((displaynametags & 2) && StatusBar != nullptr && SmallFont != nullptr)
		{
			StatusBar->AttachMessage(Create<DHUDMessageFadeOut>(nullptr, weap->GetTag(),
				1.5f, 0.90f, 0, 0, (EColorRange)*nametagcolor, 2.f, 0.35f), MAKE_ID('W', 'E', 'P', 'N'));
		}
	}

	mo->UseInventory(weap);
}

static void UseFlechette(int player)
{
	auto mo = players[player].mo;
	if (mo == nullptr || gamestate != GS_LEVEL || paused
		|| players[player].playerstate != PST_LIVE)
	{
		return;
	}

	AActor* item = nullptr;
	IFVIRTUALPTRNAME(mo, NAME_PlayerPawn, GetFlechetteItem)
		item = CallVM<AActor*>(func, mo);

	if (item == nullptr)
		return;

	// Make sure the returned item actually exists in that player's inventory.
	const unsigned id = item->InventoryID;
	AActor* invItem = mo->Inventory;
	for (; invItem != nullptr; invItem = invItem->Inventory)
	{
		if (invItem->InventoryID == id)
			break;
	}

	if (invItem == item)
		mo->UseInventory(item);
}

// [RH] Execute a special "ticcmd". The type byte should
//		have already been read, and the stream is positioned
//		at the beginning of the command's actual data.
void Net_DoCommand(int cmd, TArrayView<uint8_t>& stream, int player)
{
	uint8_t pos = 0;
	const char* s = nullptr;
	int i = 0;

	switch (cmd)
	{
	case DEM_SAY:
		{
			const char *name = players[player].userinfo.GetName();
			uint8_t who = ReadInt8(stream);

			s = ReadStringConst(stream);
			// If chat is disabled, there's nothing else to do here since the stream has been advanced.
			if (cl_showchat == CHAT_DISABLED || (MutedClients & ((uint64_t)1u << player)))
				break;

			constexpr int MSG_TEAM = 1;
			constexpr int MSG_BOLD = 2;
			if (!(who & MSG_TEAM))
			{
				if (cl_showchat < CHAT_GLOBAL)
					break;

				// Said to everyone
				if (deathmatch && teamplay)
					Printf(PRINT_CHAT, "(All) ");
				if ((who & MSG_BOLD) && !cl_noboldchat)
					Printf(PRINT_CHAT, TEXTCOLOR_BOLD "* %s [%d]" TEXTCOLOR_BOLD "%s" TEXTCOLOR_BOLD "\n", name, player, s);
				else
					Printf(PRINT_CHAT, "%s [%d]" TEXTCOLOR_CHAT ": %s" TEXTCOLOR_CHAT "\n", name, player, s);

				if (!cl_nochatsound)
					S_Sound(CHAN_VOICE, CHANF_UI, gameinfo.chatSound, 1.0f, ATTN_NONE);
			}
			else if (!deathmatch || players[player].userinfo.GetTeam() == players[consoleplayer].userinfo.GetTeam())
			{
				if (cl_showchat < CHAT_TEAM_ONLY)
					break;

				// Said only to members of the player's team
				if (deathmatch && teamplay)
					Printf(PRINT_TEAMCHAT, "(Team) ");
				if ((who & MSG_BOLD) && !cl_noboldchat)
					Printf(PRINT_TEAMCHAT, TEXTCOLOR_BOLD "* %s [%d]" TEXTCOLOR_BOLD "%s" TEXTCOLOR_BOLD "\n", name, player, s);
				else
					Printf(PRINT_TEAMCHAT, "%s [%d]" TEXTCOLOR_TEAMCHAT ": %s" TEXTCOLOR_TEAMCHAT "\n", name, player, s);

				if (!cl_nochatsound)
					S_Sound(CHAN_VOICE, CHANF_UI, gameinfo.chatSound, 1.0f, ATTN_NONE);
			}
		}
		break;

	case DEM_MUSICCHANGE:
		S_ChangeMusic(ReadStringConst(stream));
		break;

	case DEM_PRINT:
		Printf("%s", ReadStringConst(stream));
		break;

	case DEM_CENTERPRINT:
		C_MidPrint(nullptr, ReadStringConst(stream));
		break;

	case DEM_UINFCHANGED:
		D_ReadUserInfoStrings(player, stream, true);
		break;

	case DEM_SINFCHANGED:
		D_DoServerInfoChange(stream, false);
		break;

	case DEM_SINFCHANGEDXOR:
		D_DoServerInfoChange(stream, true);
		break;

	case DEM_GIVECHEAT:
		s = ReadStringConst(stream);
		cht_Give(&players[player], s, ReadInt32(stream));
		if (player != consoleplayer)
		{
			FString message = GStrings.GetString("TXT_X_CHEATS");
			message.Substitute("%s", players[player].userinfo.GetName());
			Printf("%s: give %s\n", message.GetChars(), s);
		}
		break;

	case DEM_TAKECHEAT:
		s = ReadStringConst(stream);
		cht_Take(&players[player], s, ReadInt32(stream));
		break;

	case DEM_SETINV:
		s = ReadStringConst(stream);
		i = ReadInt32(stream);
		cht_SetInv(&players[player], s, i, !!ReadInt8(stream));
		break;

	case DEM_WARPCHEAT:
		{
			int x = ReadInt16(stream);
			int y = ReadInt16(stream);
			int z = ReadInt16(stream);
			P_TeleportMove(players[player].mo, DVector3(x, y, z), true);
		}
		break;

	case DEM_GENERICCHEAT:
		cht_DoCheat(&players[player], ReadInt8(stream));
		break;

	case DEM_CHANGEMAP2:
		pos = ReadInt8(stream);
		[[fallthrough]];
	case DEM_CHANGEMAP:
		// Change to another map without disconnecting other players
		s = ReadStringConst(stream);
		// Using LEVEL_NOINTERMISSION tends to throw the game out of sync.
		// That was a long time ago. Maybe it works now?
		primaryLevel->flags |= LEVEL_CHANGEMAPCHEAT;
		primaryLevel->ChangeLevel(s, pos, 0);
		break;

	case DEM_SUICIDE:
		cht_Suicide(&players[player]);
		break;

	case DEM_ADDBOT:
		primaryLevel->BotInfo.TryAddBot(primaryLevel, stream, player);
		break;

	case DEM_KILLBOTS:
		primaryLevel->BotInfo.RemoveAllBots(primaryLevel, true);
		Printf ("Removed all bots\n");
		break;

	case DEM_CENTERVIEW:
		players[player].centering = true;
		break;

	case DEM_INVUSEALL:
		if (gamestate == GS_LEVEL && !paused
			&& players[player].playerstate != PST_DEAD)
		{
			AActor *item = players[player].mo->Inventory;
			auto pitype = PClass::FindActor(NAME_PuzzleItem);
			while (item != nullptr)
			{
				AActor *next = item->Inventory;
				IFVIRTUALPTRNAME(item, NAME_Inventory, UseAll)
				{
					VMValue param[] = { item, players[player].mo };
					VMCall(func, param, 2, nullptr, 0);
				}
				item = next;
			}
		}
		break;

	case DEM_INVUSE:
	case DEM_INVDROP:
		{
			uint32_t which = ReadInt32(stream);
			int amt = -1;
			if (cmd == DEM_INVDROP)
				amt = ReadInt32(stream);

			if (gamestate == GS_LEVEL && !paused
				&& players[player].playerstate != PST_DEAD)
			{
				auto item = players[player].mo->Inventory;
				while (item != nullptr && item->InventoryID != which)
					item = item->Inventory;

				if (item != nullptr)
				{
					if (cmd == DEM_INVUSE)
						players[player].mo->UseInventory(item);
					else
						players[player].mo->DropInventory(item, amt);
				}
			}
		}
		break;

	case DEM_SUMMON:
	case DEM_SUMMONFRIEND:
	case DEM_SUMMONFOE:
	case DEM_SUMMONMBF:
	case DEM_SUMMON2:
	case DEM_SUMMONFRIEND2:
	case DEM_SUMMONFOE2:
		{
			int angle = 0;
			int16_t tid = 0;
			uint8_t special = 0;
			int args[5];

			s = ReadStringConst(stream);
			if (cmd >= DEM_SUMMON2 && cmd <= DEM_SUMMONFOE2)
			{
				angle = ReadInt16(stream);
				tid = ReadInt16(stream);
				special = ReadInt8(stream);
				for (i = 0; i < 5; i++) args[i] = ReadInt32(stream);
			}

			AActor* source = players[player].mo;
			if (source != NULL)
			{
				PClassActor* typeinfo = PClass::FindActor(s);
				if (typeinfo != NULL)
				{
					if (GetDefaultByType(typeinfo)->flags & MF_MISSILE)
					{
						P_SpawnPlayerMissile(source, 0, 0, 0, typeinfo, source->Angles.Yaw);
					}
					else
					{
						const AActor* def = GetDefaultByType(typeinfo);
						DVector3 spawnpos = source->Vec3Angle(def->radius * 2 + source->radius, source->Angles.Yaw, 8.);

						AActor* spawned = Spawn(primaryLevel, typeinfo, spawnpos, ALLOW_REPLACE);
						if (spawned != NULL)
						{
							spawned->SpawnFlags |= MTF_CONSOLETHING;
							if (cmd == DEM_SUMMONFRIEND || cmd == DEM_SUMMONFRIEND2 || cmd == DEM_SUMMONMBF)
							{
								if (spawned->CountsAsKill())
								{
									primaryLevel->total_monsters--;
								}
								spawned->FriendPlayer = player + 1;
								spawned->flags |= MF_FRIENDLY;
								spawned->LastHeard = players[player].mo;
								spawned->health = spawned->SpawnHealth();
								if (cmd == DEM_SUMMONMBF)
									spawned->flags3 |= MF3_NOBLOCKMONST;
							}
							else if (cmd == DEM_SUMMONFOE || cmd == DEM_SUMMONFOE2)
							{
								spawned->FriendPlayer = 0;
								spawned->flags &= ~MF_FRIENDLY;
								spawned->health = spawned->SpawnHealth();
							}

							if (cmd >= DEM_SUMMON2 && cmd <= DEM_SUMMONFOE2)
							{
								spawned->Angles.Yaw = source->Angles.Yaw - DAngle::fromDeg(angle);
								spawned->special = special;
								for (i = 0; i < 5; i++) {
									spawned->args[i] = args[i];
								}
								if (tid) spawned->SetTID(tid);
							}
						}
					}
				}
				else
				{ // not an actor, must be a visualthinker
					PClass* typeinfo = PClass::FindClass(s);
					if (typeinfo && typeinfo->IsDescendantOf("VisualThinker"))
					{
						DVector3 spawnpos = source->Vec3Angle(source->radius * 4, source->Angles.Yaw, 8.);
						auto vt = DVisualThinker::NewVisualThinker(source->Level, typeinfo, false);
						if (vt)
						{
							vt->PT.Pos = spawnpos;
							vt->UpdateSector();
						}
					}
				}
			}
		}
		break;

	case DEM_SPRAY:
		s = ReadStringConst(stream);
		SprayDecal(players[player].mo, s);
		break;

	case DEM_MDK:
		s = ReadStringConst(stream);
		cht_DoMDK(&players[player], s);
		break;

	case DEM_PAUSE:
		if (gamestate == GS_LEVEL)
		{
			if (paused)
			{
				paused = 0;
				S_ResumeSound(false);
			}
			else
			{
				paused = player + 1;
				S_PauseSound(false, false);
			}
		}
		break;

	case DEM_SAVEGAME:
		if (gamestate == GS_LEVEL)
		{
			savegamefile = ReadStringConst(stream);
			savedescription = ReadStringConst(stream);
			if (player != consoleplayer)
			{
				// Paths sent over the network will be valid for the system that sent
				// the save command. For other systems, the path needs to be changed.
				FString basename = ExtractFileBase(savegamefile.GetChars(), true);
				savegamefile = G_BuildSaveName(basename.GetChars());
			}
		}
		gameaction = ga_savegame;
		break;

	case DEM_CHECKAUTOSAVE:
		// Do not autosave in multiplayer games or when dead.
		// For demo playback, DEM_DOAUTOSAVE already exists in the demo if the
		// autosave happened. And if it doesn't, we must not generate it.
		if (!netgame && !demoplayback && disableautosave < 2 && autosavecount
			&& players[player].playerstate == PST_LIVE && !deathmatch)
		{
			Net_WriteInt8(DEM_DOAUTOSAVE);
		}
		break;

	case DEM_DOAUTOSAVE:
		gameaction = ga_autosave;
		break;

	case DEM_FOV:
		{
			float newfov = ReadFloat(stream);
			if (newfov != players[player].DesiredFOV)
			{
				Printf("FOV%s set to %g\n",
					player == Net_Arbitrator ? " for everyone" : "",
					newfov);
			}

			for (auto client : NetworkClients)
				players[client].DesiredFOV = newfov;
		}
		break;

	case DEM_MYFOV:
		players[player].DesiredFOV = ReadFloat(stream);
		break;

	case DEM_RUNSCRIPT:
	case DEM_RUNSCRIPT2:
		{
			int snum = ReadInt16(stream);
			int argn = ReadInt8(stream);
			RunScript(stream, players[player].mo, snum, argn, (cmd == DEM_RUNSCRIPT2) ? ACS_ALWAYS : 0);
		}
		break;

	case DEM_RUNNAMEDSCRIPT:
		{
			s = ReadStringConst(stream);
			int argn = ReadInt8(stream);
			RunScript(stream, players[player].mo, -FName(s).GetIndex(), argn & 127, (argn & 128) ? ACS_ALWAYS : 0);
		}
		break;

	case DEM_RUNSPECIAL:
		{
			int snum = ReadInt16(stream);
			int argn = ReadInt8(stream);
			int arg[5] = {};

			for (i = 0; i < argn; ++i)
			{
				int argval = ReadInt32(stream);
				if ((unsigned)i < countof(arg))
					arg[i] = argval;
			}

			if (!CheckCheatmode(player == consoleplayer))
				P_ExecuteSpecial(primaryLevel, snum, nullptr, players[player].mo, false, arg[0], arg[1], arg[2], arg[3], arg[4]);
		}
		break;

	case DEM_CROUCH:
		if (gamestate == GS_LEVEL && players[player].mo != nullptr
			&& players[player].playerstate == PST_LIVE && !(players[player].oldbuttons & BT_JUMP)
			&& !P_IsPlayerTotallyFrozen(&players[player]))
		{
			players[player].crouching = players[player].crouchdir < 0 ? 1 : -1;
		}
		break;

	case DEM_MORPHEX:
		{
			s = ReadStringConst(stream);
			FString msg = cht_Morph(players + player, PClass::FindActor(s), false);
			if (player == consoleplayer)
				Printf("%s\n", msg[0] != '\0' ? msg.GetChars() : "Morph failed.");
		}
		break;

	case DEM_ADDCONTROLLER:
		{
			uint8_t playernum = ReadInt8(stream);
			players[playernum].settings_controller = true;
			if (consoleplayer == playernum)
				Printf("You can now control game settings\n");
			else if (consoleplayer == Net_Arbitrator)
				Printf("%s [%d] is now a settings controller\n", players[playernum].userinfo.GetName(), playernum);
		}
		break;

	case DEM_DELCONTROLLER:
		{
			uint8_t playernum = ReadInt8(stream);
			players[playernum].settings_controller = false;
			if (consoleplayer == playernum)
				Printf("You can no longer control game settings\n");
			else if (consoleplayer == Net_Arbitrator)
				Printf("%s [%d] is no longer a settings controller\n", players[playernum].userinfo.GetName(), playernum);
		}
		break;

	case DEM_KILLCLASSCHEAT:
		{
			s = ReadStringConst(stream);
			int killcount = 0;
			PClassActor *cls = PClass::FindActor(s);

			if (cls != nullptr)
			{
				killcount = primaryLevel->Massacre(false, cls->TypeName);
				PClassActor *cls_rep = cls->GetReplacement(primaryLevel);
				if (cls != cls_rep)
					killcount += primaryLevel->Massacre(false, cls_rep->TypeName);

				Printf("Killed %d monsters of type %s.\n", killcount, s);
			}
			else
			{
				Printf("%s is not an actor class.\n", s);
			}
		}
		break;

	case DEM_REMOVE:
		{
			s = ReadStringConst(stream);
			int removecount = 0;
			PClassActor *cls = PClass::FindActor(s);
			if (cls != nullptr && cls->IsDescendantOf(RUNTIME_CLASS(AActor)))
			{
				removecount = RemoveClass(primaryLevel, cls);
				const PClass *cls_rep = cls->GetReplacement(primaryLevel);
				if (cls != cls_rep)
					removecount += RemoveClass(primaryLevel, cls_rep);

				Printf("Removed %d actors of type %s.\n", removecount, s);
			}
			else
			{
				Printf("%s is not an actor class.\n", s);
			}
		}
		break;

	case DEM_CONVREPLY:
	case DEM_CONVCLOSE:
	case DEM_CONVNULL:
		P_ConversationCommand(cmd, player, stream);
		break;

	case DEM_SETSLOT:
	case DEM_SETSLOTPNUM:
		{
			int pnum = player;
			if (cmd == DEM_SETSLOTPNUM)
				pnum = ReadInt8(stream);

			unsigned int slot = ReadInt8(stream);
			int count = ReadInt8(stream);
			if (slot < NUM_WEAPON_SLOTS)
				players[pnum].weapons.ClearSlot(slot);

			for (i = 0; i < count; ++i)
			{
				PClassActor *wpn = Net_ReadWeapon(stream);
				players[pnum].weapons.AddSlot(slot, wpn, pnum == consoleplayer);
			}
		}
		break;

	case DEM_ADDSLOT:
		{
			int slot = ReadInt8(stream);
			PClassActor *wpn = Net_ReadWeapon(stream);
			players[player].weapons.AddSlot(slot, wpn, player == consoleplayer);
		}
		break;

	case DEM_ADDSLOTDEFAULT:
		{
			int slot = ReadInt8(stream);
			PClassActor *wpn = Net_ReadWeapon(stream);
			players[player].weapons.AddSlotDefault(slot, wpn, player == consoleplayer);
		}
		break;

	case DEM_SETPITCHLIMIT:
		players[player].MinPitch = DAngle::fromDeg(-ReadInt8(stream));		// up
		players[player].MaxPitch = DAngle::fromDeg(ReadInt8(stream));		// down
		break;

	case DEM_REVERTCAMERA:
		players[player].camera = players[player].mo;
		break;

	case DEM_FINISHGAME:
		// Simulate an end-of-game action
		primaryLevel->ChangeLevel(nullptr, 0, 0);
		break;

	case DEM_NETEVENT:
		{
			s = ReadStringConst(stream);
			int argn = ReadInt8(stream);
			int arg[3] = { 0, 0, 0 };
			for (int i = 0; i < 3; i++)
				arg[i] = ReadInt32(stream);
			bool manual = !!ReadInt8(stream);
			primaryLevel->localEventManager->Console(player, s, arg[0], arg[1], arg[2], manual, false);
		}
		break;

	case DEM_ENDSCREENJOB:
		EndScreenJob();
		break;

	case DEM_READIED:
		Net_PlayerReadiedUp(player);
		break;

	case DEM_ZSC_CMD:
		{
			FName cmd = ReadStringConst(stream);
			unsigned int size = ReadInt16(stream);

			TArray<uint8_t> buffer;
			if (size)
			{
				buffer.Grow(size);
				for (unsigned int i = 0u; i < size; ++i)
					buffer.Push(ReadInt8(stream));
			}

			FNetworkCommand netCmd = { player, cmd, buffer };
			primaryLevel->localEventManager->NetCommand(netCmd);
		}
		break;

	case DEM_CHANGESKILL:
		NextSkill = ReadInt32(stream);
		break;

	case DEM_KICK:
		{
			const int pNum = ReadInt8(stream);
			if (pNum == consoleplayer)
			{
				I_Error("You have been kicked from the game");
			}
			else if (NetworkClients.InGame(pNum))
			{
				Printf("%s [%d] has been kicked from the game\n", players[pNum].userinfo.GetName(), pNum);
				DisconnectClient(pNum);
			}
		}
		break;

	case DEM_WEAPSELECT:
		SelectWeapon(player, ReadInt8(stream));
		break;

	case DEM_USEFLECHETTE:
		UseFlechette(player);
		break;

	case DEM_MIDGAMESPAWN:
	{
		const int pnum = ReadInt8(stream);
		if (pnum >= 0 && pnum < (int)MAXPLAYERS && !playeringame[pnum])
		{
			// Synchronize the joiner's command sequence to the current gametic
			// on ALL nodes. ClientConnecting set CurrentSequence at CONNECT time
			// (before V_Init2/state transfer), so it's now stale by hundreds of
			// tics. Without this update, the stale sequence blocks lowestSequence
			// in TryRunTics, freezing the entire lockstep until the joiner's
			// actual commands catch up — causing a multi-second freeze for all
			// clients and subsequent input lag from command pileup.
			const int lastSeq = max(gametic / TicDup - 1, -1);
			const int lastCon = max(CurrentConsistency - 1, -1);
			ClientStates[pnum].CurrentSequence = lastSeq;
			ClientStates[pnum].SequenceAck = lastSeq;
			// Initialize consistency tracking for the new player on all nodes,
			// using the last completed world tic. The packet carrying
			// DEM_MIDGAMESPAWN may already have advanced the existing streams to
			// the first post-snapshot tic, so do not overwrite those here.
			ClientStates[pnum].LastVerifiedConsistency = lastCon;
			ClientStates[pnum].CurrentNetConsistency = lastCon;
			ClientStates[pnum].ConsistencyAck = lastCon;

			// On the joiner's node, this is now just the catch-up barrier:
			// the snapshot world is already renderable, but the real pawn is not
			// spawned until DEM_MIDGAMEACTIVE. That avoids exposing a helpless
			// player to gameplay during the activation handshake.
			if (pnum == consoleplayer)
			{
				G_ApplyRendererPitchLimitsToPlayer(pnum);
				IncomingStateTransfer.loadedSent = false;
				IncomingStateTransfer.activeSent = true;
				SendMidgameStateActive();
			}
		}
		break;
	}

	case DEM_MIDGAMEACTIVE:
	{
		const int pnum = ReadInt8(stream);
		const DAngle minPitch = DAngle::fromDeg(-ReadInt8(stream));
		const DAngle maxPitch = DAngle::fromDeg(ReadInt8(stream));
		if (pnum >= 0 && pnum < (int)MAXPLAYERS)
		{
			auto& netState = ClientStates[pnum];
			const int lastSeq = max(gametic / TicDup - 1, 0);
			const int lastCon = max(CurrentConsistency - 1, 0);
			if (!playeringame[pnum])
			{
				playeringame[pnum] = true;
				players[pnum].playerstate = PST_ENTER;
				primaryLevel->DoReborn(pnum, true);
			}
			players[pnum].MinPitch = minPitch;
			players[pnum].MaxPitch = maxPitch;
			// Activation is the first moment this player is truly live.
			// Existing clients must advance the slot's stream baseline here too,
			// otherwise lowestSequence gets pinned by the stale pre-activation
			// sequence and everyone stalls waiting on a stream that should now
			// start from the live activation point.
			netState.CurrentSequence = max(netState.CurrentSequence, lastSeq);
			netState.SequenceAck = max(netState.SequenceAck, netState.CurrentSequence);
			netState.CurrentNetConsistency = max(netState.CurrentNetConsistency, lastCon);
			netState.ConsistencyAck = max(netState.ConsistencyAck, netState.CurrentNetConsistency);
			netState.LastVerifiedConsistency = max(netState.LastVerifiedConsistency, netState.CurrentNetConsistency);
			netState.ResendSequenceFrom = -1;
			netState.ResendConsistencyFrom = -1;
			netState.Flags &= ~(CF_JOINING | CF_AWAITING_STATE | CF_AWAITING_ACTIVE | CF_MISSING | CF_RETRANSMIT);
			if (pnum == consoleplayer)
			{
				const int nextClientTic = ((gametic / TicDup) + 1) * TicDup;
				if (ClientTic < nextClientTic)
					ClientTic = nextClientTic;
				memset(LocalCmds, 0, sizeof(LocalCmds));
				NetEvents.ResetStream();
				SkipCommandTimer = SkipCommandAmount = CommandsAhead = 0;
				netState.CurrentSequence = max(ClientTic / TicDup - 1, 0);
				netState.SequenceAck = netState.CurrentSequence;
				netState.CurrentNetConsistency = lastCon;
				netState.ConsistencyAck = netState.CurrentNetConsistency;
				netState.LastVerifiedConsistency = netState.CurrentNetConsistency;
				G_ClearMidgameJoinRenderState();
				P_ClearPredictionData();
				IncomingStateTransfer.activeSent = false;
			}
			Printf("Player %d has joined the game\n", pnum + 1);
		}
		break;
	}

	case DEM_PLAYERDISCONNECT:
	{
		const int pnum = ReadInt8(stream);
		if (pnum >= 0 && pnum < (int)MAXPLAYERS && playeringame[pnum])
		{
			// Set PST_GONE so G_DoPlayerPop runs at the start of the next
			// G_Ticker. All nodes process this at the same gametic,
			// ensuring deterministic removal of the player's actor.
			players[pnum].playerstate = PST_GONE;
		}
		break;
	}

	default:
		I_Error("Unknown net command: %d", cmd);
		break;
	}
}

// Used by DEM_RUNSCRIPT, DEM_RUNSCRIPT2, and DEM_RUNNAMEDSCRIPT
static void RunScript(TArrayView<uint8_t>& stream, AActor *pawn, int snum, int argn, int always)
{
	// Scripts can be invoked without a level loaded, e.g. via puke(name) CCMD in fullscreen console
	if (pawn == nullptr)
		return;

	int arg[4] = {};
	for (int i = 0; i < argn; ++i)
	{
		int argval = ReadInt32(stream);
		if ((unsigned)i < countof(arg))
			arg[i] = argval;
	}

	P_StartScript(pawn->Level, pawn, nullptr, snum, primaryLevel->MapName.GetChars(), arg, min<int>(countof(arg), argn), ACS_NET | always);
}

// TODO: This really needs to be replaced with some kind of packet system that can simply read through packets and opt
// not to execute them. Right now this is making setting up net commands a nightmare.
// Reads through the network stream but doesn't actually execute any command. Used for getting the size of a stream.
// The skip amount is the number of bytes the command possesses. This should mirror the bytes in Net_DoCommand().
void Net_SkipCommand(int cmd, TArrayView<uint8_t>& stream)
{
	size_t skip = 0;
	switch (cmd)
	{
		case DEM_SAY:
			skip = strlen((char *)(stream.Data() + 1)) + 2;
			break;

		case DEM_ADDBOT:
			skip = strlen((char *)(stream.Data() + 1)) + 6;
			break;

		case DEM_GIVECHEAT:
		case DEM_TAKECHEAT:
			skip = strlen((char *)(stream.Data())) + 5;
			break;

		case DEM_SETINV:
			skip = strlen((char *)(stream.Data())) + 6;
			break;

		case DEM_NETEVENT:
			skip = strlen((char *)(stream.Data())) + 15;
			break;

		case DEM_ZSC_CMD:
			skip = strlen((char*)(stream.Data())) + 1;
			skip += (stream[skip] << 8) | (stream[skip + 1]) + 2;
			break;

		case DEM_SUMMON2:
		case DEM_SUMMONFRIEND2:
		case DEM_SUMMONFOE2:
			skip = strlen((char *)(stream.Data())) + 26;
			break;
		case DEM_CHANGEMAP2:
			skip = strlen((char *)(stream.Data() + 1)) + 2;
			break;
		case DEM_MUSICCHANGE:
		case DEM_PRINT:
		case DEM_CENTERPRINT:
		case DEM_UINFCHANGED:
		case DEM_CHANGEMAP:
		case DEM_SUMMON:
		case DEM_SUMMONFRIEND:
		case DEM_SUMMONFOE:
		case DEM_SUMMONMBF:
		case DEM_REMOVE:
		case DEM_SPRAY:
		case DEM_MORPHEX:
		case DEM_KILLCLASSCHEAT:
		case DEM_MDK:
			skip = strlen((char *)(stream.Data())) + 1;
			break;

		case DEM_WARPCHEAT:
			skip = 6;
			break;

		case DEM_INVUSE:
		case DEM_FOV:
		case DEM_MYFOV:
		case DEM_CHANGESKILL:
			skip = 4;
			break;

		case DEM_INVDROP:
			skip = 8;
			break;

		case DEM_GENERICCHEAT:
		case DEM_DROPPLAYER:
		case DEM_ADDCONTROLLER:
		case DEM_DELCONTROLLER:
		case DEM_KICK:
		case DEM_WEAPSELECT:
		case DEM_MIDGAMESPAWN:
		case DEM_PLAYERDISCONNECT:
			skip = 1;
			break;

		case DEM_MIDGAMEACTIVE:
			skip = 3;
			break;

		case DEM_SAVEGAME:
			skip = strlen((char *)(stream.Data())) + 1;
			skip += strlen((char *)(stream.Data()) + skip) + 1;
			break;

		case DEM_SINFCHANGEDXOR:
		case DEM_SINFCHANGED:
			{
				uint8_t t = stream[0];
				skip = 1 + (t & 63);
				if (cmd == DEM_SINFCHANGED)
				{
					switch (t >> 6)
					{
					case CVAR_Bool:
						skip += 1;
						break;
					case CVAR_Int:
					case CVAR_Float:
						skip += 4;
						break;
					case CVAR_String:
						skip += strlen((char*)(stream.Data() + skip)) + 1;
						break;
					}
				}
				else
				{
					skip += 1;
				}
			}
			break;

		case DEM_RUNSCRIPT:
		case DEM_RUNSCRIPT2:
			skip = 3 + *(stream.Data() + 2) * 4;
			break;

		case DEM_RUNNAMEDSCRIPT:
			skip = strlen((char *)(stream.Data())) + 2;
			skip += ((*(stream.Data() + skip - 1)) & 127) * 4;
			break;

		case DEM_RUNSPECIAL:
			skip = 3 + *(stream.Data() + 2) * 4;
			break;

		case DEM_CONVREPLY:
			skip = 3;
			break;

		case DEM_SETSLOT:
		case DEM_SETSLOTPNUM:
			{
				skip = 2 + (cmd == DEM_SETSLOTPNUM);
				for (int numweapons = stream[skip-1]; numweapons > 0; --numweapons)
					skip += 1 + (stream[skip] >> 7);
			}
			break;

		case DEM_ADDSLOT:
		case DEM_ADDSLOTDEFAULT:
			skip = 2 + (stream[1] >> 7);
			break;

		case DEM_SETPITCHLIMIT:
			skip = 2;
			break;
	}

	AdvanceStream(stream, skip);
}

// This was taken out of shared_hud, because UI code shouldn't do low level calculations that may change if the backing implementation changes.
int Net_GetLatency(int* localDelay, int* arbitratorDelay)
{
	const int gameDelayMs = (ClientTic - gametic) * 1000 / TICRATE;
	int severity = 0;
	if (gameDelayMs >= 160)
		severity = 3;
	else if (gameDelayMs >= 120)
		severity = 2;
	else if (gameDelayMs >= 80)
		severity = 1;
	
	*localDelay = gameDelayMs;
	*arbitratorDelay = ClientStates[consoleplayer].AverageLatency;
	return severity;
}

//==========================================================================
//
//
//
//==========================================================================

// Intermission lobby info
static int IsPlayerReady(int player)
{
	return Net_IsPlayerReady(player);
}

DEFINE_ACTION_FUNCTION_NATIVE(_ScreenJobRunner, IsPlayerReady, IsPlayerReady)
{
	PARAM_PROLOGUE;
	PARAM_INT(player);
	ACTION_RETURN_BOOL(IsPlayerReady(player));
}

static void ReadyPlayer()
{
	if (netgame && !demoplayback)
		Net_WriteInt8(DEM_READIED);
}

DEFINE_ACTION_FUNCTION_NATIVE(_ScreenJobRunner, ReadyPlayer, ReadyPlayer)
{
	PARAM_PROLOGUE;
	ReadyPlayer();
	return 0;
}

static void ResetReadyTimer()
{
	Net_StartCutscene();
}

DEFINE_ACTION_FUNCTION_NATIVE(_ScreenJobRunner, ResetReadyTimer, ResetReadyTimer)
{
	PARAM_PROLOGUE;
	ResetReadyTimer();
	return 0;
}

static int GetReadyTimer()
{
	return CutsceneCountdown;
}

DEFINE_ACTION_FUNCTION_NATIVE(_ScreenJobRunner, GetReadyTimer, GetReadyTimer)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(GetReadyTimer());
}

// [RH] List "ping" times
CCMD(pings)
{
	if (!netgame)
	{
		Printf("This command can only be used when playing in a net game\n");
		return;
	}

	if (NetworkClients.Size() <= 1)
		return;

	for (auto client : NetworkClients)
	{
		if (client != Net_Arbitrator)
			Printf("%ums %s [%d]\n", ClientStates[client].AverageLatency, players[client].userinfo.GetName(), client);
	}
}

CCMD(netlagreset)
{
	ResetNetLagHistories();
	Printf("Network lag metrics reset.\n");
	if (net_lagdump)
		Printf("Dump file reset: %s\n", GetNetLagDumpPath().GetChars());
}

CCMD(netlagdumpfile)
{
	Printf("%s\n", GetNetLagDumpPath().GetChars());
}

CCMD(kick)
{
	if (argv.argc() < 2)
	{
		Printf("Usage: kick <client numbers>\nRemove these clients from the game\n");
		return;
	}

	if (!netgame)
	{
		Printf("This command can only be used when playing in a net game\n");
		return;
	}

	// Dont give settings controllers access to this. That should be reserved as a separate power
	// the host can grant.
	if (consoleplayer != Net_Arbitrator)
	{
		Printf("This command is only accessible to the host\n");
		return;
	}

	TArray<int> cNums = {};
	for (int i = 1; i < argv.argc(); ++i)
	{
		int cNum = -1;
		if (!C_IsValidInt(argv[i], cNum) || cNum < 0 || (size_t)cNum >= MAXPLAYERS)
			Printf("Bad client number %s\n", argv[i]);
		else if (cNum != consoleplayer && cNums.Find(cNum) >= cNums.Size())
			cNums.Push(cNum);
	}

	for (auto cNum : cNums)
	{
		if (!NetworkClients.InGame(cNum))
		{
			Printf("Client %d is not in game\n", cNum);
		}
		else
		{
			Net_WriteInt8(DEM_KICK);
			Net_WriteInt8(cNum);
		}
	}
}

CCMD(mute)
{
	if (argv.argc() < 2)
	{
		Printf("Usage: mute <player numbers>\nDisable messages from these players\n");
		return;
	}

	if (!multiplayer)
	{
		Printf("This command can only be used when playing in multiplayer\n");
		return;
	}

	TArray<int> pNums = {};
	for (int i = 1; i < argv.argc(); ++i)
	{
		int pNum = -1;
		if (!C_IsValidInt(argv[i], pNum) || pNum < 0 || (size_t)pNum >= MAXPLAYERS)
			Printf("Bad player number %s\n", argv[i]);
		else if (pNum != consoleplayer && pNums.Find(pNum) >= pNums.Size())
			pNums.Push(pNum);
	}

	for (auto pNum : pNums)
	{
		if (!playeringame[pNum])
		{
			Printf("Player %d is not in game\n", pNum);
		}
		else
		{
			MutedClients |= (uint64_t)1u << pNum;
			Printf("Muted player %s [%d]\n", players[pNum].userinfo.GetName(), pNum);
		}
	}
}

CCMD(muteall)
{
	if (!multiplayer)
	{
		Printf("This command can only be used when playing in multiplayer\n");
		return;
	}

	for (size_t i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i] && i != (size_t)consoleplayer)
			MutedClients |= (uint64_t)1u << i;
	}
}

CCMD(listmuted)
{
	if (!multiplayer)
	{
		Printf("This command can only be used when playing in multiplayer\n");
		return;
	}

	bool found = false;
	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		if (MutedClients & ((uint64_t)1u << i))
		{
			found = true;
			Printf("%d. %s\n", i, players[i].userinfo.GetName());
		}
	}

	if (!found)
		Printf("No one currently muted\n");
}

CCMD(unmute)
{
	if (argv.argc() < 2)
	{
		Printf("Usage: unmute <player numbers>\nAllow messages from these players again\n");
		return;
	}

	if (!multiplayer)
	{
		Printf("This command can only be used when playing in multiplayer\n");
		return;
	}

	TArray<int> pNums = {};
	for (int i = 1; i < argv.argc(); ++i)
	{
		int pNum = -1;
		if (!C_IsValidInt(argv[i], pNum) || pNum < 0 || (size_t)pNum >= MAXPLAYERS)
			Printf("Bad player number %s\n", argv[i]);
		else if (pNum != consoleplayer && pNums.Find(pNum) >= pNums.Size())
			pNums.Push(pNum);
	}

	for (auto pNum : pNums)
	{
		if (!playeringame[pNum])
		{
			Printf("Player %d is not in game\n", pNum);
		}
		else
		{
			MutedClients &= ~((uint64_t)1u << pNum);
			Printf("Unmuted player %s [%d]\n", players[pNum].userinfo.GetName(), pNum);
		}
	}
}

CCMD(unmuteall)
{
	if (!multiplayer)
	{
		Printf("This command can only be used when playing in multiplayer\n");
		return;
	}

	MutedClients = 0u;
}

//==========================================================================
//
// Net_ChangeSettingsControllers
//
// Implement players who have the ability to change settings in a network
// game.
//
//==========================================================================

static void Net_ChangeSettingsControllers(const TArray<int>& cNums, bool add)
{
	if (!netgame)
	{
		Printf("This command can only be used when playing in a net game\n");
		return;
	}

	if (consoleplayer != Net_Arbitrator)
	{
		Printf("This command is only accessible to the host\n");
		return;
	}

	for (auto cNum : cNums)
	{
		if (cNum == Net_Arbitrator)
		{
			Printf("The host cannot change their own settings controller status\n");
		}
		else if (!NetworkClients.InGame(cNum))
		{
			Printf("Client %d is not in game\n", cNum);
		}
		else if (players[cNum].settings_controller && add)
		{
			Printf("Client %d is already a settings controller\n", cNum);
		}
		else if (!players[cNum].settings_controller && !add)
		{
			Printf("Client %d is already not a settings controller\n", cNum);
		}
		else
		{
			Net_WriteInt8(add ? DEM_ADDCONTROLLER : DEM_DELCONTROLLER);
			Net_WriteInt8(cNum);
		}
	}
}

//==========================================================================
//
// CCMD addsettingscontrollers
//
//==========================================================================

CCMD(addsettingscontrollers)
{
	if (argv.argc() < 2)
	{
		Printf("Usage: addsettingscontrollers <client numbers>\nAllow these clients to control game settings\n");
		return;
	}

	TArray<int> cNums = {};
	for (int i = 1; i < argv.argc(); ++i)
	{
		int cNum = -1;
		if (!C_IsValidInt(argv[i], cNum) || cNum < 0 || (size_t)cNum >= MAXPLAYERS)
			Printf("Bad client number %s\n", argv[i]);
		else if (cNum != Net_Arbitrator && cNums.Find(cNum) >= cNums.Size())
			cNums.Push(cNum);
	}

	Net_ChangeSettingsControllers(cNums, true);
}

//==========================================================================
//
// CCMD removesettingscontrollers
//
//==========================================================================

CCMD(removesettingscontrollers)
{
	if (argv.argc() < 2)
	{
		Printf("Usage: removesettingscontrollers <client numbers>\nRemove the ability for these clients to control game settings\n");
		return;
	}

	TArray<int> cNums = {};
	for (int i = 1; i < argv.argc(); ++i)
	{
		int cNum = -1;
		if (!C_IsValidInt(argv[i], cNum) || cNum < 0 || (size_t)cNum >= MAXPLAYERS)
			Printf("Bad player number %s\n", argv[i]);
		else if (cNum != Net_Arbitrator && cNums.Find(cNum) >= cNums.Size())
			cNums.Push(cNum);
	}

	Net_ChangeSettingsControllers(cNums, false);
}

//==========================================================================
//
// CCMD removeallsettingscontrollers
//
//==========================================================================

CCMD(removeallsettingscontrollers)
{
	TArray<int> cNums = {};
	for (auto client : NetworkClients)
	{
		if (client != Net_Arbitrator && players[client].settings_controller)
			cNums.Push(client);
	}

	Net_ChangeSettingsControllers(cNums, false);
}

//==========================================================================
//
// CCMD listsettingscontrollers
//
//==========================================================================

CCMD(listsettingscontrollers)
{
	if (!netgame)
	{
		Printf("This command can only be used when playing in a net game\n");
		return;
	}

	TArray<int> cNums = {};
	for (auto client : NetworkClients)
	{
		if (client != Net_Arbitrator && players[client].settings_controller)
			cNums.Push(client);
	}

	if (!cNums.Size())
	{
		Printf("No other settings controllers\n");
		return;
	}

	Printf("The following players can change the game settings:\n");
	for (auto cNum : cNums)
	{
		Printf("%d. %s", cNum, players[cNum].userinfo.GetName());
		if (cNum == consoleplayer)
			Printf(" [*]");
		Printf("\n");
	}
}
