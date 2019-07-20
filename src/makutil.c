#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

#include <sulfur/sulfur.h>

#define PROGRAM_NAME "makutil"

#define VERSION_MAJOR 0
#define VERSION_MINOR 1
#define VERSION_STRING "0.1"
#define VERSION_BUILDSTR "1"

#define FONT_NAME "fixed"

sulfurColor_t colorWhite;
sulfurColor_t colorBlack;

unsigned int fontContext;
xcb_font_t windowFont;

sulfurWindow_t window;
xcb_client_message_event_t event;
xcb_screen_t *screen;
xcb_connection_t* c;
xcb_pixmap_t icon;

const char* homeDir;

/*
=================
Support functions
=================
*/

void Cleanup() {
	SulfurClose();
}

void Quit( int r ) {
	Cleanup();
	exit( r );
}

/*
=============
Main function
=============
*/

int main( int argc, char** argv ) {
	if ( SulfurInit( NULL ) != 0 ) {
			printf( "Problem starting up. Is X running?\n" );
			Cleanup();
			return 1;
	}
	c = sulfurGetXcbConn();
	screen = sulfurGetXcbScreen();

	colorBlack = SULFUR_COLOR_BLACK;
	colorWhite = SULFUR_COLOR_WHITE;

	homeDir = getenv( "HOME" );
	if ( !homeDir ) {
		struct passwd *pw = getpwuid( getuid() );
		homeDir = pw->pw_dir;
	}
	if ( homeDir ) {
		chdir( homeDir );
	}
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 8;
    event.sequence = 0;
    event.window = screen->root;
    event.type = xcb_intern_atom_reply( c, xcb_intern_atom( c, 0, strlen( "_MAKRON_RELOAD" ), "_MAKRON_RELOAD" ), NULL )->atom;
    
    xcb_send_event( c, 0, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, &event );
	xcb_flush( c );

	Cleanup();
	return 0;
}
