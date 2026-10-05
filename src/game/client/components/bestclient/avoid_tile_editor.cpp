/* Copyright © 2026 BestProject Team */
#include "avoid_tile_editor.h"

#include <game/collision.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>

namespace Avoid
{
	namespace
	{
		constexpr float TILE_SIZE = 32.0f;
	}

	void CTileEditor::ClearAll()
	{
		m_TunnelTiles.clear();
		m_FinishTiles.clear();
		Touch();
	}

	void CTileEditor::MarkTunnel(int TileX, int TileY)
	{
		if(m_TunnelTiles.insert(Key(TileX, TileY)).second)
			Touch();
	}

	void CTileEditor::MarkFinish(int TileX, int TileY)
	{
		// A tile is either a goal or a corridor tile, never both: leaving it in the tunnel set
		// would make the movement restriction reject the very tile the planner has to reach.
		m_TunnelTiles.erase(Key(TileX, TileY));
		if(m_FinishTiles.insert(Key(TileX, TileY)).second)
			Touch();
	}

	void CTileEditor::Erase(int TileX, int TileY)
	{
		const bool RemovedTunnel = m_TunnelTiles.erase(Key(TileX, TileY)) != 0;
		const bool RemovedFinish = m_FinishTiles.erase(Key(TileX, TileY)) != 0;
		if(RemovedTunnel || RemovedFinish)
			Touch();
	}

	void CTileEditor::Interact(int Width, int Height, vec2 WorldMousePos, bool LeftClick, bool RightClick, int Type)
	{
		const int TileX = (int)std::floor(WorldMousePos.x / TILE_SIZE);
		const int TileY = (int)std::floor(WorldMousePos.y / TILE_SIZE);
		if(TileX < 0 || TileX >= Width || TileY < 0 || TileY >= Height)
			return;

		if(LeftClick)
		{
			if(Type == TYPE_FINISH)
				MarkFinish(TileX, TileY);
			else
				MarkTunnel(TileX, TileY);
		}
		else if(RightClick)
		{
			Erase(TileX, TileY);
		}
	}

	int CTileEditor::AutoFinish(CCollision *pCollision)
	{
		if(!pCollision || pCollision->GetWidth() <= 0 || pCollision->GetHeight() <= 0)
			return 0;

		// Reference spec 9.3: a finish tile is marked when either layer carries TILE_FINISH. The
		// tile editor of the reference walks the whole map, so the scan is deliberately not
		// restricted to the tiles that are currently visible.
		int Found = 0;
		for(int y = 0; y < pCollision->GetHeight(); ++y)
		{
			for(int x = 0; x < pCollision->GetWidth(); ++x)
			{
				const int Index = pCollision->GetIndex(x, y);
				if(Index < 0)
					continue;
				if(pCollision->GetTileIndex(Index) == TILE_FINISH ||
					pCollision->GetFrontTileIndex(Index) == TILE_FINISH)
				{
					MarkFinish(x, y);
					Found++;
				}
			}
		}
		return Found;
	}

	int CTileEditor::AutoTunnels(const std::vector<vec2> &vTrajectory, int Width, int MapWidth, int MapHeight)
	{
		if(vTrajectory.empty() || MapWidth <= 0 || MapHeight <= 0)
			return 0;

		const int Radius = std::clamp(Width, 0, 10);
		int Marked = 0;
		for(const vec2 &Pos : vTrajectory)
		{
			if(!std::isfinite(Pos.x) || !std::isfinite(Pos.y))
				continue;
			const int BaseX = (int)std::floor(Pos.x / TILE_SIZE);
			const int BaseY = (int)std::floor(Pos.y / TILE_SIZE);
			for(int dy = -Radius; dy <= Radius; ++dy)
			{
				for(int dx = -Radius; dx <= Radius; ++dx)
				{
					const int TileX = BaseX + dx;
					const int TileY = BaseY + dy;
					if(TileX < 0 || TileX >= MapWidth || TileY < 0 || TileY >= MapHeight)
						continue;
					if(m_TunnelTiles.insert(Key(TileX, TileY)).second)
					{
						// A tunnel tile is a corridor tile; a goal that happens to sit inside the
						// tube stays a goal, and the grid keeps it walkable for that reason.
						Marked++;
					}
				}
			}
		}
		if(Marked > 0)
			Touch();
		return Marked;
	}
} // namespace Avoid
