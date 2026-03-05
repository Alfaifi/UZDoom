/*
** d_net.h
**
** Networking stuff.
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

#ifndef __D_NET__
#define __D_NET__

#include "doomtype.h"
#include "doomdef.h"
#include "d_protocol.h"
#include "i_net.h"
#include <queue>

uint64_t I_msTime();
struct particle_t;

enum EChatType
{
	CHAT_DISABLED,
	CHAT_TEAM_ONLY,
	CHAT_GLOBAL,
};

enum EClientFlags
{
	CF_NONE = 0,
	CF_QUIT = 1,		// If set, this client sent an exit command and needs to be disconnected.
	CF_MISSING_SEQ = 1 << 1,	// If a sequence was missed/out of order, ask this client to send back over their info.
	CF_RETRANSMIT_SEQ = 1 << 2,	// If set, this client needs command data resent to them.
	CF_MISSING_CON = 1 << 3,	// If a consistency was missed/out of order, ask this client to send back over their info.
	CF_RETRANSMIT_CON = 1 << 4,	// If set, this client needs consistency data resent to them.
	CF_UPDATED = 1 << 5,	// Got an updated packet from this client.
	CF_DISCONNECT_PENDING = 1 << 6,		// Client has requested disconnect, waiting for ACK propagation.
	CF_DISCONNECT_NOTIFIED = 1 << 7,	// Host has notified others about this client's departure.
	CF_JOINING = 1 << 8,				// Client is in the process of joining mid-game.
	CF_AWAITING_STATE = 1 << 9,			// Client is waiting for game state transfer.

	CF_RETRANSMIT = CF_RETRANSMIT_CON | CF_RETRANSMIT_SEQ,
	CF_MISSING = CF_MISSING_CON | CF_MISSING_SEQ,
};

class FDynamicBuffer
{
public:
	FDynamicBuffer();
	~FDynamicBuffer();

	void SetData(const uint8_t* data, int len);
	uint8_t* GetData(int* len = nullptr);
	TArrayView<uint8_t> GetTArrayView();

private:
	uint8_t* m_Data;
	int m_Len, m_BufferLen;
};

// New packet structure:
//
//  One byte for the net command flags.
//  Four bytes for the last sequence we got from that client.
//  Four bytes for the last consistency we got from that client.
//  If NCMD_QUITTERS set, one byte for the number of players followed by one byte for each player's consolenum.
//  One byte for the number of players.
//  One byte for the number of tics.
//   If > 0, four bytes for the base sequence being worked from.
//  One byte for the number of world tics ran.
//   If > 0, four bytes for the base consistency being worked from.
//  If from the host, one byte for how far ahead of the host we are.
//  For each player:
//   One byte for the player number.
//	 If from the host, two bytes for the latency to the host.
//   For each consistency:
//    One byte for the delta from the base consistency.
//    Two bytes for each consistency.
//   For each tic:
//    One byte for the delta from the base sequence.
//    The remaining command and event data for that player.
struct FClientNetState
{
	// Networked client data.
	struct FNetTic {
		FDynamicBuffer	Data;
		usercmd_t		Command;
	} Tics[BACKUPTICS] = {};

	// Local information about client.
	uint8_t		CurrentLatency = 0u;		// Current latency id the client is on. If the one the client sends back is > this, update RecvTime and mark a new SentTime.
	bool		bNewLatency = true;			// If the sequence was bumped, the next latency packet sent out should record the send time.
	uint16_t	AverageLatency = 0u;		// Calculate the average latency every second or so, that way it doesn't give huge variance in the scoreboard.
	uint64_t	SentTime[MAXSENDTICS] = {};	// Timestamp for when we sent out the packet to this client.
	uint64_t	RecvTime[MAXSENDTICS] = {};	// Timestamp for when the client acknowledged our last packet.

	int				Flags = 0;				// State of this client.
	uint64_t		LastPacketReceivedTime = 0;	// Timestamp of last valid packet from this client, for timeout detection.

	uint8_t			StabilityBuffer = 0u;	// Account for if the client is trying to stabilize when measuring their performance.
	uint8_t			ResendID = 0u;			// Make sure that if the retransmit happened on a wait barrier, it can be properly resent back over.
	int				ResendSequenceFrom = -1; // If >= 0, send from this sequence up to the most recent one, capped to MAXSENDTICS.
	int				SequenceAck = -1;		// The last sequence the client reported from us.
	int 			CurrentSequence = -1;	// The last sequence we've gotten from this client.

	// Every packet includes consistencies for tics that client ran. When
	// a world tic is ran, the local client will store all the consistencies
	// of the clients in their LocalConsistency. Then the consistencies will
	// be checked against retroactively as they come in.
	int ResendConsistencyFrom = -1;				// If >= 0, send from this consistency up to the most recent one, capped to MAXSENDTICS.
	int ConsistencyAck = -1;					// Last consistency the client reported from us.
	int LastVerifiedConsistency = -1;			// Last consistency we checked from this client. If < CurrentNetConsistency, run through them.
	int CurrentNetConsistency = -1;				// Last consistency we got from this client.
	int16_t NetConsistency[BACKUPTICS] = {};	// Consistencies we got from this client.
	int16_t LocalConsistency[BACKUPTICS] = {};	// Local consistency of the client to check against.
};

// Create any new ticcmds and broadcast to other players.
void NetUpdate(int tics);

// Broadcasts special packets to other players
//	to notify of game exit
void D_QuitNetGame (void);

//? how many ticks to run?
void TryRunTics (void);

// [RH] Functions for making and using special "ticcmds"
void Net_NewClientTic();
void Net_Initialize();
void Net_WriteInt8(uint8_t);
void Net_WriteInt16(int16_t);
void Net_WriteInt32(int32_t);
void Net_WriteInt64(int64_t);
void Net_WriteFloat(float);
void Net_WriteDouble(double);
void Net_WriteString(const char *);
void Net_WriteBytes(const uint8_t *, int len);

void Net_DoCommand(int cmd, TArrayView<uint8_t>& stream, int player);
void Net_SkipCommand(int cmd, TArrayView<uint8_t>& stream);

bool Net_CheckCutsceneReady();
void Net_AdvanceCutscene();
void Net_ResetCommands(bool midTic);
void Net_SetWaiting();
void Net_ClearBuffers();
bool Net_IsWaiting();
double Net_ModifyFrac(double ticFrac);
double Net_ModifyObjectFrac(DObject* obj, double ticFrac);
double Net_ModifyParticleFrac(particle_t* part, double ticFrac);

// True on clients connected to a dedicated server (host player 0 is a ghost).
extern bool					hostIsDedicated;

// Netgame stuff (buffers and pointers, i.e. indices).

extern usercmd_t			LocalCmds[LOCALCMDTICS];
extern int					ClientTic;
extern FClientNetState		ClientStates[MAXPLAYERS];

class player_t;
class DObject;

// Host migration state transferred from departing host to new host.
struct FMigrationClientData
{
	int		clientNum;
	int		currentSequence;
	int		sequenceAck;
	int		currentNetConsistency;
	int		consistencyAck;
	int		lastVerifiedConsistency;
	int		flags;
	uint16_t averageLatency;
};

struct FMigrationState
{
	int			currentConsistency;
	int			lastSentConsistency;
	uint8_t		currentLobbyID;
	uint64_t	mutedClients;
	uint64_t	cutsceneReady;
	int			clientCount;
	FMigrationClientData clients[MAXPLAYERS];
};

// Disconnect handshake handlers (called from HandleIncomingConnection in i_net.cpp)
void HandleDisconnectRequest();
void HandleDisconnectAck();
void HandleDisconnectNotify();
void HandleDisconnectConfirm();

// Host migration handlers
void HandleMigrationBegin();
void HandleMigrationState();
void HandleMigrationStateAck();
void HandleMigrationComplete();
void HandleMigrationReady();

// Mid-game join handlers
void HandleMidgameConnect();
void HandleMidgameAccept();
void HandleMidgameReject();
void HandleMidgamePlayerJoin();
void HandleMidgamePlayerAck();

// Mid-game state transfer handlers
void HandleMidgameStateBegin();
void HandleMidgameStateChunk();
void HandleMidgameStateChunkAck();
void HandleMidgameStateComplete();
void HandleMidgameStateLoaded();
void HandleMidgameStateError();

void Net_PrepareMidgameSync();
extern struct FStateTransferRecv IncomingStateTransfer;

// Chunk size for mid-game state transfer (fits in MaxTransmitSize after headers + compression).
constexpr size_t STATE_CHUNK_PAYLOAD = 6000u;
constexpr int STATE_TRANSFER_TIMEOUT_MS = 500;
constexpr int STATE_TRANSFER_MAX_RETRIES = 20;
constexpr uint64_t STATE_TRANSFER_TOTAL_TIMEOUT_MS = 60000;	// Abort entire transfer after 60 seconds

// Host-side: tracks outgoing state transfer to a joining client.
struct FStateTransferSend
{
	bool		active = false;
	int			clientNum = -1;
	TArray<uint8_t>	data;			// Globals + snapshot, concatenated
	size_t		globalsSize = 0;	// Size of globals portion within data
	FString		mapName;
	int			hostGametic = 0;
	int			hostConsistency = 0;
	uint8_t		hostLobbyID = 0;
	size_t		numChunks = 0;
	size_t		nextChunkToSend = 0;
	uint64_t	lastSendTime = 0;
	uint64_t	transferStartTime = 0;	// Time when transfer began (for total timeout)
	int			retryCount = 0;

	void Clear() { *this = FStateTransferSend{}; }
};

// Client-side: tracks incoming state transfer from the host.
struct FStateTransferRecv
{
	bool		active = false;
	bool		loadedSent = false;	// True after STATE_LOADED was sent to host
	TArray<uint8_t>	data;			// Reassembly buffer
	size_t		totalSize = 0;
	size_t		globalsSize = 0;
	FString		mapName;
	int			hostGametic = 0;
	int			hostConsistency = 0;
	uint8_t		hostLobbyID = 0;
	size_t		numChunks = 0;
	size_t		nextExpectedChunk = 0;

	void Clear() { *this = FStateTransferRecv{}; }
};

#endif
