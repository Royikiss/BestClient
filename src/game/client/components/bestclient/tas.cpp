/* Copyright © 2026 BestProject Team */
#include "tas.h"

#include <base/io.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/linereader.h>
#include <engine/sound.h>
#include <engine/textrender.h>

#include <game/client/components/bestclient/ui_theme/style.h>
#include <game/client/components/bestclient/ui_theme/widgets.h>
#include <game/client/gameclient.h>
#include <game/client/ui.h>
#include <game/localization.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

int CTas::TasFileListCallback(const char *pName, int IsDir, int DirType, void *pUser)
{
	(void)DirType;
	if(IsDir || !pName || !pUser)
		return 0;

	const char *pExt = str_endswith(pName, ".tas");
	if(!pExt)
		return 0;

	std::vector<std::string> *pvList = static_cast<std::vector<std::string> *>(pUser);
	char aBaseName[128];
	str_copy(aBaseName, pName);
	int Len = str_length(aBaseName);
	if(Len > 4 && str_comp(aBaseName + Len - 4, ".tas") == 0)
		aBaseName[Len - 4] = '\0';

	pvList->push_back(aBaseName);
	return 0;
}

void CTas::OnConsoleInit()
{
	Console()->Register("tas_record", "?i", CFGFLAG_CLIENT, ConTasRecord, this, "Start or restart TAS recording (1=reset, 0=append)");
	Console()->Register("tas_record_toggle", "", CFGFLAG_CLIENT, ConTasToggleRecord, this, "Toggle TAS recording");
	Console()->Register("tas_play", "", CFGFLAG_CLIENT, ConTasPlay, this, "Start TAS playback immediately");
	Console()->Register("tas_play_toggle", "", CFGFLAG_CLIENT, ConTasTogglePlay, this, "Toggle TAS playback");
	Console()->Register("tas_arm", "", CFGFLAG_CLIENT, ConTasArm, this, "Arm TAS playback to trigger automatically on race start line");
	Console()->Register("tas_stop", "", CFGFLAG_CLIENT, ConTasStop, this, "Stop any active TAS recording or playback");
	Console()->Register("tas_save", "s[name]", CFGFLAG_CLIENT, ConTasSave, this, "Save current TAS track to file (in tas/ directory)");
	Console()->Register("tas_load", "s[name]", CFGFLAG_CLIENT, ConTasLoad, this, "Load TAS track from file (in tas/ directory)");
	Console()->Register("tas_save_cp", "", CFGFLAG_CLIENT, ConTasSaveCheckpoint, this, "Save current recording checkpoint");
	Console()->Register("tas_load_cp", "", CFGFLAG_CLIENT, ConTasLoadCheckpoint, this, "Roll back recording to last saved checkpoint");
	Console()->Register("tas_clear", "", CFGFLAG_CLIENT, ConTasClear, this, "Clear in-memory TAS track");
	Console()->Register("tas_status", "", CFGFLAG_CLIENT, ConTasStatus, this, "Print current TAS status");

	RefreshFileList();
}

void CTas::OnReset()
{
	if(m_State == STATE_PLAYING || m_State == STATE_ARMED)
		StopPlayback();
}

void CTas::OnMapLoad()
{
	if(m_State == STATE_PLAYING || m_State == STATE_ARMED)
		StopPlayback();
	const char *pMap = (GameClient()->Map() && GameClient()->Map()->BaseName()) ? GameClient()->Map()->BaseName() : "";
	str_copy(m_aCurrentMap, pMap);
	RefreshFileList();
}

void CTas::OnStateChange(int NewState, int OldState)
{
	(void)OldState;
	if(NewState == IClient::STATE_OFFLINE || NewState == IClient::STATE_CONNECTING || NewState == IClient::STATE_LOADING)
	{
		if(m_State == STATE_PLAYING || m_State == STATE_ARMED)
			StopPlayback();
	}
}

void CTas::RefreshFileList()
{
	m_vFileList.clear();
	Storage()->CreateFolder("tas", IStorage::TYPE_SAVE);
	Storage()->ListDirectory(IStorage::TYPE_ALL, "tas", TasFileListCallback, &m_vFileList);
	std::sort(m_vFileList.begin(), m_vFileList.end());
}

void CTas::StartRecord(bool ResetTrack)
{
	if(!g_Config.m_BcTasEnabled)
		return;

	if(ResetTrack)
	{
		m_vTicks.clear();
		m_CurrentRecordTick = 0;
		m_Checkpoint.m_Valid = false;
	}
	m_State = STATE_RECORDING;
	m_LastRecordGameTick = -1;
	GameClient()->Echo(BcLocalize("TAS recording started."));
}

void CTas::StopRecord()
{
	if(m_State == STATE_RECORDING)
	{
		m_State = STATE_IDLE;
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), BcLocalize("TAS recording stopped. Recorded %d ticks (%.2f seconds)."), (int)m_vTicks.size(), (float)m_vTicks.size() / 50.0f);
		GameClient()->Echo(aBuf);
	}
}

void CTas::ToggleRecord()
{
	if(m_State == STATE_RECORDING)
		StopRecord();
	else
		StartRecord(true);
}

void CTas::ArmPlayback()
{
	if(m_vTicks.empty())
	{
		GameClient()->Echo(BcLocalize("TAS: Cannot arm playback - no track loaded!"));
		return;
	}
	m_State = STATE_ARMED;
	m_PlaybackTick = 0;
	m_LastPlaybackGameTick = -1;
	GameClient()->Echo(BcLocalize("TAS: Playback ARMED. Waiting for race start line..."));
}

void CTas::StartPlayback()
{
	if(m_vTicks.empty())
	{
		GameClient()->Echo(BcLocalize("TAS: Cannot play - no track loaded!"));
		return;
	}
	m_State = STATE_PLAYING;
	m_PlaybackTick = 0;
	m_LastPlaybackGameTick = Client()->PredGameTick(g_Config.m_ClDummy);
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), BcLocalize("TAS: Playback started (%d ticks, %.2fs)."), (int)m_vTicks.size(), (float)m_vTicks.size() / 50.0f);
	GameClient()->Echo(aBuf);
}

void CTas::StopPlayback()
{
	if(m_State == STATE_PLAYING || m_State == STATE_ARMED)
	{
		const bool WasPlaying = m_State == STATE_PLAYING;
		m_State = STATE_IDLE;
		m_PlaybackTick = 0;
		m_LastPlaybackGameTick = -1;
		if(WasPlaying)
			GameClient()->Echo(BcLocalize("TAS: Playback stopped. Human control restored."));
	}
}

void CTas::TogglePlayback()
{
	if(m_State == STATE_PLAYING || m_State == STATE_ARMED)
		StopPlayback();
	else
	{
		if(g_Config.m_BcTasAutoStart)
			ArmPlayback();
		else
			StartPlayback();
	}
}

void CTas::SaveCheckpoint()
{
	if(m_State != STATE_RECORDING)
	{
		GameClient()->Echo(BcLocalize("TAS: Checkpoints can only be saved during recording."));
		return;
	}
	m_Checkpoint.m_Valid = true;
	m_Checkpoint.m_Tick = (int)m_vTicks.size();
	if(GameClient()->m_Snap.m_pLocalCharacter)
	{
		m_Checkpoint.m_Pos = vec2(GameClient()->m_Snap.m_pLocalCharacter->m_X, GameClient()->m_Snap.m_pLocalCharacter->m_Y);
		m_Checkpoint.m_Vel = vec2(GameClient()->m_Snap.m_pLocalCharacter->m_VelX / 256.0f, GameClient()->m_Snap.m_pLocalCharacter->m_VelY / 256.0f);
	}
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), BcLocalize("TAS: Checkpoint saved at tick %d."), m_Checkpoint.m_Tick);
	GameClient()->Echo(aBuf);
}

void CTas::LoadCheckpoint()
{
	if(!m_Checkpoint.m_Valid)
	{
		GameClient()->Echo(BcLocalize("TAS: No checkpoint saved!"));
		return;
	}
	if(m_State == STATE_RECORDING)
	{
		if(m_Checkpoint.m_Tick < (int)m_vTicks.size())
			m_vTicks.resize(m_Checkpoint.m_Tick);
		m_CurrentRecordTick = m_Checkpoint.m_Tick;
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), BcLocalize("TAS: Rolled back to checkpoint tick %d."), m_Checkpoint.m_Tick);
		GameClient()->Echo(aBuf);
	}
}

void CTas::Clear()
{
	StopPlayback();
	StopRecord();
	m_vTicks.clear();
	m_Checkpoint.m_Valid = false;
	m_aLoadedFileName[0] = '\0';
	GameClient()->Echo(BcLocalize("TAS: In-memory track cleared."));
}

bool CTas::SaveToFile(const char *pFilename)
{
	if(!pFilename || !pFilename[0])
		return false;

	if(m_vTicks.empty())
	{
		GameClient()->Echo(BcLocalize("TAS: Nothing to save, track is empty!"));
		return false;
	}

	Storage()->CreateFolder("tas", IStorage::TYPE_SAVE);

	char aPath[256];
	str_format(aPath, sizeof(aPath), "tas/%s.tas", pFilename);

	IOHANDLE File = Storage()->OpenFile(aPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
	{
		GameClient()->Echo(BcLocalize("TAS: Failed to open file for writing!"));
		return false;
	}

	char aLine[512];
	const char *pMap = (GameClient()->Map() && GameClient()->Map()->BaseName()) ? GameClient()->Map()->BaseName() : "unknown";
	str_format(aLine, sizeof(aLine), "# BESTCLIENT_TAS_V1\nMAP %s\nTICKS %d\n", pMap, (int)m_vTicks.size());
	io_write(File, aLine, str_length(aLine));

	for(size_t i = 0; i < m_vTicks.size(); ++i)
	{
		const STasTick &T = m_vTicks[i];
		str_format(aLine, sizeof(aLine),
			"T %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %.2f %.2f\n",
			T.m_Tick,
			T.m_MainInput.m_Direction,
			T.m_MainInput.m_TargetX,
			T.m_MainInput.m_TargetY,
			T.m_MainInput.m_Jump,
			T.m_MainInput.m_Fire,
			T.m_MainInput.m_Hook,
			T.m_MainInput.m_PlayerFlags,
			T.m_MainInput.m_WantedWeapon,
			T.m_HasDummy ? 1 : 0,
			T.m_DummyInput.m_Direction,
			T.m_DummyInput.m_TargetX,
			T.m_DummyInput.m_TargetY,
			T.m_DummyInput.m_Jump,
			T.m_DummyInput.m_Fire,
			T.m_DummyInput.m_Hook,
			T.m_DummyInput.m_PlayerFlags,
			T.m_DummyInput.m_WantedWeapon,
			T.m_Pos.x,
			T.m_Pos.y);
		io_write(File, aLine, str_length(aLine));
	}

	io_close(File);
	str_copy(m_aLoadedFileName, pFilename);
	RefreshFileList();

	char aMsg[128];
	str_format(aMsg, sizeof(aMsg), BcLocalize("TAS: Saved track to '%s' (%d ticks, %.2fs)."), pFilename, (int)m_vTicks.size(), (float)m_vTicks.size() / 50.0f);
	GameClient()->Echo(aMsg);
	return true;
}

bool CTas::LoadFromFile(const char *pFilename)
{
	if(!pFilename || !pFilename[0])
		return false;

	char aPath[256];
	str_format(aPath, sizeof(aPath), "tas/%s.tas", pFilename);

	IOHANDLE File = Storage()->OpenFile(aPath, IOFLAG_READ, IStorage::TYPE_ALL);
	if(!File)
	{
		char aErr[128];
		str_format(aErr, sizeof(aErr), BcLocalize("TAS: File '%s' not found!"), aPath);
		GameClient()->Echo(aErr);
		return false;
	}

	CLineReader LineReader;
	LineReader.OpenFile(File);

	Clear();

	const char *pLine = LineReader.Get();
	if(!pLine || str_comp_num(pLine, "# BESTCLIENT_TAS_V1", 19) != 0)
	{
		GameClient()->Echo(BcLocalize("TAS: Invalid file format header!"));
		return false;
	}

	while((pLine = LineReader.Get()))
	{
		if(pLine[0] == '#' || pLine[0] == '\0')
			continue;

		if(str_comp_num(pLine, "MAP ", 4) == 0)
		{
			str_copy(m_aCurrentMap, pLine + 4);
		}
		else if(str_comp_num(pLine, "TICKS ", 6) == 0)
		{
			int ExpectedTicks = str_toint(pLine + 6);
			if(ExpectedTicks > 0)
				m_vTicks.reserve(ExpectedTicks);
		}
		else if(pLine[0] == 'T' && pLine[1] == ' ')
		{
			STasTick T{};
			int HasDummy = 0;
			float PosX = 0.0f, PosY = 0.0f;
			int ReadCount = std::sscanf(pLine,
				"T %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %f %f",
				&T.m_Tick,
				&T.m_MainInput.m_Direction,
				&T.m_MainInput.m_TargetX,
				&T.m_MainInput.m_TargetY,
				&T.m_MainInput.m_Jump,
				&T.m_MainInput.m_Fire,
				&T.m_MainInput.m_Hook,
				&T.m_MainInput.m_PlayerFlags,
				&T.m_MainInput.m_WantedWeapon,
				&HasDummy,
				&T.m_DummyInput.m_Direction,
				&T.m_DummyInput.m_TargetX,
				&T.m_DummyInput.m_TargetY,
				&T.m_DummyInput.m_Jump,
				&T.m_DummyInput.m_Fire,
				&T.m_DummyInput.m_Hook,
				&T.m_DummyInput.m_PlayerFlags,
				&T.m_DummyInput.m_WantedWeapon,
				&PosX,
				&PosY);

			if(ReadCount >= 9)
			{
				T.m_HasDummy = HasDummy != 0;
				T.m_Pos = vec2(PosX, PosY);
				m_vTicks.push_back(T);
			}
		}
	}

	str_copy(m_aLoadedFileName, pFilename);
	str_copy(g_Config.m_BcTasCurrentFile, pFilename);

	char aMsg[128];
	str_format(aMsg, sizeof(aMsg), BcLocalize("TAS: Loaded '%s' (%d ticks, %.2fs)."), pFilename, (int)m_vTicks.size(), (float)m_vTicks.size() / 50.0f);
	GameClient()->Echo(aMsg);
	return true;
}

void CTas::DeleteTasFile(const char *pFilename)
{
	if(!pFilename || !pFilename[0])
		return;

	char aPath[256];
	str_format(aPath, sizeof(aPath), "tas/%s.tas", pFilename);
	if(Storage()->RemoveFile(aPath, IStorage::TYPE_SAVE))
	{
		char aMsg[128];
		str_format(aMsg, sizeof(aMsg), BcLocalize("TAS: Deleted '%s'."), aPath);
		GameClient()->Echo(aMsg);
		RefreshFileList();
	}
}

int CTas::OnSnapInput(int *pData, bool Dummy, bool Force)
{
	(void)Force;
	if(!g_Config.m_BcTasEnabled || m_State != STATE_PLAYING || !pData)
		return 0;

	if(m_PlaybackTick < 0 || m_PlaybackTick >= (int)m_vTicks.size())
	{
		StopPlayback();
		return 0;
	}

	const STasTick &Tick = m_vTicks[m_PlaybackTick];
	if(!Dummy)
	{
		mem_copy(pData, &Tick.m_MainInput, sizeof(CNetObj_PlayerInput));
		return sizeof(CNetObj_PlayerInput);
	}
	else if(g_Config.m_BcTasPlaybackDummy && Tick.m_HasDummy)
	{
		mem_copy(pData, &Tick.m_DummyInput, sizeof(CNetObj_PlayerInput));
		return sizeof(CNetObj_PlayerInput);
	}
	return 0;
}

void CTas::OnRecordInput(const int *pData, bool Dummy)
{
	if(!g_Config.m_BcTasEnabled || m_State != STATE_RECORDING || !pData)
		return;

	int CurTick = Client()->PredGameTick(g_Config.m_ClDummy);
	if(!Dummy)
	{
		if(m_LastRecordGameTick != CurTick)
		{
			m_LastRecordGameTick = CurTick;
			STasTick NewTick{};
			NewTick.m_Tick = (int)m_vTicks.size();
			mem_copy(&NewTick.m_MainInput, pData, sizeof(CNetObj_PlayerInput));
			if(GameClient()->m_Snap.m_pLocalCharacter)
			{
				NewTick.m_Pos = vec2(GameClient()->m_Snap.m_pLocalCharacter->m_X, GameClient()->m_Snap.m_pLocalCharacter->m_Y);
				NewTick.m_Vel = vec2(GameClient()->m_Snap.m_pLocalCharacter->m_VelX / 256.0f, GameClient()->m_Snap.m_pLocalCharacter->m_VelY / 256.0f);
			}
			m_vTicks.push_back(NewTick);
			m_CurrentRecordTick = (int)m_vTicks.size();
		}
	}
	else
	{
		if(!m_vTicks.empty())
		{
			STasTick &Last = m_vTicks.back();
			mem_copy(&Last.m_DummyInput, pData, sizeof(CNetObj_PlayerInput));
			Last.m_HasDummy = true;
		}
	}
}

void CTas::PrepareInputForSend(int *pData, int Size, bool Dummy)
{
	if(!g_Config.m_BcTasEnabled || m_State != STATE_PLAYING || !pData)
		return;

	int CurTick = Client()->PredGameTick(g_Config.m_ClDummy);
	if(!Dummy)
	{
		if(m_LastPlaybackGameTick != -1 && m_LastPlaybackGameTick != CurTick)
		{
			m_PlaybackTick++;
		}
		m_LastPlaybackGameTick = CurTick;
	}

	if(m_PlaybackTick < 0 || m_PlaybackTick >= (int)m_vTicks.size())
	{
		StopPlayback();
		return;
	}

	const STasTick &Tick = m_vTicks[m_PlaybackTick];
	if(!Dummy)
	{
		if(Size >= (int)sizeof(CNetObj_PlayerInput))
			mem_copy(pData, &Tick.m_MainInput, sizeof(CNetObj_PlayerInput));
	}
	else if(g_Config.m_BcTasPlaybackDummy && Tick.m_HasDummy)
	{
		if(Size >= (int)sizeof(CNetObj_PlayerInput))
			mem_copy(pData, &Tick.m_DummyInput, sizeof(CNetObj_PlayerInput));
	}
}

bool CTas::OnInput(const IInput::CEvent &Event)
{
	if(!g_Config.m_BcTasEnabled || m_State != STATE_PLAYING)
		return false;

	if(g_Config.m_BcTasAutoStopOnInput && (Event.m_Flags & IInput::FLAG_PRESS))
	{
		const int Key = Event.m_Key;
		if(Key == KEY_A || Key == KEY_D || Key == KEY_W || Key == KEY_S || Key == KEY_SPACE || Key == KEY_MOUSE_1 || Key == KEY_MOUSE_2)
		{
			StopPlayback();
			GameClient()->Echo(BcLocalize("TAS: Manual user input detected, playback interrupted."));
			return false;
		}
	}
	return false;
}

void CTas::CheckRaceAutoTrigger()
{
	if(m_State != STATE_ARMED)
		return;

	// Race line detection
	if(GameClient()->RaceHelper() && GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalPrevCharacter)
	{
		vec2 PrevPos = vec2(GameClient()->m_Snap.m_pLocalPrevCharacter->m_X, GameClient()->m_Snap.m_pLocalPrevCharacter->m_Y);
		vec2 Pos = vec2(GameClient()->m_Snap.m_pLocalCharacter->m_X, GameClient()->m_Snap.m_pLocalCharacter->m_Y);
		if(GameClient()->RaceHelper()->IsStart(PrevPos, Pos))
		{
			StartPlayback();
			return;
		}
	}

	// Server race tick change detection
	if(GameClient()->LastRaceTick() != -1 && GameClient()->LastRaceTick() != m_LastRaceTickSeen)
	{
		m_LastRaceTickSeen = GameClient()->LastRaceTick();
		StartPlayback();
		return;
	}
}

void CTas::OnRender()
{
	CheckRaceAutoTrigger();

	// Check if player died during playback
	if(m_State == STATE_PLAYING)
	{
		if(!GameClient()->m_Snap.m_pLocalCharacter)
		{
			StopPlayback();
			GameClient()->Echo(BcLocalize("TAS: Player died, playback aborted."));
			return;
		}
	}

	if(g_Config.m_BcTasShowHud)
		RenderHud();
}

void CTas::RenderHud()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	if(m_State == STATE_IDLE && m_vTicks.empty())
		return;

	CUIRect Screen = *Ui()->Screen();
	const float BoxW = 210.0f;
	const float BoxH = 50.0f;
	const float MarginX = 10.0f;
	const float MarginY = 70.0f;

	CUIRect HudRect;
	HudRect.x = Screen.w - BoxW - MarginX;
	HudRect.y = MarginY;
	HudRect.w = BoxW;
	HudRect.h = BoxH;

	// Background
	ColorRGBA BgColor(0.08f, 0.08f, 0.12f, 0.85f);
	ColorRGBA BorderColor(0.25f, 0.35f, 0.50f, 0.90f);
	ColorRGBA BadgeColor(0.5f, 0.5f, 0.5f, 1.0f);
	const char *pStateName = BcLocalize("IDLE");

	if(m_State == STATE_RECORDING)
	{
		BadgeColor = ColorRGBA(0.95f, 0.25f, 0.25f, 1.0f);
		BorderColor = ColorRGBA(0.85f, 0.25f, 0.25f, 0.95f);
		pStateName = BcLocalize("REC");
	}
	else if(m_State == STATE_PLAYING)
	{
		BadgeColor = ColorRGBA(0.25f, 0.95f, 0.40f, 1.0f);
		BorderColor = ColorRGBA(0.25f, 0.85f, 0.40f, 0.95f);
		pStateName = BcLocalize("PLAY");
	}
	else if(m_State == STATE_ARMED)
	{
		BadgeColor = ColorRGBA(0.95f, 0.85f, 0.25f, 1.0f);
		BorderColor = ColorRGBA(0.95f, 0.85f, 0.25f, 0.95f);
		pStateName = BcLocalize("ARMED");
	}

	HudRect.Draw(BgColor, IGraphics::CORNER_ALL, 6.0f);

	CUIRect Header, Content, ProgressBar;
	HudRect.Margin(5.0f, &Content);
	Content.HSplitTop(18.0f, &Header, &Content);

	// State Badge
	CUIRect BadgeRect;
	Header.VSplitLeft(55.0f, &BadgeRect, &Header);
	BadgeRect.Draw(BadgeColor, IGraphics::CORNER_ALL, 4.0f);
	TextRender()->TextColor(0.05f, 0.05f, 0.05f, 1.0f);
	Ui()->DoLabel(&BadgeRect, pStateName, 11.0f, TEXTALIGN_MC);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

	// File Name
	Header.VSplitLeft(6.0f, nullptr, &Header);
	const char *pDisplayFile = m_aLoadedFileName[0] ? m_aLoadedFileName : (m_State == STATE_RECORDING ? BcLocalize("[recording]") : BcLocalize("[none]"));
	Ui()->DoLabel(&Header, pDisplayFile, 11.0f, TEXTALIGN_ML);

	// Progress & Ticks
	Content.HSplitBottom(6.0f, &Content, &ProgressBar);
	int Cur = m_State == STATE_PLAYING ? m_PlaybackTick : (m_State == STATE_RECORDING ? m_CurrentRecordTick : 0);
	int Total = std::max(1, (int)m_vTicks.size());
	float Fraction = std::clamp((float)Cur / (float)Total, 0.0f, 1.0f);

	char aTickBuf[64];
	str_format(aTickBuf, sizeof(aTickBuf), BcLocalize("Tick: %d / %d (%.2fs)"), Cur, Total, (float)Cur / 50.0f);
	Ui()->DoLabel(&Content, aTickBuf, 10.0f, TEXTALIGN_ML);

	// Progress Bar
	ProgressBar.Draw(ColorRGBA(0.18f, 0.18f, 0.22f, 1.0f), IGraphics::CORNER_ALL, 2.0f);
	if(Fraction > 0.001f)
	{
		CUIRect Filled = ProgressBar;
		Filled.w *= Fraction;
		Filled.Draw(BadgeColor, IGraphics::CORNER_ALL, 2.0f);
	}
}

// Console Callbacks
void CTas::ConTasRecord(IConsole::IResult *pResult, void *pUserData)
{
	CTas *pThis = static_cast<CTas *>(pUserData);
	const bool Reset = pResult->NumArguments() > 0 ? (pResult->GetInteger(0) != 0) : true;
	pThis->StartRecord(Reset);
}

void CTas::ConTasToggleRecord(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CTas *>(pUserData)->ToggleRecord();
}

void CTas::ConTasPlay(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CTas *>(pUserData)->StartPlayback();
}

void CTas::ConTasTogglePlay(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CTas *>(pUserData)->TogglePlayback();
}

void CTas::ConTasArm(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CTas *>(pUserData)->ArmPlayback();
}

void CTas::ConTasStop(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CTas *>(pUserData)->StopPlayback();
	static_cast<CTas *>(pUserData)->StopRecord();
}

void CTas::ConTasSave(IConsole::IResult *pResult, void *pUserData)
{
	CTas *pThis = static_cast<CTas *>(pUserData);
	const char *pName = pResult->GetString(0);
	if(!pName || !pName[0])
		pName = g_Config.m_BcTasCurrentFile;
	pThis->SaveToFile(pName);
}

void CTas::ConTasLoad(IConsole::IResult *pResult, void *pUserData)
{
	CTas *pThis = static_cast<CTas *>(pUserData);
	const char *pName = pResult->GetString(0);
	if(!pName || !pName[0])
		pName = g_Config.m_BcTasCurrentFile;
	pThis->LoadFromFile(pName);
}

void CTas::ConTasSaveCheckpoint(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CTas *>(pUserData)->SaveCheckpoint();
}

void CTas::ConTasLoadCheckpoint(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CTas *>(pUserData)->LoadCheckpoint();
}

void CTas::ConTasClear(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CTas *>(pUserData)->Clear();
}

void CTas::ConTasStatus(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	CTas *pThis = static_cast<CTas *>(pUserData);
	const char *apStateStr[] = {"IDLE", "RECORDING", "ARMED", "PLAYING", "PAUSED"};
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf),
		"[TAS] State: %s | Ticks: %d (%.2fs) | Playback: %d | File: '%s' | Checkpoint: %s",
		apStateStr[pThis->m_State],
		(int)pThis->m_vTicks.size(),
		(float)pThis->m_vTicks.size() / 50.0f,
		pThis->m_PlaybackTick,
		pThis->m_aLoadedFileName[0] ? pThis->m_aLoadedFileName : "<none>",
		pThis->m_Checkpoint.m_Valid ? "Yes" : "No");
	pThis->GameClient()->Echo(aBuf);
}
