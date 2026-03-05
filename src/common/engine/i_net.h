/*
** i_net.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 1993-1996 by id Software, Inc.
** Copyright 1999-2016 Marisa Heit
** Copyright 2009-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: LicenseRef-Doom-Source-License
**
**---------------------------------------------------------------------------
**
*/

#ifndef __I_NET_H__
#define __I_NET_H__

#include <stdint.h>
#include "m_argv.h"

inline constexpr size_t MAXPLAYERS = 64u;

EXTERN_FARG(host);
EXTERN_FARG(join);
EXTERN_FARG(dedicated);

enum ENetConstants
{
	BACKUPTICS = 35 * 5,	// Remember up to 5 seconds of data.
	MAXTICDUP = 3,
	MAXSENDTICS = 35 * 1,	// Only send up to 1 second of data at a time.
	STABILITYTICS = 17,
	LOCALCMDTICS = (BACKUPTICS * MAXTICDUP),
	MAX_MSGLEN = 14000,
};

enum ENetCommand
{
	CMD_NONE,
	CMD_SEND,
	CMD_GET,
};

enum ENetFlags
{
	NCMD_EXIT = 0x80,		// Client has left the game
	NCMD_RETRANSMIT = 0x40,		// 
	NCMD_SETUP = 0x20,		// Guest is letting the host know who it is
	NCMD_LEVELREADY = 0x10,		// After loading a level, guests send this over to the host who then sends it back after all are received
	NCMD_QUITTERS = 0x08,		// Client is getting info about one or more players quitting
	NCMD_COMPRESSED = 0x04,		// Remainder of packet is compressed
	NCMD_LATENCYACK = 0x02,		// A latency packet was just read, so let the sender know.
	NCMD_LATENCY = 0x01,		// Latency packet, used for measuring RTT.		
};

struct FVerificationError
{
	enum EVerifyError : uint8_t
	{
		VE_NONE,
		VE_ENGINE,
		VE_FILE_UNKNOWN,
		VE_FILE_MISSING,
		VE_FILE_ORDER,
	};

	EVerifyError Error = VE_NONE;
	uint8_t Major = 0u, Minor = 0u, Revision = 0u;
	uint8_t NetMajor = 0u, NetMinor = 0u, NetRevision = 0u;
	TArray<FString> UnknownFiles = {};
	TArray<FString> ExpectedOrder = {};
	// Since the guest didn't load these, we have no checksum to fetch the proper name, so send over our own.
	TArray<FString> MissingFiles = {};
};

struct FClientStack : public TArray<int>
{
	inline bool InGame(int i) const { return Find(i) < Size(); }

	void operator+=(const int i)
	{
		if (!InGame(i))
			SortedInsert(i);
	}

	void operator-=(const int i)
	{
		Delete(Find(i));
	}
};

extern bool netgame, multiplayer;
extern int consoleplayer;
extern int Net_Arbitrator;
extern FClientStack NetworkClients;
extern uint8_t NetBuffer[MAX_MSGLEN];
extern size_t NetBufferLength;
extern uint8_t TicDup;
extern int RemoteClient;
extern int MaxClients;

bool I_InitNetwork();
void I_ClearClient(size_t client);
void I_NetCmd(ENetCommand cmd);
void I_NetDone();
enum ENetConnectType : uint8_t
{
	PRE_HEARTBEAT,			// Clients are keeping each other's connections alive
	PRE_CONNECT,			// Sent from guest to host for initial connection
	PRE_CONNECT_ACK,		// Sent from host to guest to confirm they've been connected
	PRE_DISCONNECT,			// Sent from host to guest when another guest leaves
	PRE_USER_INFO,			// Clients are sending each other user infos
	PRE_USER_INFO_ACK,		// Clients are confirming sent user infos
	PRE_GAME_INFO,			// Sent from host to guest containing general game info
	PRE_GAME_INFO_ACK,		// Sent from guest to host confirming game info was gotten
	PRE_GO,					// Sent from host to guest telling them to start the game

	PRE_FULL,				// Sent from host to guest if the lobby is full
	PRE_IN_PROGRESS,		// Sent from host to guest if the game has already started
	PRE_WRONG_PASSWORD,		// Sent from host to guest if their provided password was wrong
	PRE_VERIFICATION_ERROR,	// Sent from host to guest if something failed during the verification step.
	PRE_KICKED,				// Sent from host to guest if the host kicked them from the game
	PRE_BANNED,				// Sent from host to guest if the host banned them from the game

	// In-game disconnect handshake
	PRE_DISCONNECT_REQUEST,	// Client -> Host: "I want to leave"
	PRE_DISCONNECT_ACK,		// Host -> Client: "Acknowledged, you may leave"
	PRE_DISCONNECT_NOTIFY,	// Host -> All: "Client N has left" (also used for host leaving with nextHost)
	PRE_DISCONNECT_CONFIRM,	// All -> Host: "Got the disconnect notification"

	// Host migration
	PRE_MIGRATION_BEGIN,	// Old host -> New host: "You are the new host"
	PRE_MIGRATION_STATE,	// Old host -> New host: serialized host state
	PRE_MIGRATION_STATE_ACK,// New host -> Old host: "Got the state"
	PRE_MIGRATION_COMPLETE,	// New host -> All: "I am the new host, resume"
	PRE_MIGRATION_READY,	// All -> New host: "Acknowledged new host"

	// Mid-game join infrastructure
	PRE_MIDGAME_CONNECT,	// New client -> Host: "I want to join mid-game"
	PRE_MIDGAME_ACCEPT,		// Host -> New client: "Slot assigned, wait for state"
	PRE_MIDGAME_REJECT,		// Host -> New client: "Cannot join" + reason
	PRE_MIDGAME_PLAYER_JOIN,// Host -> All existing: "Player N is joining"
	PRE_MIDGAME_PLAYER_ACK,	// All -> Host: "Acknowledged new player"

	// Mid-game state transfer (snapshot-based)
	PRE_MIDGAME_STATE_BEGIN,	// Host -> Joiner: metadata (map, sizes, gametic)
	PRE_MIDGAME_STATE_CHUNK,	// Host -> Joiner: one chunk of state data
	PRE_MIDGAME_STATE_CHUNK_ACK,// Joiner -> Host: ACK for chunk N
	PRE_MIDGAME_STATE_COMPLETE,	// Host -> Joiner: all chunks sent
	PRE_MIDGAME_STATE_READY,	// Joiner -> Host: initialized, ready for state
	PRE_MIDGAME_STATE_LOADED,	// Joiner -> Host: snapshot loaded successfully
	PRE_MIDGAME_STATE_ERROR,	// Joiner -> Host: error, abort transfer
};

enum EMidgameRejectReason : uint8_t
{
	REJECT_FULL,
	REJECT_IN_TRANSITION,
	REJECT_BANNED,
	REJECT_PASSWORD,
	REJECT_VERIFICATION,
	REJECT_DISABLED,
};

void HandleIncomingConnection();
void CloseNetwork();
void I_SetClientAddress(int client);
void I_SendSetupPacket(int client, const uint8_t* data, size_t size);
void I_SendSetupPacketToAddress(const uint8_t* data, size_t size);
void I_GetGameID(uint8_t out[8]);
void I_SetGameID(const uint8_t in[8]);
bool I_IsAddressBanned();

#endif
