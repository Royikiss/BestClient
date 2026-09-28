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

	// LEFT COLUMN: Controls, Automation, Status
	{
		// 1. Status Block
		CUIRect StatusBox, ControlBox, AutomationBox;
		LeftColumn.HSplitTop(70.0f, &StatusBox, &LeftColumn);
		LeftColumn.HSplitTop(10.0f, nullptr, &LeftColumn);

		LeftColumn.HSplitTop(120.0f, &ControlBox, &LeftColumn);
		LeftColumn.HSplitTop(10.0f, nullptr, &LeftColumn);

		LeftColumn.HSplitTop(130.0f, &AutomationBox, &LeftColumn);

		// Render Status
		StatusBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);
		CUIRect StatusContent, StatusBadge, StatusText;
		StatusBox.Margin(8.0f, &StatusContent);
		StatusContent.HSplitTop(20.0f, &StatusBadge, &StatusText);

		ColorRGBA BadgeCol = ColorRGBA(0.5f, 0.5f, 0.5f, 1.0f);
		const char *pStateStr = BcLocalize("IDLE");
		if(Tas.IsRecordingActive())
		{
			BadgeCol = ColorRGBA(0.95f, 0.25f, 0.25f, 1.0f);
			pStateStr = BcLocalize("RECORDING");
		}
		else if(Tas.IsPlaybackActive())
		{
			BadgeCol = ColorRGBA(0.25f, 0.95f, 0.40f, 1.0f);
			pStateStr = BcLocalize("PLAYING");
		}
		else if(Tas.IsArmed())
		{
			BadgeCol = ColorRGBA(0.95f, 0.85f, 0.25f, 1.0f);
			pStateStr = BcLocalize("ARMED (WAITING START LINE)");
		}

		CUIRect BadgeRect;
		StatusBadge.VSplitLeft(160.0f, &BadgeRect, nullptr);
		BadgeRect.Draw(BadgeCol, IGraphics::CORNER_ALL, 4.0f);
		TextRender()->TextColor(0.05f, 0.05f, 0.05f, 1.0f);
		Ui()->DoLabel(&BadgeRect, pStateStr, 12.0f, TEXTALIGN_MC);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

		char aInfo[128];
		int Cur = Tas.IsPlaybackActive() ? Tas.PlaybackTick() : (Tas.IsRecordingActive() ? Tas.RecordTick() : 0);
		str_format(aInfo, sizeof(aInfo), BcLocalize("File: %s  |  Ticks: %d / %d (%.2fs)"),
			Tas.CurrentFile()[0] ? Tas.CurrentFile() : BcLocalize("<none>"),
			Cur, Tas.TotalTicks(), (float)Tas.TotalTicks() / 50.0f);
		StatusText.HSplitTop(4.0f, nullptr, &StatusText);
		Ui()->DoLabel(&StatusText, aInfo, 11.0f, TEXTALIGN_ML);

		// Render Controls
		ControlBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);
		CUIRect CtrlContent;
		ControlBox.Margin(8.0f, &CtrlContent);

		CUIRect PlayRow, RecRow, CpRow;
		CtrlContent.HSplitTop(26.0f, &PlayRow, &CtrlContent);
		CtrlContent.HSplitTop(6.0f, nullptr, &CtrlContent);
		CtrlContent.HSplitTop(26.0f, &RecRow, &CtrlContent);
		CtrlContent.HSplitTop(6.0f, nullptr, &CtrlContent);
		CtrlContent.HSplitTop(26.0f, &CpRow, &CtrlContent);

		// Play Row
		CUIRect BtnArm, BtnPlay, BtnStop;
		PlayRow.VSplitMid(&BtnArm, &BtnStop, 4.0f);
		BtnArm.VSplitMid(&BtnArm, &BtnPlay, 4.0f);

		static CButtonContainer s_ArmBtn;
		if(DoButton_Menu(&s_ArmBtn, BcLocalize("Arm (Race)"), Tas.IsArmed() ? 1 : 0, &BtnArm))
		{
			if(Tas.IsArmed())
				Tas.StopPlayback();
			else
				Tas.ArmPlayback();
		}

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

		// Rec Row
		CUIRect BtnRec, BtnStopRec, BtnClear;
		RecRow.VSplitMid(&BtnRec, &BtnClear, 4.0f);
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

		static CButtonContainer s_ClearBtn;
		if(DoButton_Menu(&s_ClearBtn, BcLocalize("Clear Track"), 0, &BtnClear))
		{
			Tas.Clear();
		}

		// Checkpoint Row
		CUIRect BtnSaveCp, BtnLoadCp;
		CpRow.VSplitMid(&BtnSaveCp, &BtnLoadCp, 4.0f);

		static CButtonContainer s_SaveCpBtn;
		if(DoButton_Menu(&s_SaveCpBtn, BcLocalize("Save Checkpoint"), 0, &BtnSaveCp))
		{
			Tas.SaveCheckpoint();
		}

		static CButtonContainer s_LoadCpBtn;
		char aLoadCpText[64];
		if(Tas.HasCheckpoint())
			str_format(aLoadCpText, sizeof(aLoadCpText), "%s (%d)", BcLocalize("Load Checkpoint"), Tas.CheckpointTick());
		else
			str_copy(aLoadCpText, BcLocalize("Load Checkpoint"));

		if(DoButton_Menu(&s_LoadCpBtn, aLoadCpText, 0, &BtnLoadCp))
		{
			Tas.LoadCheckpoint();
		}

		// Automation Box
		AutomationBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);
		CUIRect AutoContent;
		AutomationBox.Margin(8.0f, &AutoContent);

		CUIRect Row1, Row2, Row3, Row4;
		AutoContent.HSplitTop(22.0f, &Row1, &AutoContent);
		AutoContent.HSplitTop(4.0f, nullptr, &AutoContent);
		AutoContent.HSplitTop(22.0f, &Row2, &AutoContent);
		AutoContent.HSplitTop(4.0f, nullptr, &AutoContent);
		AutoContent.HSplitTop(22.0f, &Row3, &AutoContent);
		AutoContent.HSplitTop(4.0f, nullptr, &AutoContent);
		AutoContent.HSplitTop(22.0f, &Row4, &AutoContent);

		static CButtonContainer s_CbAutoStart;
		if(DoButton_CheckBox(&s_CbAutoStart, BcLocalize("Auto-start playback on crossing race start line"), g_Config.m_BcTasAutoStart, &Row1))
			g_Config.m_BcTasAutoStart ^= 1;

		static CButtonContainer s_CbDummy;
		if(DoButton_CheckBox(&s_CbDummy, BcLocalize("Play Dummy input if available in TAS file"), g_Config.m_BcTasPlaybackDummy, &Row2))
			g_Config.m_BcTasPlaybackDummy ^= 1;

		static CButtonContainer s_CbStopOnInput;
		if(DoButton_CheckBox(&s_CbStopOnInput, BcLocalize("Abort playback upon manual mouse/keyboard action"), g_Config.m_BcTasAutoStopOnInput, &Row3))
			g_Config.m_BcTasAutoStopOnInput ^= 1;

		static CButtonContainer s_CbShowHud;
		if(DoButton_CheckBox(&s_CbShowHud, BcLocalize("Show in-game TAS status and progress HUD"), g_Config.m_BcTasShowHud, &Row4))
			g_Config.m_BcTasShowHud ^= 1;
	}

	// RIGHT COLUMN: File Management & Quick Reference
	{
		CUIRect SaveRow, ListRect, ListBtnRow, KeybindBox;
		RightColumn.HSplitTop(26.0f, &SaveRow, &RightColumn);
		RightColumn.HSplitTop(6.0f, nullptr, &RightColumn);

		RightColumn.HSplitTop(200.0f, &ListRect, &RightColumn);
		RightColumn.HSplitTop(6.0f, nullptr, &RightColumn);

		RightColumn.HSplitTop(26.0f, &ListBtnRow, &RightColumn);
		RightColumn.HSplitTop(10.0f, nullptr, &RightColumn);

		RightColumn.HSplitTop(110.0f, &KeybindBox, &RightColumn);

		// Save row: Editbox + Save button
		static CLineInputBuffered<128> s_SaveFileNameInput;
		if(s_SaveFileNameInput.IsEmpty() && g_Config.m_BcTasCurrentFile[0])
			s_SaveFileNameInput.Set(g_Config.m_BcTasCurrentFile);

		CUIRect EditBoxRect, SaveBtnRect;
		SaveRow.VSplitRight(90.0f, &EditBoxRect, &SaveBtnRect);
		EditBoxRect.VSplitRight(6.0f, &EditBoxRect, nullptr);

		Ui()->DoEditBox(&s_SaveFileNameInput, &EditBoxRect, 12.0f);

		static CButtonContainer s_SaveFileBtn;
		if(DoButton_Menu(&s_SaveFileBtn, BcLocalize("Save Run"), 0, &SaveBtnRect))
		{
			const char *pName = s_SaveFileNameInput.GetString();
			if(pName && pName[0])
			{
				Tas.SaveToFile(pName);
				str_copy(g_Config.m_BcTasCurrentFile, pName);
			}
		}

		// File Listbox
		const auto &vFiles = Tas.FileList();
		static int s_SelectedFileIndex = -1;
		static CListBox s_FileListBox;

		if(s_SelectedFileIndex >= (int)vFiles.size())
			s_SelectedFileIndex = -1;

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
		if(NewSelected >= 0 && NewSelected < (int)vFiles.size())
		{
			s_SelectedFileIndex = NewSelected;
			s_SaveFileNameInput.Set(vFiles[s_SelectedFileIndex].c_str());
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
			}
		}

		// Keybind Reference Box
		KeybindBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.20f), IGraphics::CORNER_ALL, 6.0f);
		CUIRect KeybindContent;
		KeybindBox.Margin(6.0f, &KeybindContent);

		Ui()->DoLabel(&KeybindContent, BcLocalize("Quick Keybind Recommendations (type in F1 console):"), 11.0f, TEXTALIGN_TL);
		KeybindContent.HSplitTop(18.0f, nullptr, &KeybindContent);

		const char *apBinds[] = {
			"bind p tas_play_toggle",
			"bind o tas_arm",
			"bind i tas_record_toggle",
			"bind f5 tas_save_cp; bind f6 tas_load_cp",
		};
		for(const char *pBind : apBinds)
		{
			CUIRect Line;
			KeybindContent.HSplitTop(16.0f, &Line, &KeybindContent);
			TextRender()->TextColor(0.8f, 0.8f, 0.5f, 1.0f);
			Ui()->DoLabel(&Line, pBind, 11.0f, TEXTALIGN_ML);
		}
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
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

	// Right: Upcoming Auxiliary Modules
	{
		CUIRect Box2;
		RightColumn.HSplitTop(180.0f, &Box2, &RightColumn);
		Box2.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);

		CUIRect Content;
		Box2.Margin(8.0f, &Content);

		CUIRect Title, Desc;
		Content.HSplitTop(22.0f, &Title, &Content);
		Ui()->DoLabel(&Title, BcLocalize("Auxiliary Modules"), 13.0f, TEXTALIGN_ML);

		Content.HSplitTop(10.0f, nullptr, &Content);
		Content.HSplitTop(60.0f, &Desc, &Content);
		TextRender()->TextColor(0.75f, 0.85f, 0.95f, 1.0f);
		Ui()->DoLabel(&Desc, BcLocalize("This section is reserved for upcoming auxiliary modules and practice assistants.\nMore modules will be integrated here."), 12.0f, TEXTALIGN_ML);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
}

void CMenus::RenderSettingsTasAnd(CUIRect MainView)
{
	const float TopExtend = BestClientUiTheme::IsNewTabOption() ? 8.0f : 20.0f;
	MainView.y -= TopExtend;
	MainView.h += TopExtend;

	MainView.HSplitTop(BestClientUiTheme::IsNewTabOption() ? 6.0f : 8.0f, nullptr, &MainView);

	// Subtabs: TAS, Auxiliary
	const char *apTabNames[] = {
		BcLocalize("TAS"),
		BcLocalize("Auxiliary"),
	};
	constexpr int NumTabs = 2;

	static CButtonContainer s_aSubTabButtons[NumTabs];

	if(!BestClientUiTheme::IsNewTabOption())
	{
		CUIRect TabBar, TabButton;
		MainView.HSplitTop(24.0f, &TabBar, &MainView);
		const float TabWidth = TabBar.w / (float)NumTabs;
		for(int i = 0; i < NumTabs; ++i)
		{
			TabBar.VSplitLeft(TabWidth, &TabButton, &TabBar);
			const int Corners = i == 0 ? IGraphics::CORNER_L : IGraphics::CORNER_R;
			if(DoButton_MenuTab(&s_aSubTabButtons[i], apTabNames[i], g_Config.m_BcTasTab == i, &TabButton, Corners))
				g_Config.m_BcTasTab = i;
		}
		MainView.HSplitTop(10.0f, nullptr, &MainView);
	}

	if(g_Config.m_BcTasTab == 1)
		RenderSettingsTasHelpers(MainView);
	else
		RenderSettingsTas(MainView);
}
