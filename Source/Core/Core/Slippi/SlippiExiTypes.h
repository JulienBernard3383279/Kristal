#pragma once

#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"

#define REPORT_PLAYER_COUNT 4

namespace SlippiExiTypes
{
// Using pragma pack here will remove any structure padding which is what EXI comms expect
// https://www.geeksforgeeks.org/how-to-avoid-structure-padding-in-c/
// Note that none of these classes should be used outside of the handler function, pragma pack
// is supposedly not very efficient?
#pragma pack(1)

struct ReportGameQueryPlayer
{
	u8 slotType;
	u8 stocksRemaining;
	float damageDone;
	u8 syncedStocksRemaining;
	u16 syncedCurrentHealth;
};

struct ReportGameQuery
{
	u8 command;
	u8 onlineMode;
	u32 frameLength;
	u32 gameIndex;
	u32 tiebreakIndex;
	s8 winnerIdx;
	u8 gameEndMethod;
	s8 lrasInitiator;
	u32 syncedTimer;
	ReportGameQueryPlayer players[REPORT_PLAYER_COUNT];
	u8 gameInfoBlock[312];
};

struct ReportSetCompletionQuery
{
	u8 command;
	u8 endMode;
};

struct ReportMatchStatusUpdateQuery
{
	u8 command;
	u8 statusIdx;
};

struct GpCompleteStepQuery
{
	u8 command;
	u8 step_idx;
	u8 char_selection;
	u8 char_color_selection;
	u8 stage_selections[2];
};

struct GpFetchStepQuery
{
	u8 command;
	u8 step_idx;
};

struct GpFetchStepResponse
{
	u8 is_found;
	u8 is_skip;
	u8 char_selection;
	u8 char_color_selection;
	u8 stage_selections[2];
};

struct OverwriteCharSelections
{
	u8 is_set;
	u8 char_id;
	u8 char_color_id;
};
struct OverwriteSelectionsQuery
{
	u8 command;
	u16 stage_id;
	OverwriteCharSelections chars[4];
};

struct PlayerSettings
{
	char chatMessages[16][51];
};
struct GetPlayerSettingsResponse
{
	PlayerSettings settings[4];
};

struct PlayMusicQuery
{
	u8 command;
	u32 offset;
	u32 size;
};

struct ChangeMusicVolumeQuery
{
	u8 command;
	u8 volume;
};

struct ReceiveSfxQuery
{
	u8 command;  // Should be CMD_PLAY_SFX (0x42)
	u16 sfx_id;  // The sound effect ID
	u8 volume;   // The volume
	u8 pitch;    // The pitch
	u8 channel; // The requested channel
	u8 flags;   // The playback flags
	u32 axpbPointer;
};
struct SFXLoadInfoPacket
{
	uint8_t command; // Should be 0x44
	int32_t entrynum;
	int32_t bankID;
	char filename[48];
};
struct AIInitDMAPacket
{
	u8 command; // Should be 0x45
	u32 bufferPtr;
	u32 size;
};

// Not sure if resetting is strictly needed, might be contained to the file
#pragma pack()

template <typename T> inline T Convert(u8 *payload)
{
	return *reinterpret_cast<T *>(payload);
}

// Here we define custom convert functions for any type that larger than u8 sized fields to convert from big-endian

template <> inline ReportGameQuery Convert(u8 *payload)
{
	auto q = *reinterpret_cast<ReportGameQuery *>(payload);
	q.frameLength = Common::FromBigEndian(q.frameLength);
	q.gameIndex = Common::FromBigEndian(q.gameIndex);
	q.tiebreakIndex = Common::FromBigEndian(q.tiebreakIndex);
	q.syncedTimer = Common::FromBigEndian(q.syncedTimer);
	for (int i = 0; i < REPORT_PLAYER_COUNT; i++)
	{
		auto *p = &q.players[i];
		p->damageDone = Common::FromBigEndian(p->damageDone);
		p->syncedCurrentHealth = Common::FromBigEndian(p->syncedCurrentHealth);
	}
	return q;
}

template <> inline OverwriteSelectionsQuery Convert(u8 *payload)
{
	auto q = *reinterpret_cast<OverwriteSelectionsQuery *>(payload);
	q.stage_id = Common::FromBigEndian(q.stage_id);
	return q;
}

template <> inline PlayMusicQuery Convert(u8* payload)
{
	auto q = *reinterpret_cast<PlayMusicQuery *>(payload);
	q.offset = Common::FromBigEndian(q.offset);
	q.size = Common::FromBigEndian(q.size);
	return q;
}

template <> inline ReceiveSfxQuery Convert(u8 *payload)
{
	auto q = *reinterpret_cast<ReceiveSfxQuery *>(payload);
	q.sfx_id = Common::FromBigEndian(q.sfx_id);
	q.axpbPointer = Common::FromBigEndian(q.axpbPointer); // For pointers too ?
	return q;
}
template <> inline SFXLoadInfoPacket Convert(u8 *payload)
{
	auto q = *reinterpret_cast<SFXLoadInfoPacket *>(payload);
	q.bankID = Common::FromBigEndian(q.bankID);
	q.entrynum = Common::FromBigEndian(q.entrynum); // For pointers too ?
	return q;
}
template <> inline AIInitDMAPacket Convert(u8 *payload)
{
	auto q = *reinterpret_cast<AIInitDMAPacket *>(payload);
	q.bufferPtr = Common::FromBigEndian(q.bufferPtr);
	q.size = Common::FromBigEndian(q.size);
	return q;
}

}; // namespace SlippiExiTypes
