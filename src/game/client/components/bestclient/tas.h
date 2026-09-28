/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_TAS_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_TAS_H

#include <base/vmath.h>

#include <engine/console.h>
#include <engine/input.h>
#include <engine/storage.h>

#include <generated/protocol.h>

#include <game/client/component.h>
#include <game/gamecore.h>

#include <string>
#include <vector>

class CCharacter;

class CTas : public CComponent
{
public:
	enum ETasState
	{
		STATE_IDLE = 0,
		STATE_RECORDING,
		STATE_ARMED,
		STATE_PLAYING,
		STATE_PAUSED,
	};

	struct STasTick
	{
		int m_Tick = 0;
		int m_GameTick = 0;
		CNetObj_PlayerInput m_MainInput{};
		CNetObj_PlayerInput m_DummyInput{};
		bool m_HasDummy = false;
		vec2 m_Pos = vec2(0.0f, 0.0f);
		vec2 m_Vel = vec2(0.0f, 0.0f);

		// Physics state snapshot for instant rewind & load
		CCharacterCore m_MainCore{};
		int m_MainFreezeTime = 0;
		CCharacterCore m_DummyCore{};
		int m_DummyFreezeTime = 0;
	};

	struct STasCheckpoint
	{
		bool m_Valid = false;
		int m_Tick = 0;
		int m_GameTick = 0;
		vec2 m_Pos = vec2(0.0f, 0.0f);
		vec2 m_Vel = vec2(0.0f, 0.0f);
		CCharacterCore m_MainCore{};
		int m_MainFreezeTime = 0;
		CCharacterCore m_DummyCore{};
		int m_DummyFreezeTime = 0;
		bool m_HasDummy = false;
	};

	int Sizeof() const override { return sizeof(*this); }

	void OnConsoleInit() override;
	void OnReset() override;
	void OnMapLoad() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnRender() override;
	bool OnInput(const IInput::CEvent &Event) override;

	void StartRecord(bool ResetTrack = true);
	void StopRecord();
	void ToggleRecord();

	void ArmPlayback();
	void StartPlayback();
	void StopPlayback();
	void TogglePlayback();

	void SaveCheckpoint();
	void LoadCheckpoint();
	void Rewind(int NumTicks, bool IsAutoHazard = false);
	void Clear();

	bool SaveToFile(const char *pFilename);
	bool LoadFromFile(const char *pFilename);
	void DeleteTasFile(const char *pFilename);
	void RefreshFileList();

	int OnSnapInput(int *pData, bool Dummy, bool Force);
	void OnRecordInput(const int *pData, bool Dummy);
	void PrepareInputForSend(int *pData, int Size, bool Dummy);

	// Fast Practice TAS integration
	int ConsumeSlowMoTicks();
	bool CheckHazardAndRewind(int LocalClientId, int DummyClientId);
	void RecordPracticeTick(int LocalClientId, int DummyClientId, int GameTick);
	bool IsHazard(const CCharacter *pChar) const;
	void RestorePhysicalState(const CCharacterCore &MainCore, int MainFreezeTime,
	                          const CCharacterCore &DummyCore, int DummyFreezeTime,
	                          int GameTick);

	ETasState State() const { return m_State; }
	bool IsPlaybackActive() const { return m_State == STATE_PLAYING; }
	bool IsRecordingActive() const { return m_State == STATE_RECORDING; }
	bool IsArmed() const { return m_State == STATE_ARMED; }
	int PlaybackTick() const { return m_PlaybackTick; }
	int RecordTick() const { return m_CurrentRecordTick; }
	int TotalTicks() const { return (int)m_vTicks.size(); }
	const std::vector<std::string> &FileList() const { return m_vFileList; }
	const char *CurrentFile() const { return m_aLoadedFileName; }
	bool HasCheckpoint() const { return m_Checkpoint.m_Valid; }
	int CheckpointTick() const { return m_Checkpoint.m_Tick; }
	const std::vector<STasTick> &Ticks() const { return m_vTicks; }

private:
	ETasState m_State = STATE_IDLE;
	std::vector<STasTick> m_vTicks;
	STasCheckpoint m_Checkpoint;
	STasCheckpoint m_InitialState;

	int m_PlaybackTick = 0;
	int m_CurrentRecordTick = 0;
	int m_LastPlaybackGameTick = -1;
	int m_LastRecordGameTick = -1;
	int m_LastRaceTickSeen = -1;

	// Slow-mo and Hazard Auto-Rewind
	int64_t m_LastRecordTickTime = 0;
	int64_t m_RecordTimeAccumulator = 0;
	int m_HazardCooldownTicks = 0;

	char m_aCurrentMap[128] = "";
	char m_aLoadedFileName[64] = "";
	std::vector<std::string> m_vFileList;

	void RenderHud();
	void CheckRaceAutoTrigger();

	static void ConTasRecord(IConsole::IResult *pResult, void *pUserData);
	static void ConTasPlay(IConsole::IResult *pResult, void *pUserData);
	static void ConTasArm(IConsole::IResult *pResult, void *pUserData);
	static void ConTasStop(IConsole::IResult *pResult, void *pUserData);
	static void ConTasSave(IConsole::IResult *pResult, void *pUserData);
	static void ConTasLoad(IConsole::IResult *pResult, void *pUserData);
	static void ConTasSaveCheckpoint(IConsole::IResult *pResult, void *pUserData);
	static void ConTasLoadCheckpoint(IConsole::IResult *pResult, void *pUserData);
	static void ConTasRewind(IConsole::IResult *pResult, void *pUserData);
	static void ConTasClear(IConsole::IResult *pResult, void *pUserData);
	static void ConTasStatus(IConsole::IResult *pResult, void *pUserData);
	static void ConTasToggleRecord(IConsole::IResult *pResult, void *pUserData);
	static void ConTasTogglePlay(IConsole::IResult *pResult, void *pUserData);

	static int TasFileListCallback(const char *pName, int IsDir, int DirType, void *pUser);
};

#endif // GAME_CLIENT_COMPONENTS_BESTCLIENT_TAS_H
