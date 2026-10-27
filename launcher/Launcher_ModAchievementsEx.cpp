#include <Windows.h>
#include <iostream>
#include "resource.h"
#include "md5.h"

#define ExecutableFlags CREATE_SUSPENDED | DETACHED_PROCESS

__declspec(noreturn) void exit_msg( const char* format, ... )
{
	va_list arg;
	va_start( arg, format );
	vprintf( format, arg );
	va_end( arg );

	printf( "Exiting in" );
	for ( int i = 3; i > 0; i-- )
	{
		printf( " %is...", i );
		Sleep( 1000 );
	}
	exit( 0 );
}

void load_dll( PROCESS_INFORMATION& pi, const char* pszString, int nStringLen )
{
	void* pBuffer = VirtualAllocEx( pi.hProcess, NULL, nStringLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
	if ( !pBuffer )
	{
		TerminateProcess( pi.hProcess, 0 );
		exit_msg( "Failed to allocate memory to inject '%s'.\n", pszString );
	}

	if ( !WriteProcessMemory( pi.hProcess, pBuffer, pszString, nStringLen, NULL ) )
	{
		TerminateProcess( pi.hProcess, 0 );
		exit_msg( "Failed to write memory to inject '%s'.\n", pszString );
	}

	HANDLE hThread = CreateRemoteThread( pi.hProcess, NULL, NULL, (LPTHREAD_START_ROUTINE)LoadLibraryA, pBuffer, NULL, NULL );
	if ( hThread )
	{
		ResumeThread( hThread ); // we are in a suspended state so we need to resume the thread manually
		WaitForSingleObject( hThread, INFINITE );

		DWORD dwResult = 0;
		GetExitCodeThread( hThread, &dwResult );
		if ( dwResult == 0 )
		{
			TerminateProcess( pi.hProcess, 0 );
			exit_msg( "LoadLibrary failed to load file '%s'.\n", pszString );
		}

		CloseHandle( hThread );
	}
	else
	{
		TerminateProcess( pi.hProcess, 0 );
		exit_msg( "Failed to run LoadLibrary to inject '%s', error code: %i.\n", pszString, GetLastError() );
	}
}

int main( int argc, char* argv[] )
{
	if ( argc <= 0 )
		return 0; // should never happen unless I believe the app gets launched via CreateProcess but doesn't get their command line set properly

	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	ZeroMemory( &si, sizeof( si ) );
	ZeroMemory( &pi, sizeof( pi ) );
	si.cb = sizeof( si );

	// _Unrestricted.exe
	static char szExecutableName[MAX_PATH];
	if ( char* p = strrchr( argv[0], '\\' ) )
		strcpy_s( szExecutableName, p + 1 );
	else
		strcpy_s( szExecutableName, argv[0] ); // we are in a commandline with only the name passed

	if ( char* p = strstr( szExecutableName, "_ModAchievementsEx.exe" ) )
		p[0] = '\0';
	else
		exit_msg( "Name your executable according to the format described on the GitHub page:\n"
			"    <ORIGINAL EXECUTABLE NAME>_ModAchievementsEx.exe\n" );

	// We stripped out the .exe when trimming out the _ModAchievementsEx - add it back!
	strcat_s( szExecutableName, ".exe" );

	static char szCmdLine[32767] = ""; // limit on windows
	strcpy_s( szCmdLine, szExecutableName );
	for ( int i = 1; i < argc; i++ )
	{
		strcat_s( szCmdLine, " " );
		strcat_s( szCmdLine, argv[i] );
	}

	if ( !CreateProcess( nullptr,
		szCmdLine, // command line
		nullptr, nullptr, FALSE, 
		ExecutableFlags, // flags
		nullptr, nullptr, &si, &pi ) )
	{
		exit_msg( "Failed to create process '%s' with %s%s\n", 
			szExecutableName,
			argc > 1 ? "arguments:\n" : "no arguments.",
			argc > 1 ? strchr( szCmdLine, ' ' ) + 1 : "" );
	}

    printf( "Created process '%s' PID( %i )%s%s\n", szExecutableName, pi.dwProcessId,
		argc > 1 ? " with arguments:\n" : "",
		argc > 1 ? strchr( szCmdLine, ' ' ) + 1 : "" );

	// ~~Executable has everything we would ever need besides the steam_api64.dll~~
	// load_dll( pi, "steam_api64.dll", sizeof( "steam_api64.dll" ) );

	HMODULE hModule = NULL; // https://gist.github.com/davidruhmann/8008844
	GetModuleHandleEx( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCTSTR)load_dll, &hModule );
	
	HRSRC hRes = FindResource( hModule, MAKEINTRESOURCE( IDR_DLL1 ), "DLL" );
	if ( !hRes )
	{
		TerminateProcess( pi.hProcess, 0 );
		exit_msg( "Failed to find modded achievements library resource.\n" );
	}

	HGLOBAL hResData = LoadResource( hModule, hRes );
	if ( !hResData )
	{
		TerminateProcess( pi.hProcess, 0 );
		exit_msg( "Failed to load modded achievements library resource.\n" );
	}

	DWORD size = SizeofResource( hModule, hRes );
	void* data = LockResource( hResData );
	if ( !data || !size )
	{
		TerminateProcess( pi.hProcess, 0 );
		exit_msg( "Modded achievements library resource unavailable.\n" );
	}

	struct stat filestat;
	int statval = stat( "ModAchievementsEx.dll", &filestat );
	if ( statval == 0 && filestat.st_size != size )
		statval = 1;

	if ( statval == 0 )
	{
		FILE* file = NULL;
		if ( fopen_s( &file, "ModAchievementsEx.dll", "rb" ) != 0 )
			statval = 1;

		if ( file && statval != 1 )
		{
			unsigned char* filedata = new unsigned char[filestat.st_size];
			if ( fread_s( filedata, filestat.st_size, 1, filestat.st_size, file ) != filestat.st_size )
				statval = 1;

			if ( statval != 1 )
			{
				unsigned char cur_digest[MD5_SIZE], emb_digest[MD5_SIZE];
				md5( cur_digest, filedata, filestat.st_size );
				md5( emb_digest, data, size );
				if ( memcmp( cur_digest, emb_digest, MD5_SIZE ) != 0 )
					statval = 1;
			}

			fclose( file );
			delete[] filedata;
		}
	}

	if ( statval != 0 )
	{
		FILE* file = NULL;
		fopen_s( &file, "ModAchievementsEx.dll", "wb" );
		if ( !file )
		{
			TerminateProcess( pi.hProcess, 0 );
			exit_msg( "Modded achievements library couldn't be extracted.\n" );
		}
		fwrite( data, 1, size, file );
		fclose( file );
	}

#ifdef _DEBUG
	printf( "Waiting for key press to continue..." );
	if ( getchar() != '\n' ) // delay until input so we can attach a debugger
		printf( "\n" );
#endif
	load_dll( pi, "ModAchievementsEx.dll", sizeof( "ModAchievementsEx.dll" ) );

	ResumeThread( pi.hThread );
	exit_msg( "Game loaded successfully.\n" );
}