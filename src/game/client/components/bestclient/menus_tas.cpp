/* Copyright © 2026 BestProject Team */
#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <base/math.h>
#include <base/str.h>

#include <game/client/components/bestclient/tas.h>
#include <game/client/components/bestclient/ui_theme/style.h>
#include <game/client/components/bestclient/ui_theme/widgets.h>
#include <game/client/components/menus.h>
#include <game/client/gameclient.h>
#include <game/client/lineinput.h>
#include <game/client/ui.h>
#include <game/client/ui_listbox.h>
#include <game/localization.h>

#include <algorithm>
#include <string>
#include <vector>

void CMenus::RenderSettingsTas(CUIRect MainView)
{
	CTas &Tas = GameClient()->m_Tas;

	CUIRect LeftColumn, RightColumn;
	MainView.VSplitMid(&LeftColumn, &RightColumn, 14.0f);

	// LEFT COLUMN: Status, Controls, Checkpoints, Settings, Automation
	{
		CUIRect StatusBox, ControlBox, CpBox, SettingsBox, AutomationBox;
		LeftColumn.HSplitTop(68.0f, &StatusBox, &LeftColumn);
		LeftColumn.HSplitTop(6.0f, nullptr, &LeftColumn);

		LeftColumn.HSplitTop(106.0f, &ControlBox, &LeftColumn);
		LeftColumn.HSplitTop(6.0f, nullptr, &LeftColumn);

		LeftColumn.HSplitTop(140.0f, &CpBox, &LeftColumn);
		LeftColumn.HSplitTop(6.0f, nullptr, &LeftColumn);

		LeftColumn.HSplitTop(84.0f, &SettingsBox, &LeftColumn);
		LeftColumn.HSplitTop(6.0f, nullptr, &LeftColumn);

		LeftColumn.HSplitTop(78.0f, &AutomationBox, &LeftColumn);

		// Render Status
		StatusBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);
		CUIRect StatusContent, StatusBadge, StatusText;
		StatusBox.Margin(8.0f, &StatusContent);
		StatusContent.HSplitTop(20.0f, &StatusBadge, &StatusText);

		ColorRGBA BadgeCol = ColorRGBA(0.5f, 0.5f, 0.5f, 1.0f);
		const char *pStateStr = BcLocalize("IDLE");
		if(Tas.IsRecordingActive())
		{
			if(Tas.IsWaterCrossingActive())
			{
				BadgeCol = ColorRGBA(0.95f, 0.60f, 0.15f, 1.0f);
				pStateStr = BcLocalize("RECORDING (CROSSING)");
			}
			else
			{
				BadgeCol = ColorRGBA(0.95f, 0.25f, 0.25f, 1.0f);
				pStateStr = BcLocalize("RECORDING");
			}
		}
		else if(Tas.IsPlaybackActive())
		{
			BadgeCol = ColorRGBA(0.25f, 0.95f, 0.40f, 1.0f);
			pStateStr = BcLocalize("PLAYING");
		}

		CUIRect BadgeRect;
		StatusBadge.VSplitLeft(160.0f, &BadgeRect, nullptr);
		BadgeRect.Draw(BadgeCol, IGraphics::CORNER_ALL, 4.0f);
		TextRender()->TextColor(0.05f, 0.05f, 0.05f, 1.0f);
		Ui()->DoLabel(&BadgeRect, pStateStr, 12.0f, TEXTALIGN_MC);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

		char aInfo[128];
		int Cur = Tas.IsPlaybackActive() ? Tas.PlaybackTick() : (Tas.IsRecordingActive() ? Tas.RecordTick() : 0);
		if(Tas.IsRecordingActive())
		{
			if(Tas.IsWaterCrossingActive())
			{
				str_format(aInfo, sizeof(aInfo), BcLocalize("File: %s  |  Ticks: %d (%.2fs)  |  [CROSSING from #%d]"),
					Tas.CurrentFile()[0] ? Tas.CurrentFile() : BcLocalize("<sandbox>"),
					Cur, (float)Cur / 50.0f, Tas.WaterCrossingStartTick());
			}
			else
			{
				str_format(aInfo, sizeof(aInfo), BcLocalize("File: %s  |  Ticks: %d (%.2fs)  |  Speed: %d%%"),
					Tas.CurrentFile()[0] ? Tas.CurrentFile() : BcLocalize("<sandbox>"),
					Cur, (float)Cur / 50.0f, std::clamp(g_Config.m_BcTasRecordSpeed, 10, 100));
			}
		}
		else
		{
			str_format(aInfo, sizeof(aInfo), BcLocalize("File: %s  |  Ticks: %d / %d (%.2fs)"),
				Tas.CurrentFile()[0] ? Tas.CurrentFile() : BcLocalize("<none>"),
				Cur, Tas.TotalTicks(), (float)Tas.TotalTicks() / 50.0f);
		}
		StatusText.HSplitTop(4.0f, nullptr, &StatusText);
		Ui()->DoLabel(&StatusText, aInfo, 11.0f, TEXTALIGN_ML);

		// Render Controls
		ControlBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);
		CUIRect CtrlContent;
		ControlBox.Margin(8.0f, &CtrlContent);

		CUIRect PlayRow, RecRow, CrossRow;
		CtrlContent.HSplitTop(25.0f, &PlayRow, &CtrlContent);
		CtrlContent.HSplitTop(5.0f, nullptr, &CtrlContent);
		CtrlContent.HSplitTop(25.0f, &RecRow, &CtrlContent);
		CtrlContent.HSplitTop(5.0f, nullptr, &CtrlContent);
		CtrlContent.HSplitTop(25.0f, &CrossRow, &CtrlContent);

		// Play Row: Play, Stop, Clear Track (Arm button removed!)
		CUIRect BtnPlay, BtnStop, BtnClear;
		PlayRow.VSplitMid(&BtnPlay, &BtnClear, 4.0f);
		BtnPlay.VSplitMid(&BtnPlay, &BtnStop, 4.0f);

		static CButtonContainer s_PlayBtn;
		if(DoButton_Menu(&s_PlayBtn, BcLocalize("Play Now"), Tas.IsPlaybackActive() ? 1 : 0, &BtnPlay))
		{
			if(Tas.IsPlaybackActive())
				Tas.StopPlayback();
			else
				Tas.StartPlayback();
		}

		static CButtonContainer s_StopBtn;
		if(DoButton_Menu(&s_StopBtn, BcLocalize("Stop"), 0, &BtnStop))
		{
			Tas.StopPlayback();
			Tas.StopRecord();
		}

		static CButtonContainer s_ClearBtn;
		if(DoButton_Menu(&s_ClearBtn, BcLocalize("Clear Track"), 0, &BtnClear))
		{
			Tas.Clear();
		}

		// Rec Row: Record, Stop Rec, Rewind
		CUIRect BtnRec, BtnStopRec, BtnRewind;
		RecRow.VSplitMid(&BtnRec, &BtnRewind, 4.0f);
		BtnRec.VSplitMid(&BtnRec, &BtnStopRec, 4.0f);

		static CButtonContainer s_RecBtn;
		if(DoButton_Menu(&s_RecBtn, BcLocalize("Record"), Tas.IsRecordingActive() ? 1 : 0, &BtnRec))
		{
			Tas.StartRecord(true);
		}

		static CButtonContainer s_StopRecBtn;
		if(DoButton_Menu(&s_StopRecBtn, BcLocalize("Stop Rec"), 0, &BtnStopRec))
		{
			Tas.StopRecord();
		}

		static CButtonContainer s_RewindBtn;
		char aRewindText[64];
		str_format(aRewindText, sizeof(aRewindText), "%s (%d)", BcLocalize("Rewind"), g_Config.m_BcTasRewindTicks);
		if(DoButton_Menu(&s_RewindBtn, aRewindText, 0, &BtnRewind))
		{
			Tas.Rewind(g_Config.m_BcTasRewindTicks, false);
		}

		// Water Crossing Row (Temporary Hazard Auto-Rewind Bypass)
		static CButtonContainer s_CrossWaterBtn;
		char aCrossText[64];
		if(Tas.IsWaterCrossingActive())
			str_copy(aCrossText, BcLocalize("Crossing Water... (Click to Finish)"));
		else
			str_copy(aCrossText, BcLocalize("Cross Water (Ignore Hazard)"));

		if(DoButton_Menu(&s_CrossWaterBtn, aCrossText, Tas.IsWaterCrossingActive() ? 1 : 0, &CrossRow))
		{
			Tas.ToggleWaterCrossing();
		}

		// Checkpoints Box (New dedicated box!)
		CpBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);
		CUIRect CpContent;
		CpBox.Margin(8.0f, &CpContent);

		CUIRect CpTitle, CpListRect, CpBtnRow;
		CpContent.HSplitTop(18.0f, &CpTitle, &CpContent);
		CpContent.HSplitTop(4.0f, nullptr, &CpContent);
		CpContent.HSplitBottom(25.0f, &CpListRect, &CpBtnRow);
		CpListRect.HSplitBottom(5.0f, &CpListRect, nullptr);

		char aCpHeader[64];
		str_format(aCpHeader, sizeof(aCpHeader), "%s (%d)", BcLocalize("Checkpoints"), Tas.CheckpointCount());
		Ui()->DoLabel(&CpTitle, aCpHeader, 12.0f, TEXTALIGN_ML);

		const auto &vCheckpoints = Tas.Checkpoints();
		static int s_SelectedCpIndex = -1;
		static CListBox s_CpListBox;

		if(s_SelectedCpIndex >= (int)vCheckpoints.size())
			s_SelectedCpIndex = (int)vCheckpoints.size() - 1;

		s_CpListBox.DoStart(18.0f, (int)vCheckpoints.size(), 1, 3, s_SelectedCpIndex, &CpListRect, true, IGraphics::CORNER_ALL, false);
		for(size_t i = 0; i < vCheckpoints.size(); ++i)
		{
			const auto &Cp = vCheckpoints[i];
			const CListboxItem Item = s_CpListBox.DoNextItem(&vCheckpoints[i], (int)i == s_SelectedCpIndex);
			if(!Item.m_Visible)
				continue;

			char aItemText[96];
			str_format(aItemText, sizeof(aItemText), "#%d | %s %d (%.2fs) | (%.2f, %.2f)",
				(int)i + 1, BcLocalize("Tick"), Cp.m_Tick, (float)Cp.m_Tick / 50.0f, Cp.m_Pos.x / 32.0f, Cp.m_Pos.y / 32.0f);
			Ui()->DoLabel(&Item.m_Rect, aItemText, 11.0f, TEXTALIGN_ML);
		}
		const int NewCpSelected = s_CpListBox.DoEnd();
		if(s_CpListBox.WasItemSelected() && NewCpSelected >= 0 && NewCpSelected < (int)vCheckpoints.size())
		{
			s_SelectedCpIndex = NewCpSelected;
		}
		if(s_CpListBox.WasItemActivated())
		{
			if(s_SelectedCpIndex >= 0 && s_SelectedCpIndex < (int)vCheckpoints.size())
				Tas.LoadCheckpoint(s_SelectedCpIndex);
		}

		if(vCheckpoints.empty())
		{
			CUIRect EmptyLabelRect = CpListRect;
			EmptyLabelRect.Margin(6.0f, &EmptyLabelRect);
			TextRender()->TextColor(0.6f, 0.6f, 0.6f, 0.8f);
			Ui()->DoLabel(&EmptyLabelRect, BcLocalize("No checkpoints saved. Click 'Save CP' while recording."), 10.0f, TEXTALIGN_MC);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		}

		// Checkpoint action buttons: Save CP, Load CP, Delete
		CUIRect BtnSaveCp, BtnLoadCp, BtnDeleteCp;
		CpBtnRow.VSplitMid(&BtnSaveCp, &BtnDeleteCp, 4.0f);
		BtnSaveCp.VSplitMid(&BtnSaveCp, &BtnLoadCp, 4.0f);

		static CButtonContainer s_SaveCpBtn;
		if(DoButton_Menu(&s_SaveCpBtn, BcLocalize("Save CP"), 0, &BtnSaveCp))
		{
			Tas.SaveCheckpoint();
			s_SelectedCpIndex = Tas.CheckpointCount() - 1;
		}

		static CButtonContainer s_LoadCpBtn;
		if(DoButton_Menu(&s_LoadCpBtn, BcLocalize("Load CP"), 0, &BtnLoadCp))
		{
			if(s_SelectedCpIndex >= 0 && s_SelectedCpIndex < (int)vCheckpoints.size())
				Tas.LoadCheckpoint(s_SelectedCpIndex);
			else
				Tas.LoadCheckpoint();
		}

		static CButtonContainer s_DeleteCpBtn;
		if(DoButton_Menu(&s_DeleteCpBtn, BcLocalize("Delete"), 0, &BtnDeleteCp))
		{
			if(s_SelectedCpIndex >= 0 && s_SelectedCpIndex < (int)vCheckpoints.size())
			{
				Tas.DeleteCheckpoint(s_SelectedCpIndex);
				s_SelectedCpIndex = -1;
			}
		}

		// Settings Box (Record Speed & Auto-Rewind)
		SettingsBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);
		CUIRect SettingsContent;
		SettingsBox.Margin(8.0f, &SettingsContent);

		CUIRect RowSpeed, RowAutoRewind, RowRewindTicks;
		SettingsContent.HSplitTop(20.0f, &RowSpeed, &SettingsContent);
		SettingsContent.HSplitTop(4.0f, nullptr, &SettingsContent);
		SettingsContent.HSplitTop(20.0f, &RowAutoRewind, &SettingsContent);
		SettingsContent.HSplitTop(4.0f, nullptr, &SettingsContent);
		SettingsContent.HSplitTop(20.0f, &RowRewindTicks, &SettingsContent);

		Ui()->DoScrollbarOption(&g_Config.m_BcTasRecordSpeed, &g_Config.m_BcTasRecordSpeed, &RowSpeed, BcLocalize("Record speed"), 10, 100, &CUi::ms_LinearScrollbarScale, 0u, "%");

		static CButtonContainer s_CbAutoRewind;
		if(DoButton_CheckBox(&s_CbAutoRewind, BcLocalize("Auto-rewind on hitting hazard (death/freeze)"), g_Config.m_BcTasAutoRewind, &RowAutoRewind))
			g_Config.m_BcTasAutoRewind ^= 1;

		Ui()->DoScrollbarOption(&g_Config.m_BcTasRewindTicks, &g_Config.m_BcTasRewindTicks, &RowRewindTicks, BcLocalize("Hazard rewind ticks"), 5, 100, &CUi::ms_LinearScrollbarScale, 0u);

		// Automation Box (Auto-start crossing race line removed!)
		AutomationBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);
		CUIRect AutoContent;
		AutomationBox.Margin(8.0f, &AutoContent);

		CUIRect Row1, Row2, Row3;
		AutoContent.HSplitTop(20.0f, &Row1, &AutoContent);
		AutoContent.HSplitTop(4.0f, nullptr, &AutoContent);
		AutoContent.HSplitTop(20.0f, &Row2, &AutoContent);
		AutoContent.HSplitTop(4.0f, nullptr, &AutoContent);
		AutoContent.HSplitTop(20.0f, &Row3, &AutoContent);

		static CButtonContainer s_CbDummy;
		if(DoButton_CheckBox(&s_CbDummy, BcLocalize("Play Dummy input if available in TAS file"), g_Config.m_BcTasPlaybackDummy, &Row1))
			g_Config.m_BcTasPlaybackDummy ^= 1;

		static CButtonContainer s_CbStopOnInput;
		if(DoButton_CheckBox(&s_CbStopOnInput, BcLocalize("Abort playback upon manual mouse/keyboard action"), g_Config.m_BcTasAutoStopOnInput, &Row2))
			g_Config.m_BcTasAutoStopOnInput ^= 1;

		static CButtonContainer s_CbShowHud;
		if(DoButton_CheckBox(&s_CbShowHud, BcLocalize("Show in-game TAS status and progress HUD"), g_Config.m_BcTasShowHud, &Row3))
			g_Config.m_BcTasShowHud ^= 1;
	}

	// RIGHT COLUMN: File Management & Track Details (Keybinds removed!)
	{
		CUIRect SaveRow, ListRect, ListBtnRow, TrackInfoBox;
		RightColumn.HSplitTop(26.0f, &SaveRow, &RightColumn);
		RightColumn.HSplitTop(6.0f, nullptr, &RightColumn);

		RightColumn.HSplitTop(210.0f, &ListRect, &RightColumn);
		RightColumn.HSplitTop(6.0f, nullptr, &RightColumn);

		RightColumn.HSplitTop(26.0f, &ListBtnRow, &RightColumn);
		RightColumn.HSplitTop(8.0f, nullptr, &RightColumn);

		RightColumn.HSplitTop(110.0f, &TrackInfoBox, &RightColumn);

		const auto &vFiles = Tas.FileList();
		static int s_SelectedFileIndex = -1;
		static CListBox s_FileListBox;
		static CLineInputBuffered<128> s_SaveFileNameInput;

		if(s_SelectedFileIndex >= (int)vFiles.size())
		{
			s_SelectedFileIndex = -1;
		}

		// Save row: Editbox + Button ("Save Run" when none selected, "Overwrite Name" when selected)
		CUIRect EditBoxRect, SaveBtnRect;
		SaveRow.VSplitRight(100.0f, &EditBoxRect, &SaveBtnRect);
		EditBoxRect.VSplitRight(6.0f, &EditBoxRect, nullptr);

		Ui()->DoEditBox(&s_SaveFileNameInput, &EditBoxRect, 12.0f);

		const bool HasSelectedTrack = (s_SelectedFileIndex >= 0 && s_SelectedFileIndex < (int)vFiles.size());
		const char *pBtnLabel = HasSelectedTrack ? BcLocalize("Overwrite Name") : BcLocalize("Save Run");

		static CButtonContainer s_SaveFileBtn;
		if(DoButton_Menu(&s_SaveFileBtn, pBtnLabel, 0, &SaveBtnRect))
		{
			const char *pName = s_SaveFileNameInput.GetString();
			if(pName && pName[0])
			{
				if(HasSelectedTrack)
				{
					const std::string OldName = vFiles[s_SelectedFileIndex];
					if(str_comp(OldName.c_str(), pName) == 0)
					{
						Tas.SaveToFile(OldName.c_str());
					}
					else
					{
						if(Tas.RenameTasFile(OldName.c_str(), pName))
						{
							if(!Tas.Ticks().empty() && str_comp(Tas.CurrentFile(), pName) == 0)
								Tas.SaveToFile(pName);
						}
					}
				}
				else
				{
					if(Tas.SaveToFile(pName))
					{
						s_SaveFileNameInput.Clear();
						s_SelectedFileIndex = -1;
					}
				}
			}
		}

		// File Listbox
		ListRect.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);
		CUIRect InnerList;
		ListRect.Margin(4.0f, &InnerList);

		s_FileListBox.DoStart(20.0f, (int)vFiles.size(), 1, 3, s_SelectedFileIndex, &InnerList, false);
		for(size_t i = 0; i < vFiles.size(); ++i)
		{
			const CListboxItem Item = s_FileListBox.DoNextItem(&vFiles[i], (int)i == s_SelectedFileIndex);
			if(!Item.m_Visible)
				continue;

			const bool IsCurrentLoaded = str_comp(Tas.CurrentFile(), vFiles[i].c_str()) == 0;
			if(IsCurrentLoaded)
				TextRender()->TextColor(0.3f, 1.0f, 0.4f, 1.0f);

			Ui()->DoLabel(&Item.m_Rect, vFiles[i].c_str(), 12.0f, TEXTALIGN_ML);

			if(IsCurrentLoaded)
				TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		}
		const int NewSelected = s_FileListBox.DoEnd();
		if(s_FileListBox.WasItemSelected())
		{
			if(NewSelected == s_SelectedFileIndex)
			{
				// Clicking the currently selected item again toggles off selection
				s_SelectedFileIndex = -1;
				s_SaveFileNameInput.Clear();
			}
			else if(NewSelected >= 0 && NewSelected < (int)vFiles.size())
			{
				s_SelectedFileIndex = NewSelected;
				s_SaveFileNameInput.Set(vFiles[s_SelectedFileIndex].c_str());
			}
		}
		if(s_FileListBox.WasItemActivated())
		{
			if(s_SelectedFileIndex >= 0 && s_SelectedFileIndex < (int)vFiles.size())
				Tas.LoadFromFile(vFiles[s_SelectedFileIndex].c_str());
		}

		if(vFiles.empty())
		{
			CUIRect EmptyLabelRect = InnerList;
			EmptyLabelRect.Margin(10.0f, &EmptyLabelRect);
			TextRender()->TextColor(0.6f, 0.6f, 0.6f, 0.8f);
			Ui()->DoLabel(&EmptyLabelRect, BcLocalize("No .tas runs found. Record and save a run!"), 11.0f, TEXTALIGN_MC);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		}

		// List Action Buttons: Load, Refresh, Delete
		CUIRect BtnLoad, BtnRefresh, BtnDelete;
		ListBtnRow.VSplitMid(&BtnLoad, &BtnDelete, 6.0f);
		BtnLoad.VSplitMid(&BtnLoad, &BtnRefresh, 6.0f);

		static CButtonContainer s_LoadBtn;
		if(DoButton_Menu(&s_LoadBtn, BcLocalize("Load Selected"), 0, &BtnLoad))
		{
			if(s_SelectedFileIndex >= 0 && s_SelectedFileIndex < (int)vFiles.size())
				Tas.LoadFromFile(vFiles[s_SelectedFileIndex].c_str());
			else if(!s_SaveFileNameInput.IsEmpty())
				Tas.LoadFromFile(s_SaveFileNameInput.GetString());
		}

		static CButtonContainer s_RefreshBtn;
		if(DoButton_Menu(&s_RefreshBtn, BcLocalize("Refresh"), 0, &BtnRefresh))
		{
			Tas.RefreshFileList();
		}

		static CButtonContainer s_DeleteBtn;
		if(DoButton_Menu(&s_DeleteBtn, BcLocalize("Delete"), 0, &BtnDelete))
		{
			if(s_SelectedFileIndex >= 0 && s_SelectedFileIndex < (int)vFiles.size())
			{
				Tas.DeleteTasFile(vFiles[s_SelectedFileIndex].c_str());
				s_SelectedFileIndex = -1;
				s_SaveFileNameInput.Clear();
			}
		}

		// Track Info Box (Displays map, start coords, and total ticks for selected track)
		TrackInfoBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);
		CUIRect InfoContent;
		TrackInfoBox.Margin(8.0f, &InfoContent);

		static CTas::STasFileInfo s_CachedFileInfo{};
		static std::string s_CachedFileName;

		if(s_SelectedFileIndex >= 0 && s_SelectedFileIndex < (int)vFiles.size())
		{
			if(s_CachedFileName != vFiles[s_SelectedFileIndex])
			{
				s_CachedFileName = vFiles[s_SelectedFileIndex];
				Tas.GetTasFileInfo(s_CachedFileName.c_str(), &s_CachedFileInfo);
			}
			const CTas::STasFileInfo &Info = s_CachedFileInfo;

			CUIRect RowTitle, RowMap, RowPos, RowTicks;
			InfoContent.HSplitTop(18.0f, &RowTitle, &InfoContent);
			InfoContent.HSplitTop(2.0f, nullptr, &InfoContent);
			InfoContent.HSplitTop(16.0f, &RowMap, &InfoContent);
			InfoContent.HSplitTop(2.0f, nullptr, &InfoContent);
			InfoContent.HSplitTop(16.0f, &RowPos, &InfoContent);
			InfoContent.HSplitTop(2.0f, nullptr, &InfoContent);
			InfoContent.HSplitTop(16.0f, &RowTicks, &InfoContent);

			char aTitle[128];
			str_format(aTitle, sizeof(aTitle), "%s: %s", BcLocalize("Track Details"), vFiles[s_SelectedFileIndex].c_str());
			TextRender()->TextColor(0.4f, 0.9f, 1.0f, 1.0f);
			Ui()->DoLabel(&RowTitle, aTitle, 12.0f, TEXTALIGN_ML);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

			char aMap[128];
			str_format(aMap, sizeof(aMap), "%s: %s", BcLocalize("Map"), Info.m_aMap[0] ? Info.m_aMap : BcLocalize("unknown"));
			Ui()->DoLabel(&RowMap, aMap, 11.0f, TEXTALIGN_ML);

			char aPos[128];
			str_format(aPos, sizeof(aPos), "%s: (%.2f, %.2f)", BcLocalize("Start Pos"), Info.m_StartPos.x / 32.0f, Info.m_StartPos.y / 32.0f);
			Ui()->DoLabel(&RowPos, aPos, 11.0f, TEXTALIGN_ML);

			char aTicks[128];
			str_format(aTicks, sizeof(aTicks), "%s: %d %s (%.2f%s)", BcLocalize("Length"), Info.m_TotalTicks, BcLocalize("ticks"), (float)Info.m_TotalTicks / 50.0f, BcLocalize("s"));
			Ui()->DoLabel(&RowTicks, aTicks, 11.0f, TEXTALIGN_ML);
		}
		else
		{
			TextRender()->TextColor(0.6f, 0.6f, 0.6f, 0.8f);
			Ui()->DoLabel(&InfoContent, BcLocalize("No track selected (click a track in the list to view details)"), 11.0f, TEXTALIGN_MC);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		}
	}
}

void CMenus::RenderSettingsTasHelpers(CUIRect MainView)
{
	CUIRect LeftColumn, RightColumn;
	MainView.VSplitMid(&LeftColumn, &RightColumn, 14.0f);

	// Left: Fast Practice & Simulation Helpers
	{
		CUIRect Box1;
		LeftColumn.HSplitTop(180.0f, &Box1, &LeftColumn);
		Box1.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);

		CUIRect Content;
		Box1.Margin(8.0f, &Content);

		CUIRect Title, RowBtn, Desc;
		Content.HSplitTop(22.0f, &Title, &Content);
		Ui()->DoLabel(&Title, BcLocalize("Fast Practice (Local Sandbox World)"), 13.0f, TEXTALIGN_ML);

		Content.HSplitTop(6.0f, nullptr, &Content);
		Content.HSplitTop(26.0f, &RowBtn, &Content);

		static CButtonContainer s_FpBtn;
		const bool FpEnabled = GameClient()->m_FastPractice.Enabled();
		if(DoButton_Menu(&s_FpBtn, FpEnabled ? BcLocalize("Stop Fast Practice") : BcLocalize("Start Fast Practice"), FpEnabled ? 1 : 0, &RowBtn))
		{
			GameClient()->m_FastPractice.Toggle();
		}

		Content.HSplitTop(8.0f, nullptr, &Content);
		Content.HSplitTop(50.0f, &Desc, &Content);
		TextRender()->TextColor(0.7f, 0.7f, 0.7f, 1.0f);
		Ui()->DoLabel(&Desc, BcLocalize("Fast Practice runs a completely local simulation world on your client, allowing you to practice movements and record TAS runs offline without network lag or interference."), 11.0f, TEXTALIGN_ML);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}

	// Right: HUD module editor shortcut, so the auxiliary tab leads somewhere useful.
	{
		CUIRect Box2;
		RightColumn.HSplitTop(180.0f, &Box2, &RightColumn);
		Box2.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);

		CUIRect Content;
		Box2.Margin(8.0f, &Content);

		CUIRect Title, RowBtn, Desc;
		Content.HSplitTop(22.0f, &Title, &Content);
		Ui()->DoLabel(&Title, BcLocalize("Practice HUD modules"), 13.0f, TEXTALIGN_ML);

		Content.HSplitTop(6.0f, nullptr, &Content);
		Content.HSplitTop(26.0f, &RowBtn, &Content);

		static CButtonContainer s_HudEditorBtn;
		if(DoButton_Menu(&s_HudEditorBtn, BcLocalize("Open HUD editor"), 0, &RowBtn))
		{
			if(GameClient()->m_HudEditor.IsActive())
				GameClient()->m_HudEditor.Deactivate();
			else
				GameClient()->m_HudEditor.Activate();
		}

		Content.HSplitTop(8.0f, nullptr, &Content);
		Content.HSplitTop(70.0f, &Desc, &Content);
		TextRender()->TextColor(0.75f, 0.85f, 0.95f, 1.0f);
		Ui()->DoLabel(&Desc, BcLocalize("The TAS status overlay, the Avoid status panel and the TAS trajectory are regular HUD modules: enable, move and scale them in the HUD editor."), 11.0f, TEXTALIGN_ML);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
}

void CMenus::RenderSettingsTasAnd(CUIRect MainView)
{
	const float TopExtend = BestClientUiTheme::IsNewTabOption() ? 8.0f : 20.0f;
	MainView.y -= TopExtend;
	MainView.h += TopExtend;

	MainView.HSplitTop(BestClientUiTheme::IsNewTabOption() ? 6.0f : 8.0f, nullptr, &MainView);

	// Subtabs: TAS, Avoid, Auxiliary
	const char *apTabNames[] = {
		BcLocalize("TAS"),
		BcLocalize("Avoid"),
		BcLocalize("Auxiliary"),
	};
	constexpr int NumTabs = 3;

	static CButtonContainer s_aSubTabButtons[NumTabs];

	if(!BestClientUiTheme::IsNewTabOption())
	{
		CUIRect TabBar, TabButton;
		MainView.HSplitTop(24.0f, &TabBar, &MainView);
		const float TabWidth = TabBar.w / (float)NumTabs;
		for(int i = 0; i < NumTabs; ++i)
		{
			TabBar.VSplitLeft(TabWidth, &TabButton, &TabBar);
			const int Corners = i == 0 ? IGraphics::CORNER_L : (i == NumTabs - 1 ? IGraphics::CORNER_R : IGraphics::CORNER_NONE);
			if(DoButton_MenuTab(&s_aSubTabButtons[i], apTabNames[i], g_Config.m_BcTasTab == i, &TabButton, Corners))
				g_Config.m_BcTasTab = i;
		}
		MainView.HSplitTop(10.0f, nullptr, &MainView);
	}

	if(g_Config.m_BcTasTab == 1)
		RenderSettingsAvoid(MainView);
	else if(g_Config.m_BcTasTab == 2)
		RenderSettingsTasHelpers(MainView);
	else
		RenderSettingsTas(MainView);
}
