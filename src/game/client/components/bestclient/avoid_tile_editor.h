/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_TILE_EDITOR_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_TILE_EDITOR_H

#include <base/vmath.h>

#include <cstdint>
#include <unordered_set>
#include <vector>

class CCollision;

namespace Avoid
{
	/* -----------------------------------------------------------------------------------------
	 * CTileEditor - the tile editor of the reference client (spec 9).
	 *
	 * It owns two tile sets: the "tunnel" tiles a planner is allowed to walk on and the "finish"
	 * tiles it has to reach. Both are handed to CNavigator::Rebuild(), which is where they turn
	 * into the goal set and the movement restriction of the flow field. Nothing in here touches the
	 * map itself, and every mutation bumps Revision() so that the planners know their grid and
	 * their flow field are stale.
	 * --------------------------------------------------------------------------------------- */
	class CTileEditor
	{
	public:
		enum
		{
			TYPE_TUNNEL = 0,
			TYPE_FINISH = 1,
		};

		// Tile coordinates are map absolute; the reference stores them as (y << 32) | x.
		static uint64_t Key(int TileX, int TileY)
		{
			return ((uint64_t)(uint32_t)TileY << 32) | (uint32_t)TileX;
		}

		bool HasTunnels() const { return !m_TunnelTiles.empty(); }
		bool HasFinish() const { return !m_FinishTiles.empty(); }
		bool IsTunnel(int TileX, int TileY) const { return m_TunnelTiles.find(Key(TileX, TileY)) != m_TunnelTiles.end(); }
		bool IsFinish(int TileX, int TileY) const { return m_FinishTiles.find(Key(TileX, TileY)) != m_FinishTiles.end(); }
		int TunnelCount() const { return (int)m_TunnelTiles.size(); }
		int FinishCount() const { return (int)m_FinishTiles.size(); }

		// Reference spec 9.2: drop every edited tile.
		void ClearAll();
		void MarkTunnel(int TileX, int TileY);
		void MarkFinish(int TileX, int TileY);
		void Erase(int TileX, int TileY);
		// Reference spec 9.5: left click adds (tunnel or finish), right click erases.
		void Interact(int Width, int Height, vec2 WorldMousePos, bool LeftClick, bool RightClick, int Type);
		// Reference spec 9.3: scan the game and front layer for TILE_FINISH (34).
		int AutoFinish(CCollision *pCollision);
		// Reference spec 9.4: expand a loaded TAS trajectory into a tunnel of Width tiles around
		// every recorded position.
		int AutoTunnels(const std::vector<vec2> &vTrajectory, int Width, int MapWidth, int MapHeight);

		// Bumped whenever a tile set changes.
		unsigned Revision() const { return m_Revision; }
		void Touch() { m_Revision++; }

	private:
		std::unordered_set<uint64_t> m_TunnelTiles;
		std::unordered_set<uint64_t> m_FinishTiles;
		unsigned m_Revision = 1;
	};
} // namespace Avoid

#endif // GAME_CLIENT_COMPONENTS_BESTCLIENT_AVOID_TILE_EDITOR_H
