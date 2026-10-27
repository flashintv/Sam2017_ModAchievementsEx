#include <Windows.h>
#include <Psapi.h>
#include "safetyhook.hpp"
#include "Zydis.h"
#include "steam_api.h"

#define M_ENABLE_ACHIEVEMENTS 1
#define M_ENABLE_LEADERBOARDS 0 // For now disabled as if enabling it while unfinished crashes the game

/* GameEnv feature set flags */
enum EGameEnvCapabilities
{
	FULL_GAME_LICENSE = 1,
	ENABLE_ACHIEVEMENTS = 2,
	DISABLE_PLAYER_NAME_CUSTOMIZATION_IN_MULTIPLAYER_OPTIONS = 4,
	ENABLE_LEADERBOARDS = 8,
	ENABLE_XBOX_LIVE = 16,
	ENABLE_INGAME_ACHIEVEMENTS_VIEWING = 32,
	ENABLE_VOICE_OPTIONS = 64,
	ENABLE_RANKED_OR_NOT = 128,
	DISABLE_DEFAULT_PROFILE_SAVING = 256,
	ENABLE_WORKSHOP = 512,
	ENABLE_LEADERBOARD_PLAYER_DETAILS = 1024,
	ENABLE_GAME_INVITES = 2048,
	ENABLE_VOICE_CHAT = 4096,
	ENABLE_UNSECURED_SESSIONS = 8192
};

// Serious Engine 3 uses the same console logging system as Serious Engine 2
#define FMT_INT32(x)	0xABCD0003u, (int32)(x)
#define FMT_UINT32(x)	0xABCD0004u, (uint32)(x)
#define FMT_INT64(x)	0xABCD0005u, (int64)(x)
#define FMT_UINT64(x)	0xABCD0006u, (uint64)(x)
#define FMT_FLOAT(x)	0xABCD0007u, (double)(x)
#define FMT_STRING(x)	0xABCD0009u, (const char*)(x)
#define FMT_WSTRING(x)	0xABCD000Au, (const wchar_t*)(x)
#define FMT_POINTER(x)	0xABCD000Bu, (void*)(x)

namespace SeriousEngine
{
	void (*SeriousEngine_conPrintF)(unsigned int severity, const char* format, ...) = NULL;
}
#define conPrintF( severity, format, ... )	SeriousEngine_conPrintF( severity, format, __VA_ARGS__ )
#define conErrorF( format, ... )			conPrintF( 32, format, __VA_ARGS__ )
#define conWarningF( format, ... )			conPrintF( 16, format, __VA_ARGS__ )
#define conDebugF( format, ... )			conPrintF( 8, format, __VA_ARGS__ )
#define conInfoF( format, ... )				conPrintF( 4, format, __VA_ARGS__ )
#define conLogF( format, ... )				conPrintF( 1, format, __VA_ARGS__ )

// Allows us to create the stupid achievement icons without having to do a ton of shit
class CSmartObject* (*CreateAchievementIcon)(int iIndex) = NULL;

int* md_bAllowRawLoading = NULL;

// We need to use the same malloc and heap as the game
void* (__fastcall* mem_alloc)(size_t size) = NULL;
void (__cdecl* mem_free)(void* ptr) = NULL;

// Classes and structs for no pointer n offset bullshish
static char g_EmptyString[] = "\x00\x12\x34\x56\x78";
struct CString
{
	const char* m_pString = g_EmptyString;

	CString() {}
	CString( char* pString )
	{
		m_pString = pString;
	}
	~CString()
	{
		// luckily Serious Engine has 00h 12h 34h 56h 78h in the empty string pointer to detect a bad free
		if ( m_pString && ( m_pString[0] != '\0' || m_pString[1] != (char)0x12 || m_pString[2] != (char)0x34 || m_pString[3] != (char)0x56 || m_pString[4] != (char)0x78 ) )
			mem_free( (void*)m_pString );
	}

	operator const char*() const { return m_pString; }
	CString& operator=( const char* pString )
	{ 
		if ( m_pString == pString )
			return *this;

		const char* pNewString = NULL;
		if ( pString )
		{
			const size_t len = strlen( pString ) + 1;
			char* pCopy = (char*)mem_alloc( len );

			if ( !pCopy )
				throw std::bad_alloc();

			memcpy( pCopy, pString, len );
			pNewString = pCopy;
		}

		// luckily Serious Engine has 00h 12h 34h 56h 78h in the empty string pointer to detect a bad free
		if ( m_pString && (m_pString[0] != '\0' || m_pString[1] != (char)0x12 || m_pString[2] != (char)0x34 || m_pString[3] != (char)0x56 || m_pString[4] != (char)0x78) )
			mem_free( (void*)m_pString );

		m_pString = pNewString ? pNewString : g_EmptyString;
		return *this;
	}
	CString& operator=( CString& string )
	{
		return *this = string.m_pString;
	}
};

template <typename T>
struct CStaticStackArray
{
	T* m_pElements;
	int m_Count;
	int m_Capacity;
	int m_GrowSize;

	CStaticStackArray( int nGrowSize = 16 )
	{
		m_pElements = NULL;
		m_Count = 0;
		m_Capacity = 0;
		m_GrowSize = nGrowSize;
	}

	~CStaticStackArray()
	{
		for ( int i = m_Count - 1; i >= 0; i-- )
			m_pElements[i].~T();

		m_Count = 0;
		if ( m_pElements )
			mem_free( (void*)m_pElements );
	}

	void Resize( int nNewCapacity )
	{
		T* pNewElements = static_cast<T*>( mem_alloc( sizeof( T ) * nNewCapacity ) );

		int nCount = min( m_Count, nNewCapacity );
		for ( int i = 0; i < nCount; i++ )
		{
			T* pDst = &pNewElements[i];
			T* pSrc = &m_pElements[i];

			new (pDst) T();
			*pDst = *pSrc;
		}

		for ( int i = m_Count - 1; i >= 0; i-- )
			m_pElements[i].~T();
		mem_free( (void*)m_pElements );

		m_pElements = pNewElements;
		m_Capacity = nNewCapacity;
	}

	T* Push()
	{
		if ( m_Count + 1 > m_Capacity )
			Resize( m_GrowSize * (m_Count / m_GrowSize + 1) );
		return new (&m_pElements[m_Count++]) T();
	}
};

class CSmartObject
{
public:
	virtual ~CSmartObject();
	virtual void OnScriptEventRegistered();
	virtual void pure_unk1();
	virtual CSmartObject* AcquireReplacement();
	virtual bool IsProxyHostOnly();
	virtual void ResolveProxyHost();
	virtual bool IsReplacementReady();

public:
	inline static void Release( CSmartObject*& obj )
	{
		CSmartObject* pobj = obj;
		obj = NULL;
		if ( pobj )
			pobj->RemRef( obj );
	}

	void AddRef()
	{
		InterlockedIncrement( &m_nRefCount );
	}
	void RemRef( const void* p )
	{
		if ( InterlockedExchangeAdd( &m_nRefCount, -1 ) <= 1 )
			AcquireReplacement();
	}

public:
	unsigned int m_unFlags;
	volatile long m_nRefCount;
};

struct AchievementEntry_t
{
	CString name;
	bool unlocked = false;
	bool hidden = false;
	char __pad[6];
	CSmartObject* icon = NULL;
	CString displayname;
	CString description;

	AchievementEntry_t()
	{
		ZeroMemory( __pad, sizeof( __pad ) );
		unlocked = false;
		hidden = false;
		icon = NULL;
	}
};
struct CAchievementInfo
{
	char __pad0[8];
	AchievementEntry_t entry;

	CAchievementInfo() {}
};

class CAchievementStats
{
public:
	CAchievementStats()
		: m_StatsReceivedCallback( this, &CAchievementStats::OnStatsReceivedCallback )
		, m_StatsStoredCallback( this, &CAchievementStats::OnStatsStoredCallback )
		, m_AchievementStoredCallback( this, &CAchievementStats::OnAchievementStoredCallback )
		, m_GameID( SteamUtils()->GetAppID() )
	{
		m_bStatsAvailable = false;
		m_pSteamUserStats = SteamUserStats();
	}

	virtual ~CAchievementStats() {}

	virtual void AwardAchievement( const char* pchName )
	{
		if ( !m_pSteamUserStats )
			return;

		AchievementEntry_t* pAchievement = NULL;
		for ( int i = 0; i < m_Achievements.m_Count; i++ )
			if ( _stricmp( m_Achievements.m_pElements[i].name, pchName ) == 0 )
				pAchievement = &m_Achievements.m_pElements[i];

		if ( !pAchievement )
		{
			SeriousEngine::conErrorF( "Trying to award unregistered Steam achievement \"%1\"!\n", FMT_STRING( pchName ) );
			return;
		}

		if ( !pAchievement->unlocked )
		{
			if ( m_pSteamUserStats->SetAchievement( pAchievement->name ) )
			{
				pAchievement->unlocked = 1;
				CSmartObject::Release( pAchievement->icon );
				dword98 = 1;
			}
			else
			{
				SeriousEngine::conLogF( "Error awarding Steam achievement \"%1\"!\n", FMT_STRING( pchName ) );
			}
		}
	}

public:
	void Populate()
	{
		if ( !m_pSteamUserStats )
			return;

		// Emulate whatever Serious Engine does
		for ( uint32 i = 0; i < m_pSteamUserStats->GetNumAchievements(); i++ )
		{
			AchievementEntry_t* pEntry = m_Achievements.Push();
			pEntry->name = m_pSteamUserStats->GetAchievementName( i );

			// If we don't check for an unlocked, it can cause some achievements to be count as unlocked?
			if ( m_pSteamUserStats->GetAchievement( pEntry->name, &pEntry->unlocked ) )
			{
				pEntry->hidden = pEntry->unlocked ? false : (_stricmp( m_pSteamUserStats->GetAchievementDisplayAttribute( pEntry->name, "hidden" ), "1" ) == 0);
				pEntry->displayname = m_pSteamUserStats->GetAchievementDisplayAttribute( pEntry->name, "name" );
				pEntry->description = m_pSteamUserStats->GetAchievementDisplayAttribute( pEntry->name, "desc" );
				
				/*int iIndex = m_pSteamUserStats->GetAchievementIcon( pszName );
				CSmartObject* pObj = iIndex ? CreateAchievementIcon( iIndex ) : NULL;
				if ( pObj ) pObj->AddRef();
				pEntry->icon = pObj;*/
			}
		}
		m_pSteamUserStats->RequestCurrentStats();
	}

public:
	STEAM_CALLBACK( CAchievementStats, OnStatsReceivedCallback, UserStatsReceived_t, m_StatsReceivedCallback )
	{
		if ( m_GameID.ToUint64() != pParam->m_nGameID )
			return;

		if ( pParam->m_eResult != k_EResultOK )
		{
			SeriousEngine::conErrorF( "Requesting stats from Steam failed: %1\n", FMT_INT32( pParam->m_eResult ) );
			return;
		}

		if ( !m_pSteamUserStats )
			return;

		for ( int i = 0; i < m_Achievements.m_Count; ++i )
		{
			AchievementEntry_t* pAchievement = &m_Achievements.m_pElements[i];
			if ( m_pSteamUserStats->GetAchievement( pAchievement->name, &pAchievement->unlocked ) )
			{
				pAchievement->displayname = m_pSteamUserStats->GetAchievementDisplayAttribute( pAchievement->name, "name" );
				pAchievement->hidden = ( _stricmp( m_pSteamUserStats->GetAchievementDisplayAttribute( pAchievement->name, "hidden" ), "1" ) == 0 );
				if ( !pAchievement->hidden || pAchievement->unlocked )
					pAchievement->description = m_pSteamUserStats->GetAchievementDisplayAttribute( pAchievement->name, "desc" );
			}
		}

		m_bStatsAvailable = true;
	}
	STEAM_CALLBACK( CAchievementStats, OnStatsStoredCallback, UserStatsStored_t, m_StatsStoredCallback )
	{ /* empty */ }
	STEAM_CALLBACK( CAchievementStats, OnAchievementStoredCallback, UserAchievementStored_t, m_AchievementStoredCallback )
	{ /* empty */ }

public:
	CGameID m_GameID;
	bool m_bStatsAvailable;
	BYTE gap71[7];
	ISteamUserStats* m_pSteamUserStats;
	CStaticStackArray<AchievementEntry_t> m_Achievements;
	DWORD dword98;
};

struct CUserStats
{
	CAchievementStats m_AchievementStats;
	DWORD m_bAchievementsEnabled;

	CUserStats( bool bPopulate = false )
	{
		if ( bPopulate )
			m_AchievementStats.Populate();
	}

	bool DoesAchievementExist( char const* pszAchievementName )
	{
		for ( int i = 0; i < m_AchievementStats.m_Achievements.m_Count; i++ )
			if ( _stricmp( m_AchievementStats.m_Achievements.m_pElements[i].name, pszAchievementName ) == 0 )
				return true;
		return false;
	}

	void SetAchievementProgress( char const* pszAchievementName, int nCurProgress, int nMaxProgress )
	{
		if ( m_bAchievementsEnabled )
			m_AchievementStats.m_pSteamUserStats->IndicateAchievementProgress( pszAchievementName, nCurProgress, nMaxProgress );
	}

	void GetAchievementInfos( CStaticStackArray<CAchievementInfo>* pArray )
	{
		if ( !m_bAchievementsEnabled || !m_AchievementStats.m_pSteamUserStats )
			return;
		
		for ( int i = 0; i < m_AchievementStats.m_Achievements.m_Count; i++ )
		{
			// Yes, they are barely different. But that's how it is in Serious Sam.
			AchievementEntry_t* pEntry = &m_AchievementStats.m_Achievements.m_pElements[i];

			CAchievementInfo* pInfo = pArray->Push();

			pInfo->entry.name = pEntry->name;
			pInfo->entry.unlocked = pEntry->unlocked;
			pInfo->entry.hidden = pEntry->unlocked ? false : pEntry->hidden;
			ZeroMemory( pInfo->entry.__pad, sizeof( pInfo->entry.__pad ) );

			if ( pEntry->icon && (pEntry->icon->m_unFlags & 1) != 0 )
				pEntry->icon->IsProxyHostOnly(); // this can't be anything else, so why is this here?
			if ( !pEntry->icon )
			{
				int iIndex = m_AchievementStats.m_pSteamUserStats->GetAchievementIcon( pEntry->name );

				CSmartObject* pObj = iIndex ? CreateAchievementIcon( iIndex ) : NULL;

				// I could simplify this into an operator like Serious Engine does
				CSmartObject* pPrevObj = pEntry->icon;
				if ( pObj ) pObj->AddRef();
				pEntry->icon = pObj;
				if ( pPrevObj ) pPrevObj->RemRef( &pEntry->icon );
			}

			CSmartObject* pPrevInfoObj = pInfo->entry.icon;
			if ( pEntry->icon ) pEntry->icon->AddRef();
			pInfo->entry.icon = pEntry->icon;
			if ( pPrevInfoObj ) pPrevInfoObj->RemRef( &pInfo->entry.icon );

			pInfo->entry.displayname = pEntry->displayname;
			if ( !pEntry->hidden || pEntry->unlocked )
				pInfo->entry.description = pEntry->description;
		}
	}
};

struct CLeaderboardStats
{
	CLeaderboardStats( bool bPopulate = false )
	{

	}
};

struct CUserIndex
{
	int a;
	int b;
};

struct CGameEnvSteam
{
	BYTE gap0[8];
	UINT64 qword8;
	BYTE gap10[400];
	CUserStats* m_pUserStats;
	CLeaderboardStats* m_pLeaderboardStats;
	UINT64 m_LobbyStuff;
	UINT64 qword1B8;
	UINT64 qword1C0;
	BYTE gap1C8[552];
	UINT64 qword3F0;

	void MakeSureStatsExist( bool bPopulate = true )
	{
		if ( !m_pUserStats && M_ENABLE_ACHIEVEMENTS )
		{
			m_pUserStats = (CUserStats*)mem_alloc( sizeof( CUserStats ) );
			new (m_pUserStats) CUserStats( bPopulate );
		}
		if ( !m_pLeaderboardStats && M_ENABLE_LEADERBOARDS )
		{
			m_pLeaderboardStats = (CLeaderboardStats*)mem_alloc( sizeof( CLeaderboardStats ) );
			new (m_pLeaderboardStats) CLeaderboardStats( bPopulate );
		}
	}
};

// Hooks
SafetyHookInline sysStartProcessHook;
long long sysStartProcessHookFn( char const* pszExePath, void* args )
{
	// seems to return a long long unlike a long in Serious Engine 2
	static char szOurExePath[MAX_PATH];
	strncpy_s( szOurExePath, pszExePath, sizeof( szOurExePath ) );
	szOurExePath[sizeof( szOurExePath ) - 1] = '\0';

	if ( char* p = strrchr( szOurExePath, '.' ) ) 
		p[0] = '\0'; // this should in reality return NULL

	// path before this should be \\Sam2017(_VR) or \\Sam2017_Unrestricted(_VR) as we stripped out the .exe
	strcat_s( szOurExePath, "_ModAchievementsEx.exe" );
	return sysStartProcessHook.call<long long>( szOurExePath, &args );
}

SafetyHookMid CGameEnvSteam_InititalizeMidHook;
void CGameEnvSteam_InititalizeMidHookFn( SafetyHookContext& ctx )
{
	// mov [rbx+1A0h], rax
	// rbx = CGameEnvSteam*
	CGameEnvSteam* p = reinterpret_cast<CGameEnvSteam*>( ctx.rbx );

	// internal doesn't populate the achievements for whatever reason (why?)
	// if md_bAllowRawLoading isn't set - we are in internal, populate it manually
	p->MakeSureStatsExist( !md_bAllowRawLoading );
}

SafetyHookInline CGameInfo_GetAchievementsStatusHook;
int CGameInfo_GetAchievementsStatusHookFn( void* )
{
	return 0; // 0 = allowed, 1 = cheating, 2 = trial
}

#pragma region ANTI-TAMPER HOOKS
SafetyHookInline lncVerifyExecutableSignatureHook;
BOOL lncVerifyExecutableSignatureHookFn()
{
	(*md_bAllowRawLoading) *= 2;
	return FALSE;
}

SafetyHookInline sub_140E350A0Hook;
BOOL sub_140E350A0HookFn( uint64 a, uint64 b )
{
	// Make the first argument actually a valid pointer that reads to a non-zero value
	return sub_140E350A0Hook.call<BOOL>( "1", b ); // literally anything but an empty string
}

SafetyHookInline sub_1407C7D70Hook;
void sub_1407C7D70HookFn()
{
	// stupid antitamper per frame function
}

SafetyHookInline cscCheckCodeSegment_OnStepHook;
void cscCheckCodeSegment_OnStepHookFn()
{
	// stupid antitamper code that happens onstep
}
#pragma endregion

// Virtual function replacements
int (*Original_GetCapabilities)( CGameEnvSteam* thisptr );
int GetCapabilities( CGameEnvSteam* thisptr )
{
	// Calling original function, so we add-on instead of replace 
	// (causing leaderboards to break on non-modded, when it's not implemented)
	int caps = Original_GetCapabilities( thisptr );
	/*DISABLE_PLAYER_NAME_CUSTOMIZATION_IN_MULTIPLAYER_OPTIONS
		| ENABLE_INGAME_ACHIEVEMENTS_VIEWING
		| ENABLE_VOICE_OPTIONS
		| ENABLE_WORKSHOP
		| ENABLE_LEADERBOARD_PLAYER_DETAILS
		| ENABLE_VOICE_CHAT
		| ENABLE_UNSECURED_SESSIONS;*/

	// internal has ENABLE_INGAME_ACHIEVEMENTS_VIEWING disabled, for what reason?
	if ( M_ENABLE_ACHIEVEMENTS )
		caps |= ENABLE_ACHIEVEMENTS | ENABLE_INGAME_ACHIEVEMENTS_VIEWING;
	if ( M_ENABLE_LEADERBOARDS )
		caps |= ENABLE_LEADERBOARDS | ENABLE_LEADERBOARD_PLAYER_DETAILS;

	return caps;
}

void UserAwardAchievement( CGameEnvSteam* thisptr, CUserIndex const& iUser, char const* pszAchievementName )
{
	// We don't care about some appid validation or the code guard, as this code should never crash
	if ( iUser.a ) return;

	if ( thisptr->m_pUserStats )
		thisptr->m_pUserStats->m_AchievementStats.AwardAchievement( pszAchievementName );
}

void UserSetAchievementProgress( CGameEnvSteam* thisptr, CUserIndex const& iUser, char const* pszAchievementName, int nCurProgress, int nMaxProgress )
{
	// We don't care about some appid validation or the code guard, as this code should never crash
	if ( iUser.a ) return;

	if ( thisptr->m_pUserStats )
		thisptr->m_pUserStats->SetAchievementProgress( pszAchievementName, nCurProgress, nMaxProgress );
}

void UserGetAchievementInfos( CGameEnvSteam* thisptr, CUserIndex const& iUser, CStaticStackArray<CAchievementInfo>* pArray )
{
	// We don't care about some appid validation or the code guard, as this code should never crash
	if ( iUser.a ) return;

	thisptr->m_pUserStats->GetAchievementInfos( pArray );
}

bool RecognizeAchievementName( CGameEnvSteam* thisptr, char const* pszAchievementName )
{
	// no safety check here
	return thisptr->m_pUserStats->DoesAchievementExist( pszAchievementName );
}

// Helpers
void* FindSignature( const char* pszmodule, const char* pszsignature )
{
	#define INRANGE(x, a, b) (x >= a && x <= b)
	#define getBits(x) (INRANGE((x & (~0x20)), 'A', 'F') ? ((x & (~0x20)) - 'A' + 0xa) : (INRANGE(x, '0', '9') ? x - '0' : 0))
	#define getByte(x) (getBits(x[0]) << 4 | getBits(x[1]))

	MODULEINFO modInfo;
	GetModuleInformation( GetCurrentProcess(), GetModuleHandleA( pszmodule ), &modInfo, sizeof( MODULEINFO ) );
	uintptr_t startAddress = (uintptr_t)modInfo.lpBaseOfDll;
	uintptr_t endAddress = startAddress + modInfo.SizeOfImage;
	const char* pat = pszsignature;
	uintptr_t firstMatch = 0;
	for ( uintptr_t pCur = startAddress; pCur < endAddress; pCur++ ) {
		if ( !*pat )
			return (void*)firstMatch;
		if ( *(PBYTE)pat == '\?' || *(BYTE*)pCur == getByte( pat ) ) {
			if ( !firstMatch )
				firstMatch = pCur;
			if ( !pat[2] )
				return (void*)firstMatch;
			if ( *(PWORD)pat == '\?\?' || *(PBYTE)pat != '\?' )
				pat += 3;
			else
				pat += 2; // one ?
		}
		else {
			pat = pszsignature;
			firstMatch = 0;
		}
	}
	return NULL;
}

std::vector<void*> FindAllFromSignature( const char* pszmodule, const char* pszsignature )
{
	#define INRANGE(x, a, b) (x >= a && x <= b)
	#define getBits(x) (INRANGE((x & (~0x20)), 'A', 'F') ? ((x & (~0x20)) - 'A' + 0xa) : (INRANGE(x, '0', '9') ? x - '0' : 0))
	#define getByte(x) (getBits(x[0]) << 4 | getBits(x[1]))

	// bad code but its not supposed to be amazing dont bother
	std::vector<void*> addrMatches;

	MODULEINFO modInfo;
	GetModuleInformation( GetCurrentProcess(), GetModuleHandleA( pszmodule ), &modInfo, sizeof( MODULEINFO ) );
	uintptr_t startAddress = (uintptr_t)modInfo.lpBaseOfDll;
	uintptr_t endAddress = startAddress + modInfo.SizeOfImage;
	const char* pat = pszsignature;
	uintptr_t firstMatch = 0;
	for ( uintptr_t pCur = startAddress; pCur < endAddress; pCur++ ) {
		if ( !*pat )
		{
			addrMatches.push_back( (void*)firstMatch );
			firstMatch = 0;
			pat = pszsignature;
			continue;
		}
		if ( *(PBYTE)pat == '\?' || *(BYTE*)pCur == getByte( pat ) ) {
			if ( !firstMatch )
				firstMatch = pCur;
			if ( !pat[2] )
			{
				addrMatches.push_back( (void*)firstMatch );
				firstMatch = 0;
				pat = pszsignature;
				continue;
			}
			if ( *(PWORD)pat == '\?\?' || *(PBYTE)pat != '\?' )
				pat += 3;
			else
				pat += 2; // one ?
		}
		else {
			pat = pszsignature;
			firstMatch = 0;
		}
	}
	return addrMatches;
}

void* FindPointer( const char* pszmodule, void* pointer )
{
	if ( !pointer )
		return NULL;

	MODULEINFO modInfo;
	GetModuleInformation( GetCurrentProcess(), GetModuleHandleA( pszmodule ), &modInfo, sizeof( MODULEINFO ) );
	uintptr_t startAddress = (uintptr_t)modInfo.lpBaseOfDll;
	uintptr_t endAddress = startAddress + modInfo.SizeOfImage;

	for ( uintptr_t pCur = startAddress; pCur < endAddress; pCur += sizeof( uintptr_t ) )
		if ( *(uintptr_t*)pCur == (uintptr_t)pointer )
			return (void*)pCur;
	return NULL;
}

template< typename T >
void WriteMemory( T* dest, T p )
{
	DWORD oldprot;
	VirtualProtect( (void*)dest, sizeof( T ), PAGE_EXECUTE_READWRITE, &oldprot );
	*dest = p;
	VirtualProtect( (void*)dest, sizeof( T ), oldprot, &oldprot );
	FlushInstructionCache( GetCurrentProcess(), (void*)dest, sizeof( T ) );
}

void TryDisableChecksumCheck( ZydisDecoder& decoder, const char* szExecutableName, const char* pszPattern )
{
	std::vector<void*> vecMatches = FindAllFromSignature( szExecutableName, pszPattern );
	for ( void* addr : vecMatches )
	{
		// go bytes before the `addr` and look for the last conditional jump and change it to a normal jump
		void* addrjz = NULL;
		for ( uintptr_t offset = 0; offset < 64; )
		{
			void* caddr = reinterpret_cast<void*>( reinterpret_cast<uintptr_t>( addr ) - 64 + offset );

			ZydisDecodedInstruction instruction;
			if ( !ZYAN_SUCCESS( ZydisDecoderDecodeInstruction( &decoder, (ZydisDecoderContext*)ZYAN_NULL, caddr, 15, &instruction ) ) )
			{
				offset++;
				continue;
			}

			// from what i seen in the tens of functions i looked at it, it was only a JZ
			if ( instruction.mnemonic == ZYDIS_MNEMONIC_JZ )
				addrjz = caddr;
			offset += instruction.length;
		}
		if ( addrjz )
			WriteMemory( reinterpret_cast<byte*>(addrjz), byte( 0xEB ) );
		else if ( IsDebuggerPresent() )
			DebugBreak();
	}
}

// Entry point
BOOL APIENTRY DllMain( HINSTANCE hModule, DWORD reason, LPVOID lpReserved )
{
	switch ( reason )
	{
	case DLL_PROCESS_ATTACH:
	{
		DisableThreadLibraryCalls( hModule );

		char szExecutableName[MAX_PATH];
		GetModuleFileName( NULL, szExecutableName, MAX_PATH );
		szExecutableName[MAX_PATH - 1] = '\0';
		if ( char* p = strrchr( szExecutableName, '\\' ) ) memmove( szExecutableName, p + 1, strlen( p + 1 ) + 1 );

		// conPrintF so we can print stuff to the console
		SeriousEngine::SeriousEngine_conPrintF = reinterpret_cast< decltype( SeriousEngine::SeriousEngine_conPrintF ) >( 
			FindSignature( szExecutableName, "48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20 53 48 83 EC 30 8B D9" ) );

		CreateAchievementIcon = reinterpret_cast< decltype( CreateAchievementIcon ) >(
			FindSignature( szExecutableName, "41 56 41 57 48 83 EC 48 44 8B F9" ) );

		// I hate this but we must
		mem_free = reinterpret_cast<decltype(mem_free)>(
			FindSignature( szExecutableName, "48 85 C9 0F 84 EC 00 00 00 53" ));
		mem_alloc = reinterpret_cast<decltype(mem_alloc)>(
			FindSignature( szExecutableName, "48 89 5C 24 20 57 48 83 EC 20 8B 15" ));

		// This is the exact same as Serious Engine 2, just without an export
		sysStartProcessHook = safetyhook::create_inline(
			FindSignature( szExecutableName, "48 8B C4 53 41 56 48 83 EC 58 48 89 68 08 48 89 70 E8" ),
			sysStartProcessHookFn );

		// Unrestricted doesn't initialize achievement and leaderboard data
		CGameEnvSteam_InititalizeMidHook = safetyhook::create_mid(
			FindSignature( szExecutableName, "FF 15 ?? ?? ?? ?? 48 85 C0 74 3E" ),
			CGameEnvSteam_InititalizeMidHookFn );

		// Fix profile-based achievements not unlocking 
		CGameInfo_GetAchievementsStatusHook = safetyhook::create_inline(
			FindSignature( szExecutableName, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 85 C0 75 3B" ),
			CGameInfo_GetAchievementsStatusHookFn );

		// Remove if cheating checks from awarding
		std::vector<void*> vecMatches = FindAllFromSignature( szExecutableName, "48 8D 4F 28 E8 ?? ?? ?? ?? 83 38 00 ?? ?? 83 BF F8 01 00 00 00" );
		for ( void* addr : vecMatches )
		{
			byte patch[] = { 0x31, 0xC0 };

			DWORD oldprot;
			VirtualProtect( addr, 21, PAGE_EXECUTE_READWRITE, &oldprot );
			memset( addr, 0x90, 21 );
			memcpy( addr, patch, sizeof( patch ) );
			VirtualProtect( addr, 21, oldprot, &oldprot );
			FlushInstructionCache( GetCurrentProcess(), addr, 21 );
		}

		#pragma region ANTI-TAMPER HOOKS SETUP
		// Used by anti-tamper code
		void* md_bAllowRawLoadingSig = FindSignature( szExecutableName, "D1 25 ?? ?? ?? ?? 48 8D 8C 24 50 02 00 00" );
		if ( md_bAllowRawLoadingSig )
		{
			int* md_bAllowRawLoadingIns = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(md_bAllowRawLoadingSig) + 2);
			md_bAllowRawLoading = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(md_bAllowRawLoadingIns) + 4 + (*md_bAllowRawLoadingIns));
		}

		// lncVerifyExecutableSignature
		void* lncVerifyExecutableSignature = FindSignature( szExecutableName, "40 53 48 81 EC 30 02 00 00" );
		if ( lncVerifyExecutableSignature )
		{
			lncVerifyExecutableSignatureHook = safetyhook::create_inline(
				lncVerifyExecutableSignature,
				lncVerifyExecutableSignatureHookFn );
		}

		// Should get rid of the anti-tamper that fucks up the movement keys.
		void* sub_140E350A0 = FindSignature( szExecutableName, "40 56 57 48 83 EC 58 83 7A 08 00" );
		if ( sub_140E350A0 )
		{
			sub_140E350A0Hook = safetyhook::create_inline(
				sub_140E350A0,
				sub_140E350A0HookFn );
		}

		// Should fix all of the game crashes due to stack overflow because of checksum mismatches on public
		ZydisDecoder decoder;
		if ( !ZYAN_SUCCESS( ZydisDecoderInit( &decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64 ) ) )
			return FALSE;
		// Should be 97 matches, can be shortened down to FF C1 89 0D ?? ?? ?? ?? 8D 48
		TryDisableChecksumCheck( decoder, szExecutableName, "FF C1 89 0D ?? ?? ?? ?? 8D 48 01 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 83 F9 ?? ?? ?? EB ??" );
		// Should be 87 matches, prob can be shortened out, haven't checked.
		TryDisableChecksumCheck( decoder, szExecutableName, "48 8B 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 45 33 C9 45 33 C0 E8 ?? ?? ?? ??" );

		// Anti-tamper inside of TextureConvertor service
		void* sub_1407C7D70 = FindSignature( szExecutableName, "48 83 EC 48 E8 ?? ?? ?? ?? 48 85 C0 0F 84 98 02 00 00" );
		if ( sub_1407C7D70 )
		{
			sub_1407C7D70Hook = safetyhook::create_inline(
				sub_1407C7D70,
				sub_1407C7D70HookFn );
		}

		void* cscCheckCodeSegment_OnStep = FindSignature( szExecutableName, "48 83 EC 48 48 85 C9 0F 84 E1 02 00 00" );
		if ( cscCheckCodeSegment_OnStep )
		{
			cscCheckCodeSegment_OnStepHook = safetyhook::create_inline(
				FindSignature( szExecutableName, "48 83 EC 48 48 85 C9 0F 84 E1 02 00 00" ),
				cscCheckCodeSegment_OnStepHookFn );
		}
		#pragma endregion

		/*
		UserAwardAchievement(CUserIndex const&, char const*)
		UserSetAchievementProgress(CUserIndex const&, char const*, int, int)
		UserGetAchievementInfos(CUserIndex const&, CStaticStackArray<CAchievementInfo>*)
		CacheUserAchievementInfos(CUserIndex const&)
		RecognizeAchievementName(char const*)
		LeaderboardEnumerate()
		LeaderboardWrite(CUserIndex const&, char const*, CStringValueMap const&, int)
		LeaderboardGetWriteStatus(int)
		LeaderboardPrepareReading(CUserIndex const&, char const*, LeaderboardReadMethod, CString const*, int)
		LeaderboardGetUserRow()
		LeaderboardGetNumRows()
		LeaderboardGetRow(int)
		LeaderboardIsReading()
		LeaderboardReadHasFailed()
		*/

		void* pInitialize = FindSignature( szExecutableName, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8D 6C 24 B0" );
		void* pInitializeVtblPos = FindPointer( szExecutableName, pInitialize );
		uintptr_t* pGetCapabilitiesVtblPos = reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(pInitializeVtblPos) + (3 * sizeof( uintptr_t )));
		Original_GetCapabilities = reinterpret_cast< decltype( Original_GetCapabilities ) >( *pGetCapabilitiesVtblPos );
		WriteMemory( pGetCapabilitiesVtblPos, reinterpret_cast<uintptr_t>(&GetCapabilities) );

		// I needed a non-version specific way of getting the vtable positions on all 6 versions
		void* pGetDesiredLanguage = FindSignature( szExecutableName, "40 53 48 83 EC 20 48 8B DA FF 15 ?? ?? ?? ?? 48 8B C8" );
		void* pGetDesiredLanguageVtblPos = FindPointer( szExecutableName, pGetDesiredLanguage );
		uintptr_t* pAwardAchievementVtblPos = reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(pGetDesiredLanguageVtblPos) + (9 * sizeof( uintptr_t )));
		WriteMemory( pAwardAchievementVtblPos + 0, reinterpret_cast<uintptr_t>(&UserAwardAchievement) );
		WriteMemory( pAwardAchievementVtblPos + 1, reinterpret_cast<uintptr_t>(&UserSetAchievementProgress) );
		WriteMemory( pAwardAchievementVtblPos + 2, reinterpret_cast<uintptr_t>(&UserGetAchievementInfos) );
		WriteMemory( pAwardAchievementVtblPos + 4, reinterpret_cast<uintptr_t>(&RecognizeAchievementName) );

		break;
	}
	case DLL_PROCESS_DETACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	}

	return TRUE;
}