
#include "Globals.h"  // NOTE: MSVC stupidness requires this to be the same across all modules

#include "BlockID.h"
#include "cWorld.h"
#include "cRedstone.h"
#include "cChunk.h"
#include "cClientHandle.h"
#include "cPickup.h"
#include "cBlockToPickup.h"
#include "cPlayer.h"
#include "cServer.h"
#include "cItem.h"
#include "cRoot.h"
#include "../iniFile/iniFile.h"
#include "cChunkMap.h"
#include "cSimulatorManager.h"
#include "cWaterSimulator.h"
#include "cLavaSimulator.h"
#include "cFireSimulator.h"
#include "cSandSimulator.h"
#include "cRedstoneSimulator.h"
#include "cChicken.h"
#include "cSpider.h"
#include "cCow.h" //cow
#include "cSquid.h" //Squid
#include "cWolf.h" //wolf
#include "cSlime.h" //slime
#include "cSkeleton.h" //Skeleton
#include "cSilverfish.h" //Silverfish
#include "cPig.h" //pig
#include "cSheep.h" //sheep
#include "cZombie.h" //zombie
#include "cEnderman.h" //enderman
#include "cCreeper.h" //creeper
#include "cCavespider.h" //cavespider
#include "cGhast.h" //Ghast
#include "cZombiepigman.h" //Zombiepigman
#include "cGenSettings.h"
#include "cMakeDir.h"
#include "cChunkGenerator.h"
#include "MersenneTwister.h"
#include "cWorldGenerator_Test.h"
#include "cTracer.h"


#include "packets/cPacket_TimeUpdate.h"
#include "packets/cPacket_NewInvalidState.h"
#include "packets/cPacket_Thunderbolt.h"

#include "Vector3d.h"

#include <time.h>

#include "tolua++.h"

#ifndef _WIN32
	#include <stdlib.h>
#endif





/// Up to this many m_SpreadQueue elements are handled each world tick
const int MAX_LIGHTING_SPREAD_PER_TICK = 10;





float cWorld::m_Time = 0.f;

char g_BlockLightValue[128];
char g_BlockSpreadLightFalloff[128];
bool g_BlockTransparent[128];
bool g_BlockOneHitDig[128];
bool g_BlockPistonBreakable[128];





///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// cWorldLoadProgress:

/// A simple thread that displays the progress of world loading / saving in cWorld::InitializeSpawn()
class cWorldLoadProgress :
	public cIsThread
{
public:
	cWorldLoadProgress(cWorld * a_World) :
		cIsThread("cWorldLoadProgress"),
		m_World(a_World)
	{
		Start();
	}
	
protected:

	cWorld * m_World;
	
	virtual void Execute(void) override
	{
		for (;;)
		{
			LOG("%d chunks to load, %d chunks to generate", 
				m_World->GetStorage().GetLoadQueueLength(),
				m_World->GetGenerator().GetQueueLength()
			);
			
			// Wait for 2 sec, but be "reasonably wakeable" when the thread is to finish
			for (int i = 0; i < 20; i++)
			{
				cSleep::MilliSleep(100);
				if (m_ShouldTerminate)
				{
					return;
				}
			}
		}  // for (-ever)
	}
	
} ;





///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// cWorld:

cWorld* cWorld::GetWorld()
{
	LOGWARN("WARNING: Using deprecated function cWorld::GetWorld() use cRoot::Get()->GetWorld() instead!");
	return cRoot::Get()->GetWorld();
}





cWorld::~cWorld()
{
	{
		cCSLock Lock(m_CSEntities);
		while( m_AllEntities.begin() != m_AllEntities.end() )
		{
			cEntity* Entity = *m_AllEntities.begin();
			m_AllEntities.remove( Entity );
			if ( !Entity->IsDestroyed() )
			{
				Entity->Destroy();
			}
			delete Entity;
		}
	}

	delete m_SimulatorManager;
	delete m_SandSimulator;
	delete m_WaterSimulator;
	delete m_LavaSimulator;
	delete m_FireSimulator;
	delete m_RedstoneSimulator;

	m_Generator.Stop();

	UnloadUnusedChunks();
	
	m_Storage.WaitForFinish();

	delete m_ChunkMap;
}





cWorld::cWorld( const AString & a_WorldName )
	: m_SpawnMonsterTime( 0.f )
	, m_RSList ( 0 )
	, m_Weather ( eWeather_Sunny )
{
	LOG("cWorld::cWorld(%s)", a_WorldName.c_str());
	m_WorldName = a_WorldName;

	cMakeDir::MakeDir(m_WorldName.c_str());

	MTRand r1;
	m_SpawnX = (double)((r1.randInt()%1000)-500);
	m_SpawnY = cChunk::c_ChunkHeight;
	m_SpawnZ = (double)((r1.randInt()%1000)-500);
	m_WorldSeed = r1.randInt();
	m_GameMode = eGameMode_Creative;

	AString GeneratorName;
	AString StorageSchema("Default");

	cIniFile IniFile( m_WorldName + "/world.ini");
	if( IniFile.ReadFile() )
	{
		m_SpawnX = IniFile.GetValueF("SpawnPosition", "X", m_SpawnX );
		m_SpawnY = IniFile.GetValueF("SpawnPosition", "Y", m_SpawnY );
		m_SpawnZ = IniFile.GetValueF("SpawnPosition", "Z", m_SpawnZ );
		m_WorldSeed = IniFile.GetValueI("Seed", "Seed", m_WorldSeed );
		m_GameMode = (eGameMode)IniFile.GetValueI("GameMode", "GameMode", m_GameMode );
		GeneratorName = IniFile.GetValue("Generator", "GeneratorName", GeneratorName);
		StorageSchema = IniFile.GetValue("Storage", "Schema", StorageSchema);
	}
	else
	{
		IniFile.SetValueF("SpawnPosition", "X", m_SpawnX );
		IniFile.SetValueF("SpawnPosition", "Y", m_SpawnY );
		IniFile.SetValueF("SpawnPosition", "Z", m_SpawnZ );
		IniFile.SetValueI("Seed", "Seed", m_WorldSeed );
		IniFile.SetValueI("GameMode", "GameMode", m_GameMode );
		IniFile.SetValue("Generator", "GeneratorName", GeneratorName);
		IniFile.SetValue("Storage", "Schema", StorageSchema);
		if( !IniFile.WriteFile() )
		{
			LOG("WARNING: Could not write to %s/world.ini", a_WorldName.c_str());
		}
	}
	LOGINFO("Seed: %i", m_WorldSeed );

	m_Storage.Start(this, StorageSchema);
	m_Generator.Start(this, GeneratorName);

	m_bAnimals = true;
	m_SpawnMonsterRate = 10;
	cIniFile IniFile2("settings.ini");
	if( IniFile2.ReadFile() )
	{
		m_bAnimals = IniFile2.GetValueB("Monsters", "AnimalsOn", true );
		m_SpawnMonsterRate = (float)IniFile2.GetValueF("Monsters", "AnimalSpawnInterval", 10 );
		SetMaxPlayers(IniFile2.GetValueI("Server", "MaxPlayers", 9001));
		m_Description = IniFile2.GetValue("Server", "Description", "MCServer! - It's OVER 9000!").c_str();
	}

	m_ChunkMap = new cChunkMap(this );
	
	m_ChunkSender.Start(this);

	m_Time = 0;
	m_WorldTimeFraction = 0.f;
	m_WorldTime = 0;
	m_LastSave = 0;
	m_LastUnload = 0;

	//Simulators:
	m_WaterSimulator = new cWaterSimulator( this );
	m_LavaSimulator = new cLavaSimulator( this );
	m_SandSimulator = new cSandSimulator(this);
	m_FireSimulator = new cFireSimulator(this);
	m_RedstoneSimulator = new cRedstoneSimulator(this);

	m_SimulatorManager = new cSimulatorManager();
	m_SimulatorManager->RegisterSimulator(m_WaterSimulator, 6);
	m_SimulatorManager->RegisterSimulator(m_LavaSimulator, 12);
	m_SimulatorManager->RegisterSimulator(m_SandSimulator, 1);
	m_SimulatorManager->RegisterSimulator(m_FireSimulator, 10);
	m_SimulatorManager->RegisterSimulator(m_RedstoneSimulator, 1);

	memset( g_BlockLightValue,         0x0, sizeof( g_BlockLightValue ) );
	memset( g_BlockSpreadLightFalloff, 0xf, sizeof( g_BlockSpreadLightFalloff ) ); // 0xf means total falloff
	memset( g_BlockTransparent,        0x0, sizeof( g_BlockTransparent ) );
	memset( g_BlockOneHitDig,          0x0, sizeof( g_BlockOneHitDig ) );
	memset( g_BlockPistonBreakable,    0x0, sizeof( g_BlockPistonBreakable ) );

	// Emissive blocks
	g_BlockLightValue[ E_BLOCK_TORCH ] =			14;
	g_BlockLightValue[ E_BLOCK_FIRE ] =				15;
	g_BlockLightValue[ E_BLOCK_LAVA ] =				15;
	g_BlockLightValue[ E_BLOCK_STATIONARY_LAVA ] =	15;
	g_BlockLightValue[ E_BLOCK_GLOWSTONE ] =		15;

	// Spread blocks
	g_BlockSpreadLightFalloff[ E_BLOCK_AIR ]				= 1;
	g_BlockSpreadLightFalloff[ E_BLOCK_TORCH ]				= 1;
	g_BlockSpreadLightFalloff[ E_BLOCK_FIRE ]				= 1;
	g_BlockSpreadLightFalloff[ E_BLOCK_LAVA ]				= 1;
	g_BlockSpreadLightFalloff[ E_BLOCK_STATIONARY_LAVA ]	= 1;
	g_BlockSpreadLightFalloff[ E_BLOCK_WATER ]				= 4;				// Light in water dissapears faster
	g_BlockSpreadLightFalloff[ E_BLOCK_STATIONARY_WATER ]	= 4;
	g_BlockSpreadLightFalloff[ E_BLOCK_LEAVES ]				= 1;
	g_BlockSpreadLightFalloff[ E_BLOCK_GLASS ]				= 1;
	g_BlockSpreadLightFalloff[ E_BLOCK_GLOWSTONE ]			= 1;
	g_BlockSpreadLightFalloff[ E_BLOCK_SIGN_POST ]			= 1;
	g_BlockSpreadLightFalloff[ E_BLOCK_WALLSIGN ]			= 1;

	// Transparent blocks
	g_BlockTransparent[ E_BLOCK_AIR ]		= true;
	g_BlockTransparent[ E_BLOCK_GLASS ]		= true;
	g_BlockTransparent[ E_BLOCK_FIRE ]		= true;
	g_BlockTransparent[ E_BLOCK_ICE ] 		= true;
	g_BlockTransparent[ E_BLOCK_TORCH ]		= true;
	g_BlockTransparent[ E_BLOCK_SIGN_POST ] = true;
	g_BlockTransparent[ E_BLOCK_WALLSIGN ]	= true;
	
	// TODO: Also set flowers, mushrooms etc as transparent

	// One hit break blocks
	g_BlockOneHitDig[ E_BLOCK_SAPLING ]				= true;
	g_BlockOneHitDig[ E_BLOCK_YELLOW_FLOWER ]		= true;
	g_BlockOneHitDig[ E_BLOCK_RED_ROSE ]			= true;
	g_BlockOneHitDig[ E_BLOCK_BROWN_MUSHROOM ]		= true;
	g_BlockOneHitDig[ E_BLOCK_RED_MUSHROOM ]		= true;
	g_BlockOneHitDig[ E_BLOCK_TNT ]					= true;
	g_BlockOneHitDig[ E_BLOCK_TORCH ]				= true;
	g_BlockOneHitDig[ E_BLOCK_REDSTONE_WIRE ]		= true;
	g_BlockOneHitDig[ E_BLOCK_CROPS ]				= true;
	g_BlockOneHitDig[ E_BLOCK_REDSTONE_TORCH_OFF ]	= true;
	g_BlockOneHitDig[ E_BLOCK_REDSTONE_TORCH_ON ]	= true;
	g_BlockOneHitDig[ E_BLOCK_REEDS ]				= true;
	g_BlockOneHitDig[ E_BLOCK_REDSTONE_WIRE ]		= true;
	g_BlockOneHitDig[ E_BLOCK_REDSTONE_REPEATER_OFF ]		= true;
	g_BlockOneHitDig[ E_BLOCK_REDSTONE_REPEATER_ON ]		= true;
	g_BlockOneHitDig[ E_BLOCK_LOCKED_CHEST ]		= true;
	g_BlockOneHitDig [ E_BLOCK_FIRE ]				= true;

	// Blocks that breaks when pushed by piston
	g_BlockPistonBreakable[ E_BLOCK_AIR ]				= true;
	g_BlockPistonBreakable[ E_BLOCK_STATIONARY_WATER ]	= false;		//This gave pistons the ability to drop water :D
	g_BlockPistonBreakable[ E_BLOCK_WATER ]				= false;
	g_BlockPistonBreakable[ E_BLOCK_STATIONARY_LAVA ]	= false;
	g_BlockPistonBreakable[ E_BLOCK_LAVA ]				= false;
	g_BlockPistonBreakable[ E_BLOCK_BED ]				= true;
	g_BlockPistonBreakable[ E_BLOCK_COBWEB ]			= true;
	g_BlockPistonBreakable[ E_BLOCK_TALL_GRASS ]		= true;
	g_BlockPistonBreakable[ E_BLOCK_YELLOW_FLOWER ]		= true;
	g_BlockPistonBreakable[ E_BLOCK_BROWN_MUSHROOM ]	= true;
	g_BlockPistonBreakable[ E_BLOCK_RED_ROSE ]			= true;
	g_BlockPistonBreakable[ E_BLOCK_RED_MUSHROOM ]		= true;
	g_BlockPistonBreakable[ E_BLOCK_DEAD_BUSH ]			= true;
	g_BlockPistonBreakable[ E_BLOCK_TORCH ]				= true;
	g_BlockPistonBreakable[ E_BLOCK_FIRE ]				= true;
	g_BlockPistonBreakable[ E_BLOCK_REDSTONE_WIRE ]		= true;
	g_BlockPistonBreakable[ E_BLOCK_CROPS ]				= true;
	g_BlockPistonBreakable[ E_BLOCK_LADDER ]			= true;
	g_BlockPistonBreakable[ E_BLOCK_WOODEN_DOOR ]		= true;
	g_BlockPistonBreakable[ E_BLOCK_IRON_DOOR ]			= true;
	g_BlockPistonBreakable[ E_BLOCK_LEVER ]				= true;
	g_BlockPistonBreakable[ E_BLOCK_STONE_BUTTON ]		= true;
	g_BlockPistonBreakable[ E_BLOCK_REDSTONE_TORCH_ON ]	= true;
	g_BlockPistonBreakable[ E_BLOCK_REDSTONE_TORCH_OFF ]= true;
	g_BlockPistonBreakable[ E_BLOCK_SNOW ]				= true;
	g_BlockPistonBreakable[ E_BLOCK_REEDS ]				= true;
	g_BlockPistonBreakable[ E_BLOCK_PUMPKIN_STEM ]		= true;
	g_BlockPistonBreakable[ E_BLOCK_MELON_STEM ]		= true;
	g_BlockPistonBreakable[ E_BLOCK_MELON ]				= true;
	g_BlockPistonBreakable[ E_BLOCK_PUMPKIN ]			= true;
	g_BlockPistonBreakable[ E_BLOCK_JACK_O_LANTERN ]	= true;
	g_BlockPistonBreakable[ E_BLOCK_VINES ]				= true;
	g_BlockPistonBreakable[ E_BLOCK_STONE_PRESSURE_PLATE ] = true;
	g_BlockPistonBreakable[ E_BLOCK_WOODEN_PRESSURE_PLATE ] = true;
}





void cWorld::SetWeather( eWeather a_Weather )
{
	switch( a_Weather )
	{
	case eWeather_Sunny:
		{
			m_Weather = a_Weather;
			cPacket_NewInvalidState WeatherPacket;
			WeatherPacket.m_Reason = 2; //stop rain
			Broadcast ( WeatherPacket );
		}
		break;
	case eWeather_Rain:
		{
			m_Weather = a_Weather;
			cPacket_NewInvalidState WeatherPacket;
			WeatherPacket.m_Reason = 1; //begin rain
			Broadcast ( WeatherPacket );
		}
		break;
	case eWeather_ThunderStorm:
		{
			m_Weather = a_Weather;
			cPacket_NewInvalidState WeatherPacket;
			WeatherPacket.m_Reason = 1; //begin rain
			Broadcast ( WeatherPacket );
			CastThunderbolt ( 0, 0, 0 ); //start thunderstorm with a lightning strike at 0, 0, 0. >:D
		}
		break;
	default:
		LOGWARN("Trying to set unknown weather %d", a_Weather );
		break;
	}
}





void cWorld::CastThunderbolt ( int a_X, int a_Y, int a_Z )
{
	cPacket_Thunderbolt ThunderboltPacket;
	ThunderboltPacket.m_xLBPos = a_X;
	ThunderboltPacket.m_yLBPos = a_Y;
	ThunderboltPacket.m_zLBPos = a_Z;
	Broadcast( ThunderboltPacket ); // FIXME: Broadcast to chunk instead of entire world
}





void cWorld::InitializeSpawn()
{
	int ChunkX = 0, ChunkY = 0, ChunkZ = 0;
	BlockToChunk( (int)m_SpawnX, (int)m_SpawnY, (int)m_SpawnZ, ChunkX, ChunkY, ChunkZ );
	
	// For the debugging builds, don't make the server build too much world upon start:
	#ifdef _DEBUG
	int ViewDist = 9;
	#else
	int ViewDist = 20;  // Always prepare an area 20 chunks across, no matter what the actual cClientHandle::VIEWDISTANCE is
	#endif  // _DEBUG
	
	LOG("Preparing spawn area in world \"%s\"", m_WorldName.c_str());
	for (int x = 0; x < ViewDist; x++)
	{
		for (int z = 0; z < ViewDist; z++)
		{
			m_ChunkMap->TouchChunk( x + ChunkX-(ViewDist - 1) / 2, 0, z + ChunkZ-(ViewDist - 1) / 2 );  // Queue the chunk in the generator / loader
		}
	}
	
	// Display progress during this process:
	cWorldLoadProgress Progress(this);
	
	// Wait for the loader to finish loading
	m_Storage.WaitForQueuesEmpty();
	
	// Wait for the generator to finish generating
	m_Generator.WaitForQueueEmpty();
	
	m_SpawnY = (double)GetHeight( (int)m_SpawnX, (int)m_SpawnZ ) + 1.6f; // +1.6f eye height
}





void cWorld::Tick(float a_Dt)
{
	m_Time += a_Dt / 1000.f;

	CurrentTick++;

	bool bSendTime = false;
	m_WorldTimeFraction += a_Dt / 1000.f;
	while ( m_WorldTimeFraction > 1.f )
	{
		m_WorldTimeFraction -= 1.f;
		m_WorldTime += 20;
		bSendTime = true;
	}
	m_WorldTime %= 24000; // 24000 units in a day
	if ( bSendTime )
	{
		Broadcast( cPacket_TimeUpdate( (m_WorldTime) ) );
	}

	{
		cCSLock Lock(m_CSEntities);
		for (cEntityList::iterator itr = m_AllEntities.begin(); itr != m_AllEntities.end();)
		{
			if ((*itr)->IsDestroyed())
			{
				LOG("Destroying entity #%i", (*itr)->GetUniqueID());
				cEntity * RemoveMe = *itr;
				itr = m_AllEntities.erase( itr );
				m_RemoveEntityQueue.push_back( RemoveMe ); 
				continue;
			}
			(*itr)->Tick(a_Dt);
			itr++;
		}
	}

	TickLighting();

	m_ChunkMap->Tick(a_Dt, m_TickRand);
	
	GetSimulatorManager()->Simulate(a_Dt);

	TickWeather(a_Dt);

	// Asynchronously set blocks:
	sSetBlockList FastSetBlockQueueCopy;
	{
		cCSLock Lock(m_CSFastSetBlock);
		std::swap(FastSetBlockQueueCopy, m_FastSetBlockQueue);
	}
	m_ChunkMap->FastSetBlocks(FastSetBlockQueueCopy);
	if (FastSetBlockQueueCopy.size() > 0)
	{
		// Some blocks failed, store them for next tick:
		cCSLock Lock(m_CSFastSetBlock);
		m_FastSetBlockQueue.splice(m_FastSetBlockQueue.end(), FastSetBlockQueueCopy);
	}

	if( m_Time - m_LastSave > 60 * 5 ) // Save each 5 minutes
	{
		SaveAllChunks();
	}

	if( m_Time - m_LastUnload > 10 ) // Unload every 10 seconds
	{
		UnloadUnusedChunks();
	}

	// Delete entities queued for removal:
	for (cEntityList::iterator itr = m_RemoveEntityQueue.begin(); itr != m_RemoveEntityQueue.end(); ++itr)
	{
		delete *itr;
	}
	m_RemoveEntityQueue.clear();

	TickSpawnMobs(a_Dt);

	std::vector<int> m_RSList_copy(m_RSList);
	
	m_RSList.clear();

	std::vector<int>::const_iterator cii;	// FIXME - Please rename this variable, WTF is cii??? Use human readable variable names or common abbreviations (i, idx, itr, iter)
	for(cii=m_RSList_copy.begin(); cii!=m_RSList_copy.end();)
	{
		int tempX = *cii;cii++;
		int tempY = *cii;cii++;
		int tempZ = *cii;cii++;
		int state = *cii;cii++;
		
		if ( (state == 11111) && ( (int)GetBlock( tempX, tempY, tempZ ) == E_BLOCK_REDSTONE_TORCH_OFF ) )
		{
			FastSetBlock( tempX, tempY, tempZ, E_BLOCK_REDSTONE_TORCH_ON, (int)GetBlockMeta( tempX, tempY, tempZ ) );
			cRedstone Redstone(this);
			Redstone.ChangeRedstone( tempX, tempY, tempZ, true );
		}
		else if ( (state == 00000) && ( (int)GetBlock( tempX, tempY, tempZ ) == E_BLOCK_REDSTONE_TORCH_ON ) )
		{
			FastSetBlock( tempX, tempY, tempZ, E_BLOCK_REDSTONE_TORCH_OFF, (int)GetBlockMeta( tempX, tempY, tempZ ) );
			cRedstone Redstone(this);
			Redstone.ChangeRedstone( tempX, tempY, tempZ, false );
		}
	}
	m_RSList_copy.erase(m_RSList_copy.begin(),m_RSList_copy.end());
}





void cWorld::TickWeather(float a_Dt)
{
	if ( GetWeather() == 0 )  // if sunny
	{
		if( CurrentTick % 19 == 0 )  //every 20 ticks random weather
		{
			unsigned randWeather = (m_TickRand.randInt() % 10000);
			if (randWeather == 0)
			{
				LOG("Starting Rainstorm!");
				SetWeather ( eWeather_Rain );
			}
			else if  (randWeather == 1)
			{
				LOG("Starting Thunderstorm!");
				SetWeather ( eWeather_ThunderStorm );
			}
		}
	}

	if ( GetWeather() != 0 )  // if raining or thunderstorm
	{
		if ( CurrentTick % 19 == 0 ) // every 20 ticks random weather
		{
			unsigned randWeather = (m_TickRand.randInt() % 4999);
			if (randWeather == 0)  //2% chance per second
			{
				LOG("Back to sunny!");
				SetWeather ( eWeather_Sunny );
			}
			else if ( (randWeather > 4000) && (GetWeather() != 2) )  // random chance for rainstorm to turn into thunderstorm.
			{
				LOG("Starting Thunderstorm!");
				SetWeather ( eWeather_ThunderStorm );
			}
		}
	}

	if ( GetWeather() == 2 )  // if thunderstorm
	{
		if (m_TickRand.randInt() % 199 == 0)  // 0.5% chance per tick of thunderbolt
		{
			CastThunderbolt ( 0, 0, 0 );  // TODO: find random possitions near players to cast thunderbolts.
		}
	}
}





void cWorld::TickSpawnMobs(float a_Dt)
{
	if (!m_bAnimals || (m_Time - m_SpawnMonsterTime <= m_SpawnMonsterRate))
	{
		return;
	}
	
	m_SpawnMonsterTime = m_Time;
	Vector3d SpawnPos;
	{
		cCSLock Lock(m_CSPlayers);
		if ( m_Players.size() <= 0)
		{
			return;
		}
		int RandomPlayerIdx = m_TickRand.randInt() & m_Players.size();
		cPlayerList::iterator itr = m_Players.begin();
		for( int i = 1; i < RandomPlayerIdx; i++ )
		{
			itr++;
		}
		SpawnPos = (*itr)->GetPosition();
	}

	cMonster * Monster = NULL;
	int dayRand   = m_TickRand.randInt() % 6;
	int nightRand = m_TickRand.randInt() % 10;

	SpawnPos += Vector3d( (double)(m_TickRand.randInt() % 64) - 32, (double)(m_TickRand.randInt() % 64) - 32, (double)(m_TickRand.randInt() % 64) - 32 );
	int Height = GetHeight( (int)SpawnPos.x, (int)SpawnPos.z );

	if (m_WorldTime >= 12000 + 1000)
	{
		if (nightRand == 0) //random percent to spawn for night
			Monster = new cSpider();
		else if (nightRand == 1)
			Monster = new cZombie();
		else if (nightRand == 2)
			Monster = new cEnderman();
		else if (nightRand == 3)
			Monster = new cCreeper();
		else if (nightRand == 4)
			Monster = new cCavespider();
		else if (nightRand == 5)
			Monster = new cGhast();
		else if (nightRand == 6)
			Monster = new cZombiepigman();
		else if (nightRand == 7)
			Monster = new cSlime();
		else if (nightRand == 8)
			Monster = new cSilverfish();
		else if (nightRand == 9)
			Monster = new cSkeleton();
		//end random percent to spawn for night
	}
	else
	{
		if (dayRand == 0) //random percent to spawn for day
			Monster = new cChicken();
		else if (dayRand == 1)
			Monster = new cCow();
		else if (dayRand == 2)
			Monster = new cPig();
		else if (dayRand == 3)
			Monster = new cSheep();
		else if (dayRand == 4)
			Monster = new cSquid();
		else if (dayRand == 5)
			Monster = new cWolf();
		//end random percent to spawn for day
	}

	if( Monster )
	{
		Monster->Initialize( this );
		Monster->TeleportTo( SpawnPos.x, (double)(Height) + 2, SpawnPos.z );
		Monster->SpawnOn(0);
	}
}





void cWorld::TickLighting(void)
{
	// To avoid a deadlock, we lock the spread queue only long enough to pick the chunk coords to spread
	// The spreading itself will run unlocked
	cChunkCoordsList SpreadQueue;
	{
		cCSLock Lock(m_CSLighting);
		if (m_SpreadQueue.size() == 0)
		{
			return;
		}
		if (m_SpreadQueue.size() >= MAX_LIGHTING_SPREAD_PER_TICK )
		{
			LOGWARN("cWorld: Lots of lighting to do! Still %i chunks left!", m_SpreadQueue.size() );
		}
		// Move up to MAX_LIGHTING_SPREAD_PER_TICK elements from m_SpreadQueue out into SpreadQueue:
		cChunkCoordsList::iterator itr = m_SpreadQueue.begin();
		std::advance(itr, MIN(m_SpreadQueue.size(), MAX_LIGHTING_SPREAD_PER_TICK));
		SpreadQueue.splice(SpreadQueue.begin(), m_SpreadQueue, m_SpreadQueue.begin(), itr);
	}
	
	for (cChunkCoordsList::iterator itr = SpreadQueue.begin(); itr != SpreadQueue.end(); ++itr)
	{
		m_ChunkMap->SpreadChunkLighting(itr->m_ChunkX, itr->m_ChunkY, itr->m_ChunkZ);
	}
}





void cWorld::GrowTree( int a_X, int a_Y, int a_Z )
{
	// new tree code, looks much better
	// with help from seanj
	// converted from php to lua then lua to c++

	// build trunk
	MTRand r1;
	int trunk = r1.randInt() % (7 - 5 + 1) + 5;
	for (int i = 0; i < trunk; i++) 
	{
		FastSetBlock( a_X, a_Y + i, a_Z, E_BLOCK_LOG, 0 );
	}

	// build tree
	for (int j = 0; j < trunk; j++)
	{
		int radius = trunk - j;
		if (radius < 4)
		{
			if (radius > 2)
			{
				radius = 2;
			}
			for (int i = a_X - radius; i <= a_X + radius; i++)
			{
				for (int k = a_Z-radius; k <= a_Z + radius; k++)
				{
					// small chance to be missing a block to add a little random
					if (k != a_Z || i != a_X && (r1.randInt() % 100 + 1) > 20)
					{
						if( GetBlock( i, a_Y + j, k ) == E_BLOCK_AIR )
						{
							FastSetBlock(i, a_Y+j, k, E_BLOCK_LEAVES, 0 );
						}
					}
					else
					{
						//if( m_BlockType[ MakeIndex(i, TopY+j, k) ] == E_BLOCK_AIR )
						//	m_BlockType[ MakeIndex(i, TopY+j, k) ] = E_BLOCK_LEAVES;
					}
				}
			}
			if (GetBlock( a_X, a_Y+j, a_Z ) == E_BLOCK_AIR )
			{
				FastSetBlock( a_X, a_Y+j, a_Z, E_BLOCK_LOG, 0 );
			}
		}
	}

	// do the top
	if( GetBlock( a_X+1, a_Y+trunk, a_Z ) == E_BLOCK_AIR )
		FastSetBlock( a_X+1, a_Y+trunk, a_Z, E_BLOCK_LEAVES, 0 );

	if( GetBlock( a_X-1, a_Y+trunk, a_Z ) == E_BLOCK_AIR )
		FastSetBlock( a_X-1, a_Y+trunk, a_Z, E_BLOCK_LEAVES, 0 );

	if( GetBlock( a_X, a_Y+trunk, a_Z+1 ) == E_BLOCK_AIR )
		FastSetBlock( a_X, a_Y+trunk, a_Z+1, E_BLOCK_LEAVES, 0 );

	if( GetBlock( a_X, a_Y+trunk, a_Z-1 ) == E_BLOCK_AIR )
		FastSetBlock( a_X, a_Y+trunk, a_Z-1, E_BLOCK_LEAVES, 0 );

	if( GetBlock( a_X, a_Y+trunk, a_Z ) == E_BLOCK_AIR )
		FastSetBlock( a_X, a_Y+trunk, a_Z, E_BLOCK_LEAVES, 0 );

	// end new tree code
}





void cWorld::SetBlock( int a_X, int a_Y, int a_Z, char a_BlockType, char a_BlockMeta )
{
	m_ChunkMap->SetBlock(a_X, a_Y, a_Z, a_BlockType, a_BlockMeta);

	GetSimulatorManager()->WakeUp(a_X, a_Y, a_Z);
}





void cWorld::FastSetBlock( int a_X, int a_Y, int a_Z, char a_BlockType, char a_BlockMeta )
{
	cCSLock Lock(m_CSFastSetBlock);
	m_FastSetBlockQueue.push_back(sSetBlock(a_X, a_Y, a_Z, a_BlockType, a_BlockMeta)); 
}





char cWorld::GetBlock(int a_X, int a_Y, int a_Z)
{
	// First check if it isn't queued in the m_FastSetBlockQueue:
	{
		int X = a_X, Y = a_Y, Z = a_Z;
		int ChunkX, ChunkY, ChunkZ;
		AbsoluteToRelative(X, Y, Z, ChunkX, ChunkY, ChunkZ);
		
		cCSLock Lock(m_CSFastSetBlock);
		for (sSetBlockList::iterator itr = m_FastSetBlockQueue.begin(); itr != m_FastSetBlockQueue.end(); ++itr)
		{
			if ((itr->x == X) && (itr->y == Y) && (itr->z == Z) && (itr->ChunkX == ChunkX) && (itr->ChunkZ == ChunkZ))
			{
				return itr->BlockType;
			}
		}  // for itr - m_FastSetBlockQueue[]
	}
	
	return m_ChunkMap->GetBlock(a_X, a_Y, a_Z);
}





char cWorld::GetBlockMeta( int a_X, int a_Y, int a_Z )
{
	// First check if it isn't queued in the m_FastSetBlockQueue:
	{
		cCSLock Lock(m_CSFastSetBlock);
		for (sSetBlockList::iterator itr = m_FastSetBlockQueue.begin(); itr != m_FastSetBlockQueue.end(); ++itr)
		{
			if ((itr->x == a_X) && (itr->y == a_Y) && (itr->y == a_Y))
			{
				return itr->BlockMeta;
			}
		}  // for itr - m_FastSetBlockQueue[]
	}
	
	return m_ChunkMap->GetBlockMeta(a_X, a_Y, a_Z);
}





void cWorld::SetBlockMeta( int a_X, int a_Y, int a_Z, char a_MetaData )
{
	m_ChunkMap->SetBlockMeta(a_X, a_Y, a_Z, a_MetaData);
}





bool cWorld::DigBlock( int a_X, int a_Y, int a_Z, cItem & a_PickupItem )
{
	bool res = m_ChunkMap->DigBlock(a_X, a_Y, a_Z, a_PickupItem);
	if (res)
	{
		GetSimulatorManager()->WakeUp(a_X, a_Y, a_Z);
	}
	return res;
}





void cWorld::SendBlockTo( int a_X, int a_Y, int a_Z, cPlayer * a_Player )
{
	m_ChunkMap->SendBlockTo(a_X, a_Y, a_Z, a_Player);
}





// TODO: This interface is dangerous!
cBlockEntity * cWorld::GetBlockEntity( int a_X, int a_Y, int a_Z )
{
	return NULL;
}





int cWorld::GetHeight( int a_X, int a_Z )
{
	return m_ChunkMap->GetHeight(a_X, a_Z);
}





const double & cWorld::GetSpawnY(void)
{
	return m_SpawnY;
}




void cWorld::Broadcast( const cPacket & a_Packet, cClientHandle * a_Exclude)
{
	cCSLock Lock(m_CSPlayers);
	for (cPlayerList::iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
	{
		cClientHandle * ch = (*itr)->GetClientHandle();
		if ((ch == a_Exclude) || (ch == NULL) || !ch->IsLoggedIn() || ch->IsDestroyed())
		{
			continue;
		}
		(*itr)->GetClientHandle()->Send( a_Packet );
	}
}





void cWorld::BroadcastToChunk(int a_ChunkX, int a_ChunkY, int a_ChunkZ, const cPacket & a_Packet, cClientHandle * a_Exclude)
{
	m_ChunkMap->BroadcastToChunk(a_ChunkX, a_ChunkY, a_ChunkZ, a_Packet, a_Exclude);
}





void cWorld::BroadcastToChunkOfBlock(int a_X, int a_Y, int a_Z, cPacket * a_Packet, cClientHandle * a_Exclude)
{
	m_ChunkMap->BroadcastToChunkOfBlock(a_X, a_Y, a_Z, a_Packet, a_Exclude);
}





void cWorld::MarkChunkDirty (int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->MarkChunkDirty (a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::MarkChunkSaving(int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->MarkChunkSaving(a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::MarkChunkSaved (int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->MarkChunkSaved (a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::ChunkDataLoaded(int a_ChunkX, int a_ChunkY, int a_ChunkZ, const char * a_BlockData, cEntityList & a_Entities, cBlockEntityList & a_BlockEntities)
{
	m_ChunkMap->ChunkDataLoaded(a_ChunkX, a_ChunkY, a_ChunkZ, a_BlockData, a_Entities, a_BlockEntities);
	m_ChunkSender.ChunkReady(a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::ChunkDataGenerated(int a_ChunkX, int a_ChunkY, int a_ChunkZ, const char * a_BlockData, cEntityList & a_Entities, cBlockEntityList & a_BlockEntities)
{
	m_ChunkMap->ChunkDataGenerated(a_ChunkX, a_ChunkY, a_ChunkZ, a_BlockData, a_Entities, a_BlockEntities);
	m_ChunkSender.ChunkReady(a_ChunkX, a_ChunkY, a_ChunkZ);
}





bool cWorld::GetChunkData(int a_ChunkX, int a_ChunkY, int a_ChunkZ, cChunkDataCallback & a_Callback)
{
	return m_ChunkMap->GetChunkData(a_ChunkX, a_ChunkY, a_ChunkZ, a_Callback);
}





bool cWorld::GetChunkBlocks(int a_ChunkX, int a_ChunkY, int a_ChunkZ, char * a_Blocks)
{
	return m_ChunkMap->GetChunkBlocks(a_ChunkX, a_ChunkY, a_ChunkZ, a_Blocks);
}





bool cWorld::GetChunkBlockData(int a_ChunkX, int a_ChunkY, int a_ChunkZ, char * a_BlockData)
{
	return m_ChunkMap->GetChunkBlockData(a_ChunkX, a_ChunkY, a_ChunkZ, a_BlockData);
}





bool cWorld::IsChunkValid(int a_ChunkX, int a_ChunkY, int a_ChunkZ) const
{
	return m_ChunkMap->IsChunkValid(a_ChunkX, a_ChunkY, a_ChunkZ);
}





bool cWorld::HasChunkAnyClients(int a_ChunkX, int a_ChunkY, int a_ChunkZ) const
{
	return m_ChunkMap->HasChunkAnyClients(a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::UnloadUnusedChunks(void )
{
	m_LastUnload = m_Time;
	m_ChunkMap->UnloadUnusedChunks();
}





void cWorld::CollectPickupsByPlayer(cPlayer * a_Player)
{
	m_ChunkMap->CollectPickupsByPlayer(a_Player);
}





void cWorld::SetMaxPlayers(int iMax)
{
	m_MaxPlayers = MAX_PLAYERS;
	if (iMax > 0 && iMax < MAX_PLAYERS)
	{
		m_MaxPlayers = iMax;
	}
}





void cWorld::AddPlayer( cPlayer* a_Player )
{
	cCSLock Lock(m_CSPlayers);
	
	ASSERT(std::find(m_Players.begin(), m_Players.end(), a_Player) == m_Players.end());  // Is it already in the list? HOW?
	
	m_Players.remove( a_Player );  // Make sure the player is registered only once
	m_Players.push_back( a_Player );
}





void cWorld::RemovePlayer( cPlayer* a_Player )
{
	cCSLock Lock(m_CSPlayers);
	m_Players.remove( a_Player );
}





bool cWorld::ForEachPlayer(cPlayerListCallback & a_Callback)
{
	// Calls the callback for each player in the list
	cCSLock Lock(m_CSPlayers);
	for (cPlayerList::iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
	{
		if (a_Callback.Item(*itr))
		{
			return false;
		}
	}  // for itr - m_Players[]
	return true;
}




// TODO: This interface is dangerous!
cPlayer* cWorld::GetPlayer( const char* a_PlayerName )
{
	cPlayer* BestMatch = 0;
	unsigned int MatchedLetters = 0;
	unsigned int NumMatches = 0;
	bool bPerfectMatch = false;

	unsigned int NameLength = strlen( a_PlayerName );
	cCSLock Lock(m_CSPlayers);
	for (cPlayerList::iterator itr = m_Players.begin(); itr != m_Players.end(); itr++ )
	{
		std::string Name = (*itr)->GetName();
		if( NameLength > Name.length() ) continue; // Definitely not a match

		for (unsigned int i = 0; i < NameLength; i++)
		{
			char c1 = (char)toupper( a_PlayerName[i] );
			char c2 = (char)toupper( Name[i] );
			if( c1 == c2 )
			{
				if( i+1 > MatchedLetters )
				{
					MatchedLetters = i+1;
					BestMatch = *itr;
				}
				if( i+1 == NameLength )
				{
					NumMatches++;
					if( NameLength == Name.length() )
					{
						bPerfectMatch = true;
						break;
					}
				}
			}
			else
			{
				if( BestMatch == *itr ) BestMatch = 0;
				break;
			}
			if( bPerfectMatch )
				break;
		}
	}
	if ( NumMatches == 1 )
	{
		return BestMatch;
	}

	// More than one matches, so it's undefined. Return NULL instead
	return NULL;
}





cPlayer * cWorld::FindClosestPlayer(const Vector3f & a_Pos, float a_SightLimit)
{
	cTracer LineOfSight(this);

	float ClosestDistance = a_SightLimit;
	cPlayer* ClosestPlayer = NULL;

	cCSLock Lock(m_CSPlayers);
	for (cPlayerList::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
	{
		Vector3f Pos = (*itr)->GetPosition();
		float Distance = (Pos - a_Pos).Length();

		if (Distance <= a_SightLimit)
		{
			if (!LineOfSight.Trace(a_Pos,(Pos - a_Pos),(int)(Pos - a_Pos).Length()))
			{
				if (Distance < ClosestDistance)
				{
					ClosestDistance = Distance;
					ClosestPlayer = *itr;
				}
			}
		}
	}
	return ClosestPlayer;
}





void cWorld::SendPlayerList(cPlayer * a_DestPlayer)
{
	// Sends the playerlist to a_DestPlayer
	cCSLock Lock(m_CSPlayers);
	for ( cPlayerList::iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
	{
		cClientHandle * ch = (*itr)->GetClientHandle();
		if ((ch != NULL) && !ch->IsDestroyed())
		{
			cPacket_PlayerListItem PlayerListItem((*itr)->GetColor() + (*itr)->GetName(), true, (*itr)->GetClientHandle()->GetPing());
			a_DestPlayer->GetClientHandle()->Send( PlayerListItem );
		}
	}
}





bool cWorld::DoWithEntity( int a_UniqueID, cEntityCallback & a_Callback )
{
	cCSLock Lock(m_CSEntities);
	for (cEntityList::iterator itr = m_AllEntities.begin(); itr != m_AllEntities.end(); ++itr )
	{
		if( (*itr)->GetUniqueID() == a_UniqueID )
		{
			return a_Callback.Item(*itr);
		}
	} // for itr - m_AllEntities[]
	return false;
}





void cWorld::RemoveEntityFromChunk(cEntity * a_Entity, int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->RemoveEntityFromChunk(a_Entity, a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::MoveEntityToChunk(cEntity * a_Entity, int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->MoveEntityToChunk(a_Entity, a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::CompareChunkClients(int a_ChunkX1, int a_ChunkY1, int a_ChunkZ1, int a_ChunkX2, int a_ChunkY2, int a_ChunkZ2, cClientDiffCallback & a_Callback)
{
	m_ChunkMap->CompareChunkClients(a_ChunkX1, a_ChunkY1, a_ChunkZ1, a_ChunkX2, a_ChunkY2, a_ChunkZ2, a_Callback);
}





bool cWorld::AddChunkClient(int a_ChunkX, int a_ChunkY, int a_ChunkZ, cClientHandle * a_Client)
{
	return m_ChunkMap->AddChunkClient(a_ChunkX, a_ChunkY, a_ChunkZ, a_Client);
}





void cWorld::RemoveChunkClient(int a_ChunkX, int a_ChunkY, int a_ChunkZ, cClientHandle * a_Client)
{
	m_ChunkMap->RemoveChunkClient(a_ChunkX, a_ChunkY, a_ChunkZ, a_Client);
}





void cWorld::RemoveClientFromChunks(cClientHandle * a_Client, const cChunkCoordsList & a_Chunks)
{
	m_ChunkMap->RemoveClientFromChunks(a_Client, a_Chunks);
}





void cWorld::SendChunkTo(int a_ChunkX, int a_ChunkY, int a_ChunkZ, cClientHandle * a_Client)
{
	m_ChunkSender.QueueSendChunkTo(a_ChunkX, a_ChunkY, a_ChunkZ, a_Client);
}





void cWorld::RemoveClientFromChunkSender(cClientHandle * a_Client)
{
	m_ChunkSender.RemoveClient(a_Client);
}





void cWorld::TouchChunk(int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->TouchChunk(a_ChunkX, a_ChunkY, a_ChunkZ);
}





bool cWorld::LoadChunk(int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	return m_ChunkMap->LoadChunk(a_ChunkX, a_ChunkY, a_ChunkZ);
}



	

void cWorld::LoadChunks(const cChunkCoordsList & a_Chunks)
{
	m_ChunkMap->LoadChunks(a_Chunks);
}





void cWorld::ChunkLoadFailed(int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->ChunkLoadFailed(a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::UpdateSign(int a_X, int a_Y, int a_Z, const AString & a_Line1, const AString & a_Line2, const AString & a_Line3, const AString & a_Line4)
{
	m_ChunkMap->UpdateSign(a_X, a_Y, a_Z, a_Line1, a_Line2, a_Line3, a_Line4);
}





void cWorld::ChunksStay(const cChunkCoordsList & a_Chunks, bool a_Stay)
{
	m_ChunkMap->ChunksStay(a_Chunks, a_Stay);
}





void cWorld::SaveAllChunks()
{
	LOG("Saving all chunks...");
	m_LastSave = m_Time;
	m_ChunkMap->SaveAllChunks();
}





void cWorld::ReSpreadLighting(int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	cCSLock Lock(m_CSLighting);
	m_SpreadQueue.remove(cChunkCoords(a_ChunkX, a_ChunkY, a_ChunkZ)); 
	m_SpreadQueue.push_back(cChunkCoords(a_ChunkX, a_ChunkY, a_ChunkZ));
}





void cWorld::RemoveSpread(int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	cCSLock Lock(m_CSLighting);
	m_SpreadQueue.remove(cChunkCoords(a_ChunkX, a_ChunkY, a_ChunkZ));
}





/************************************************************************/
/* Get and set                                                          */
/************************************************************************/
// void cWorld::AddClient( cClientHandle* a_Client )
// {
// 	m_m_Clients.push_back( a_Client );
// }
// cWorld::ClientList & cWorld::GetClients()
// {
// 	return m_m_Clients;
// }





void cWorld::AddEntity( cEntity* a_Entity )
{
	cCSLock Lock(m_CSEntities);
	m_AllEntities.push_back( a_Entity ); 
}





unsigned int cWorld::GetNumPlayers()
{
	cCSLock Lock(m_CSPlayers);
	return m_Players.size(); 
}





int cWorld::GetNumChunks(void) const
{
	return m_ChunkMap->GetNumChunks();
}




