#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <stdarg.h>
#include <xcb/xcb.h>
#include <xcb/xcb_atom.h>
#include <sulfur/sulfur.h>

#define PROGRAM_NAME "makron"

#define VERSION_MAJOR 0
#define VERSION_MINOR 2
#define VERSION_PATCH 0
#define VERSION_STRING "0.2.0"
#define VERSION_BUILDSTR "38"

#define BORDER_SIZE_LEFT 1
#define BORDER_SIZE_RIGHT 1
#define BORDER_SIZE_TOP 19
#define BORDER_SIZE_BOTTOM 1

#define FONT_NAME "fixed"

#define DEBUGLEVEL 0

typedef enum {
	STATE_WITHDRAWN = 0,
	STATE_ICON = 1,
	STATE_NORMAL = 3,
} clientWindowState_t;

typedef enum {
	STATE_NO_REDIRECT, //override redirect
	STATE_INIT,
	STATE_REPARENTED,
	STATE_TRANSIENT
} clientManagementState_t;

typedef enum {
	WMSTATE_IDLE,
	WMSTATE_DRAG,
	WMSTATE_CLOSE
} wmState_t;

typedef enum {
	NODE_ROOT,
	NODE_CLIENT,
	NODE_FRAME,
	NODE_GROUP,
} nodeType_t;

typedef struct node_s {
	nodeType_t type;

	xcb_window_t window;
	xcb_window_t parent;
	
	struct node_s* ppparent;

	char parentMapped;

	short width;
	short height;
	short x;
	short y;

	clientWindowState_t windowState;
	clientManagementState_t managementState;

	char name[256];

	//todo: gravity

	struct node_s *nextNode; // todo: remove this

	int childrenMax;
	struct node_s **children;
} node_t;

node_t *rootNode = NULL;
node_t *firstClient = NULL;
xcb_connection_t *c;
xcb_screen_t *screen;
xcb_generic_event_t *e;

int debugLevel = 99;

sulfurColor_t colorWhite;
sulfurColor_t colorLightGrey;
sulfurColor_t colorGrey;
sulfurColor_t colorDarkGrey;
sulfurColor_t colorBlack;
sulfurColor_t colorLightAccent;
sulfurColor_t colorAccent;
sulfurColor_t colorDarkAccent;

unsigned int inactiveFontContext;
unsigned int activeFontContext;

xcb_font_t windowFont;

xcb_atom_t WM_DELETE_WINDOW;
xcb_atom_t WM_PROTOCOLS;

wmState_t wmState = WMSTATE_IDLE;
node_t *dragClient;
short dragStartX;
short dragStartY;
short mouseLastKnownX;
short mouseLastKnownY;
short mouseIsOverCloseButton;

xcb_window_t activeWindow;

// list of all windows, in most recently raised order
int windowListMax = 4;
node_t** windowList;

/*
=================
Support functions
=================
*/

void Cleanup( void );
void Quit( int r );

void dbgprintf( int level, char* fmt, ... ) {
	va_list args;
	va_start( args, fmt );
	if ( level >= DEBUGLEVEL ) {
		vprintf( fmt, args );
	}
	va_end( args );
}

void AddNodeToList( node_t* n, node_t** list, int* top ) {
	int i;

	for ( i = 0 ; i < *top; i++ ) {
		if ( list[i] == NULL ) {
			list[i] = n;
			return;
		}
	}
	dprintf( 2, "growing client list\n" );
	windowList = realloc( windowList, sizeof ( node_t ) * ( windowListMax += 4 ) );
	if ( windowList == NULL ) {
		fprintf( stderr, "failure growing client list\n" );
		Quit( 2 );
	}
	dprintf( 2, "client list size is %i\n", *top );
	AddNodeToList( n, list, top );
}

void RemoveNodeFromList( node_t* n, node_t** list, int* top ) {
	int i;

	for ( i = 0 ; i < *top; i++ ) {
		if ( list[i] == n ) {
			list[i] = NULL;
			break;
		}
	}
	for ( i++ ; i < *top; i++ ) {
		list[i - 1] = list[i];
		if ( list[i] == NULL ) {
			break;
		}
	}
	if ( i < *top - 4 ) {
		dprintf( 2, "shrinking client list\n" );
		windowList = realloc( windowList, windowListMax -= 4 );
		if ( windowList == NULL ) {
			fprintf( stderr, "failure shrinking client list\n" );
			Quit( 2 );
		}
		dprintf( 2, "client list size is %i\n", *top );
	}

}

node_t* CreateNode( nodeType_t type, xcb_window_t wnd, node_t* parent, short width, short height, short x, short y ) {
	node_t* n = calloc( 1, sizeof( node_t ) );
	if ( !n )
		return NULL;
	n->childrenMax = 4;
	n->children = calloc( n->childrenMax, sizeof( node_t*) );
	if ( !n->children ) {
		free(n);
		return NULL;
	}

	n->type = type;
	n->window = wnd;
	n->ppparent = parent;
	n->width = width;
	n->height = height;
	n->x = x;
	n->y = y;
	return n;
}

void DestroyNode( node_t* n ) {
	int i;

	if ( !n || n->type == NODE_ROOT )
		return;

	// reparent any child windows
	if ( n->children && n->ppparent && n->ppparent->children ) {
		for ( i = 0; ( i < n->childrenMax ) && ( n->children[i] != NULL ); i++ ) {
			xcb_reparent_window( c, n->window, n->ppparent->window, n->x, n->y );
			AddNodeToList( n->children[i], n->ppparent->children, &n->ppparent->childrenMax );
			RemoveNodeFromList( n->children[i], n->children, &n->childrenMax );
		}
	}

	RemoveNodeFromList( n, windowList, &windowListMax );
	xcb_destroy_window( c, n->window );

	// if our parent is a frame or group, and it is empty, it should also be destroyed
	if ( ( n->ppparent->type == NODE_FRAME ) || ( n->ppparent->type == NODE_GROUP ) ) {
		if ( ( n->ppparent->children ) && ( n->ppparent->children[i] == NULL ) ) {
			DestroyNode( n->ppparent );
		}
	}
	free( n->children );
	free( n );
	xcb_flush( c );
}

void Cleanup( void ) {
	if ( !windowList )
		return;

	while ( windowList[0] != NULL ) {
		DestroyNode( windowList[0] );
	}

	xcb_disconnect( c );
}

void Quit( int r ) {
	Cleanup();
	exit( r );
}

void ConfigureClient( node_t *n, short x, short y, unsigned short width, unsigned short height ) {
	int nx, ny;
	unsigned short pmask = 	XCB_CONFIG_WINDOW_X |
							XCB_CONFIG_WINDOW_Y |
							XCB_CONFIG_WINDOW_WIDTH |
							XCB_CONFIG_WINDOW_HEIGHT |
							XCB_CONFIG_WINDOW_BORDER_WIDTH;

	unsigned short cmask = 	XCB_CONFIG_WINDOW_WIDTH |
							XCB_CONFIG_WINDOW_HEIGHT |
							XCB_CONFIG_WINDOW_BORDER_WIDTH;
	int i;

	if ( n == NULL ) {
		return;
	}

	nx = x;
	ny = y;

	i = ( width + nx );
	if ( i > screen->width_in_pixels ) {
		nx -= i - screen->width_in_pixels;
	}
	i = ( height + ny );
	if ( i > screen->height_in_pixels ) {
		ny -= i - screen->height_in_pixels;
	}
	if ( nx < 0 ) {
		nx = 0;
	}
	if ( ny < 0 ) {
		ny = 0;
	}
	unsigned int pv[5] = {
		nx, 
		ny, 
		width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT, 
		height + BORDER_SIZE_TOP + BORDER_SIZE_BOTTOM, 
		0
	};
	unsigned int cv[3] = {
		width, 
		height, 
		0
	};
	
	xcb_configure_window( c, n->parent, pmask, pv );
	xcb_configure_window( c, n->window, cmask, cv );
	xcb_flush( c );
}

void DrawFrame( node_t *n ) {
	int i;
	int textLen = 0, textWidth = 0, textPos = 0;
	xcb_query_text_extents_reply_t *r;
	xcb_char2b_t *s;

	if( n == NULL || n->managementState == STATE_NO_REDIRECT ) {
		return;
	}

	textLen = strnlen( n->name, 256 );
	s = malloc( textLen * sizeof( xcb_char2b_t ) );
	for( int i = 0; i < textLen; i++ ) {
		s[i].byte1 = 0;
		s[i].byte2 = n->name[i];
	}
	r = xcb_query_text_extents_reply( c, xcb_query_text_extents( c, windowFont, textLen, s ), NULL );
	textWidth = r->overall_width;
	free( r ); 
	free( s );
	textPos = ( ( n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT ) / 2 ) - ( textWidth / 2 );

	if ( n->parent == activeWindow ) {
		SGrafDrawFill( n->parent, colorLightGrey, 0, 0, n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT - 1, n->height + BORDER_SIZE_TOP + BORDER_SIZE_BOTTOM - 1 );
		SGrafDrawRect( n->parent, colorBlack, 0, 0, n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT - 1, n->height + BORDER_SIZE_TOP + BORDER_SIZE_BOTTOM - 1 );

		SGrafDrawLine( n->parent, colorBlack, 1, BORDER_SIZE_TOP - 1, n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT - 2, BORDER_SIZE_TOP - 1 );

		for ( i = 4; i < 16; i += 2 ) {
			SGrafDrawLine( n->parent, colorGrey, 2, i, n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT - 3, i );
		}

		SGrafDrawLine( n->parent, colorLightAccent, 1, 1, n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT - 2, 1 );
		SGrafDrawLine( n->parent, colorLightAccent, 1, 1, 1, BORDER_SIZE_TOP - 2 );
		SGrafDrawLine( n->parent, colorAccent, 1, BORDER_SIZE_TOP - 2, n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT - 2, BORDER_SIZE_TOP - 2 );
		SGrafDrawLine( n->parent, colorAccent, n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT - 2, 1, n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT - 2, BORDER_SIZE_TOP - 2 );

		SGrafDrawRect( n->parent, colorLightGrey, 8, 3, 12, 12 );
		SGrafDrawFill( n->parent, colorDarkAccent, 9, 4, 11, 11 );
		if ( ! ( wmState == WMSTATE_CLOSE && mouseIsOverCloseButton ) ) {
			SGrafDrawRect( n->parent, colorLightAccent, 10, 5, 9, 9 );
			SGrafDrawFill( n->parent, colorGrey, 11, 6, 7, 7 );
		}
		SGrafDrawFill( n->parent, colorLightGrey, textPos - 8, 3, textWidth + 16, 12 );
		xcb_image_text_8( c, textLen, n->parent, activeFontContext, textPos, 14, n->name );
	} else {
		SGrafDrawFill( n->parent, colorWhite, 0, 0, n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT - 1, n->height + BORDER_SIZE_TOP + BORDER_SIZE_BOTTOM - 1 );
		SGrafDrawRect( n->parent, colorDarkGrey, 0, 0, n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT - 1, n->height + BORDER_SIZE_TOP + BORDER_SIZE_BOTTOM - 1 );

		SGrafDrawLine( n->parent, colorDarkGrey, 1, BORDER_SIZE_TOP - 1, n->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT - 2, BORDER_SIZE_TOP - 1 );
		xcb_image_text_8( c, textLen, n->parent, inactiveFontContext, textPos, 14, n->name );
	}
	
	xcb_flush( c );
	return;
}

node_t* GetNodeByWindow( xcb_window_t w ) {
	int i;
	for ( i = 0 ; i < windowListMax; i++ )
		if ( ( windowList[i] == NULL ) || ( windowList[i]->window == w ) )
			return windowList[i];
	return NULL;
}

node_t *GetClientByParent( xcb_window_t w ) {
	node_t *n = firstClient;

	for ( ; n != NULL; n = n->nextNode ) {
		if ( n->parent == w ) {
			return n;
		}
	}
	return NULL;
}

void RaiseClient( node_t *n ) {
	unsigned short mask = XCB_CONFIG_WINDOW_STACK_MODE;
	unsigned int v[1] = { XCB_STACK_MODE_ABOVE };
	node_t *m = GetClientByParent( activeWindow );

	xcb_set_input_focus( c, XCB_INPUT_FOCUS_NONE, n->window, 0 );
	activeWindow = n->parent;

	DrawFrame( n );
	DrawFrame( m ); //Yes, m could be null. DrawFrame() handles this case.
	if ( n->managementState == STATE_NO_REDIRECT ) { //this might not be necessary
		xcb_configure_window( c, n->window, mask, v );
	} else {
		xcb_configure_window( c, n->parent, mask, v );
	}

	xcb_flush( c );
}

void SetupColors() {
	colorWhite = SULFUR_COLOR_WHITE;
	colorLightGrey = SGrafColor( 0xef, 0xef, 0xef );
	colorGrey = SGrafColor( 0xa5, 0xa5, 0xa5 );
	colorDarkGrey = SGrafColor( 0x73, 0x73, 0x73 );
	colorBlack = SULFUR_COLOR_BLACK;
	colorLightAccent = SGrafColor( 0xcf, 0xcf, 0xff );
	colorAccent = SGrafColor( 0xa7, 0xa7, 0xd7 );
	colorDarkAccent = SGrafColor( 0x2d, 0x2d, 0x63 );
}

void SetupAtoms() {	
	WM_DELETE_WINDOW = xcb_intern_atom_reply( c, xcb_intern_atom( c, 0, strlen( "WM_DELETE_WINDOW" ), "WM_DELETE_WINDOW" ), NULL )->atom;
	WM_PROTOCOLS = xcb_intern_atom_reply( c, xcb_intern_atom( c, 0, strlen( "WM_PROTOCOLS" ), "WM_PROTOCOLS" ), NULL )->atom;
}

void SetupFonts() {
	unsigned int v[3];

	windowFont = xcb_generate_id( c );
	xcb_open_font( c, windowFont, strnlen( FONT_NAME, 256 ), FONT_NAME );

	v[2] = windowFont;

	activeFontContext = xcb_generate_id( c );
	v[0] = colorBlack;
	v[1] = colorLightGrey;
	xcb_create_gc( c, activeFontContext, screen->root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT, v );

	inactiveFontContext = xcb_generate_id( c );
	v[0] = colorDarkGrey;
	v[1] = colorWhite;
	xcb_create_gc( c, inactiveFontContext, screen->root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT, v );
}

int BecomeWM(  ) {
	unsigned int v[1];
	xcb_void_cookie_t cookie;
	xcb_generic_error_t *error;

	v[0] = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;
	cookie = xcb_change_window_attributes_checked( c, screen->root, XCB_CW_EVENT_MASK, v );
	error = xcb_request_check( c, cookie );
	if ( error ) {
		free( error );
 		return -1;
	}
	return 0;
} 

void SetRootBackground() {
	int w = screen->width_in_pixels, h = screen->height_in_pixels;
	xcb_pixmap_t fill = xcb_generate_id( c );
	xcb_pixmap_t pixmap = xcb_generate_id( c );
	unsigned int v[1] = { pixmap };

	xcb_create_pixmap( c, screen->root_depth, fill, screen->root, 2, 2 );
	xcb_create_pixmap( c, screen->root_depth, pixmap, screen->root, w, h );

	SGrafDrawFill( pixmap, colorGrey, 0, 0, w, h );

	// draw rounded corners
	SGrafDrawLine( pixmap, colorBlack, 0, 0, 0, 4 );
	SGrafDrawLine( pixmap, colorBlack, 1, 0, 4, 0 );
	SGrafDrawLine( pixmap, colorBlack, 1, 1, 1, 2 );
	SGrafDrawLine( pixmap, colorBlack, 1, 1, 2, 1 );

	SGrafDrawLine( pixmap, colorBlack, w - 1, 0, w - 5, 0 );
	SGrafDrawLine( pixmap, colorBlack, w - 1, 0, w - 1, 4 );
	SGrafDrawLine( pixmap, colorBlack, w - 2, 1, w - 2, 2 );
	SGrafDrawLine( pixmap, colorBlack, w - 2, 1, w - 3, 1 );

	SGrafDrawLine( pixmap, colorBlack, 0, h - 1, 0, h - 5 );
	SGrafDrawLine( pixmap, colorBlack, 1, h - 1, 4, h - 1 );
	SGrafDrawLine( pixmap, colorBlack, 1, h - 2, 1, h - 3 );
	SGrafDrawLine( pixmap, colorBlack, 1, h - 2, 2, h - 2 );

	SGrafDrawLine( pixmap, colorBlack, w - 1, h - 1, w - 5, h - 1 );
	SGrafDrawLine( pixmap, colorBlack, w - 1, h - 1, w - 1, h - 5 );
	SGrafDrawLine( pixmap, colorBlack, w - 2, h - 2, w - 2, h - 3 );
	SGrafDrawLine( pixmap, colorBlack, w - 2, h - 2, w - 3, h - 2 );
	
	xcb_change_window_attributes( c, screen->root, XCB_CW_BACK_PIXMAP, v );
	xcb_clear_area( c, 1, screen->root, 0, 0, w, h );

	xcb_flush( c );
}

void SetupRoot() {
	rootNode = calloc( 1, sizeof( node_t ) );

	rootNode->type = NODE_ROOT;
	rootNode->window = screen->root;

	SetRootBackground();
	AddNodeToList(rootNode, windowList, &windowListMax );
}

void ReparentWindow( xcb_window_t win, xcb_window_t parent, short x, short y, unsigned short width, unsigned short height, unsigned char override_redirect ) {
	unsigned int v[2] = { 	colorWhite, 
							XCB_EVENT_MASK_EXPOSURE | 
							XCB_EVENT_MASK_BUTTON_PRESS | 
							XCB_EVENT_MASK_BUTTON_RELEASE | 
							XCB_EVENT_MASK_POINTER_MOTION | 
							XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | 
							XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT };
	node_t *m = firstClient;
	node_t *n = firstClient;

	node_t* newClient = malloc( sizeof( node_t ) );


	if ( n == NULL ) {
		firstClient = n = newClient;
		n->nextNode = NULL;
	} else {
		while ( n != NULL ) {
			if( win == n->window || win == n->parent ) {
					printf( "New frame\n" );
				return;
			}
			m = n;
			n = m->nextNode;
		}
		m->nextNode = n = newClient;
		n->nextNode = NULL;
	}

	v[0] = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS | 
						XCB_EVENT_MASK_BUTTON_RELEASE | 
						XCB_EVENT_MASK_POINTER_MOTION;
	xcb_change_window_attributes( c, newClient->window, XCB_CW_EVENT_MASK, v );

	newClient->window = win;
	newClient->width = width;
	newClient->height = height;
	newClient->x = x;
	newClient->y = y;
	newClient->parentMapped = 0;
	newClient->ppparent = NULL;

	strncpy( newClient->name, "", 256 );

	newClient->managementState = STATE_WITHDRAWN;

	if ( ( override_redirect ) || ( parent != screen->root ) ) {
		newClient->managementState = STATE_NO_REDIRECT;
		newClient->window = win;
		printf( "New unreparented window\n" );
	} else {
		newClient->parent = xcb_generate_id( c );
		xcb_create_window (		c, XCB_COPY_FROM_PARENT, newClient->parent, screen->root, 
								0, 0, BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT + 1, BORDER_SIZE_TOP + BORDER_SIZE_BOTTOM + 1, 
								0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 
								XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, v);
		xcb_reparent_window( c, newClient->window, newClient->parent, 1, 19 );
	}

	AddNodeToList( newClient, windowList, &windowListMax );

	xcb_flush( c );
}

void ReparentExistingWindows() {
	xcb_query_tree_cookie_t treecookie;
	xcb_query_tree_reply_t *treereply;
	xcb_get_geometry_cookie_t geocookie;
	xcb_get_geometry_reply_t *georeply;
	int i;
	xcb_window_t *children;

	treecookie = xcb_query_tree( c, screen->root );
	treereply = xcb_query_tree_reply( c, treecookie, NULL );
	if ( treereply == NULL ) {
		return;
	}
	children = xcb_query_tree_children( treereply );
	for( i = 0; i < xcb_query_tree_children_length( treereply ); i++ ) {
		geocookie = xcb_get_geometry( c, children[i] );
		georeply = xcb_get_geometry_reply( c, geocookie, NULL );
		if ( georeply != NULL ) {
			ReparentWindow( children[i], screen->root, georeply->x, georeply->y, georeply->width, georeply->height, 0 );
			free( georeply );
		}
	}
	free( treereply );
}

/*
==============
Event handlers
==============
*/

void DoButtonPress( xcb_button_press_event_t *e ) {
	node_t *n = GetClientByParent( e->event );

	printf( "button press on window %x\n", e->event );
	
	if ( n == NULL ) {
		n = GetNodeByWindow( e->event );
		printf( "is client window\n" );
		if ( n == NULL ) {
			return;
		}	
		RaiseClient( n );
		return;
	}
	if ( n->parent == activeWindow ) {
		// (9,4) (20,15)
		if ( mouseIsOverCloseButton ) {
			wmState = WMSTATE_CLOSE;
			printf("Close clicked!\n");
			DrawFrame( n );
			return;
		}
	}
	RaiseClient( n );
	dragClient = n;
	wmState = WMSTATE_DRAG;
	dragStartX = e->event_x;
	dragStartY = e->event_y;
}

void DoButtonRelease( xcb_button_release_event_t *e ) {
	switch ( wmState ) {
		case WMSTATE_IDLE:
			break;
		case WMSTATE_DRAG:
			wmState = WMSTATE_IDLE;
			dragClient = NULL;
			dragStartX = 0;
			dragStartY = 0;
			break;
		case WMSTATE_CLOSE:
			if ( mouseIsOverCloseButton == 1 ) {
				printf( "Close window now!\n" );

				xcb_client_message_event_t *msg = calloc(32, 1);
				msg->response_type = XCB_CLIENT_MESSAGE;
				msg->window = GetClientByParent( activeWindow )->window;
				msg->format = 32;
				msg->sequence = 0;
				msg->type = WM_PROTOCOLS;
				msg->data.data32[0] = WM_DELETE_WINDOW;
				msg->data.data32[1] = XCB_CURRENT_TIME;
				xcb_send_event( c, 0, GetClientByParent( activeWindow )->window, XCB_EVENT_MASK_NO_EVENT, (char*)msg );
				
				xcb_flush( c );
				free( msg );
			}
			wmState = WMSTATE_IDLE;
			DrawFrame( GetClientByParent( activeWindow ) );
			break;
		default:
			wmState = WMSTATE_IDLE;
			break;
	}
}

void DoMotionNotify( xcb_motion_notify_event_t *e ) {
	int x;
	int y;

	mouseLastKnownX = e->root_x;
	mouseLastKnownY = e->root_y;

	mouseIsOverCloseButton = 0;
	if ( e->event_x >= 9 && e->event_x <= 20 ) {
		if ( e->event_y >= 4 && e->event_y <= 15 ) {
			mouseIsOverCloseButton = 1;
		}
	}

	if ( wmState == WMSTATE_CLOSE ) {
		DrawFrame( GetClientByParent( activeWindow ) );
	}

	if ( wmState == WMSTATE_DRAG ) {
		x = e->root_x - dragStartX;
		y =  e->root_y - dragStartY;
		ConfigureClient( dragClient, x, y, dragClient->width, dragClient->height );
	}
}

void DoExpose( xcb_expose_event_t *e ) {
	DrawFrame( GetClientByParent( e->window ) );
	SetRootBackground();
}

void DoCreateNotify( xcb_create_notify_event_t *e ) {
	/*if ( e->parent != screen->root ) {
		return;
	}*/
	ReparentWindow( e->window, e->parent, e->x, e->y, e->width, e->height, e->override_redirect );
}

void DoDestroy( xcb_destroy_notify_event_t *e ) {
	node_t *n = firstClient;
	node_t *m = firstClient;

	node_t* node = GetNodeByWindow( e->window );
	if ( node ) {
		RemoveNodeFromList( node, windowList, &windowListMax );
		if ( node->ppparent != NULL )
			RemoveNodeFromList( node, node->ppparent->children, &node->ppparent->childrenMax );
	} else {
		fprintf(stderr, "warning: window removed that was not in window list\n");
	}

	for ( ; n != NULL; m = n, n = n->nextNode ) {
		if ( n->window == e->window ) {
			m->nextNode = n->nextNode;
			if ( activeWindow == n->parent ) {
				activeWindow = 0;
			}
			xcb_destroy_window( c, n->parent );
			if( firstClient == n ) {
				firstClient = n->nextNode;
			}
			if( dragClient == n ) {
				dragClient = NULL;
			}
			free( n );
			n = NULL;
			xcb_flush( c );
			return;
		}
	}
}

void DoMapRequest( xcb_map_request_event_t *e ) {
	node_t *n = GetNodeByWindow( e->window );

	/* Todo:
		Add ICCCM section 4.1.4 compatibility
		https://tronche.com/gui/x/icccm/sec-4.html#s-4.1.3
	*/
	if ( n == NULL ) {
		return;
	}
	n->windowState = STATE_NORMAL;
	xcb_map_window( c, n->parent );
	xcb_map_window( c, n->window );
	xcb_flush( c );
	RaiseClient( n );
	printf( "window %x mapped\n", e->window );
}

//This is all a hack to make ReparentExistingWindows work.
//It relies on the fact that X will unmap then remap windows on reparent,
//if and only if they are already mapped
void DoMapNotify( xcb_map_notify_event_t *e ) {
	node_t *n = GetNodeByWindow( e->window );

	if ( n == NULL ) {
		return;
	}
	if ( n->parentMapped == 0 ) {  
		n->windowState = STATE_NORMAL;
		n->parentMapped = 1;
		xcb_map_window( c, n->parent );
		xcb_flush( c );
		RaiseClient( n );
	}
}

void DoUnmapNotify( xcb_unmap_notify_event_t *e ) {
	node_t *n = GetNodeByWindow( e->window );

	if ( n == NULL ) {
		return;
	}
	if ( n->parentMapped == 1 ) {
		n->windowState = STATE_WITHDRAWN;
		n->parentMapped = 0;
		xcb_unmap_window( c, n->parent );
		xcb_flush( c );
	}
}

void DoReparentNotify( xcb_reparent_notify_event_t *e ) {
	node_t *n = GetNodeByWindow( e->window );

	if ( n == NULL ) {
		return;
	}

	n->managementState = STATE_REPARENTED;
	ConfigureClient( n, n->x, n->y, n->width, n->height );
	printf( "window %x reparented to window %x\n", e->window, e->parent );
	return;
}

void DoConfigureRequest( xcb_configure_request_event_t *e ) {
	ConfigureClient( GetNodeByWindow( e->window ), e->x, e->y, e->width, e->height );
}

void DoConfigureNotify( xcb_configure_notify_event_t *e ) {
	node_t *n = GetNodeByWindow( e->window );
	
	if ( n == NULL ) {
		node_t *n = GetClientByParent( e->window );
		if ( n == NULL ) {
			return;
		}
		n->x = e->x;
		n->y = e->y;
		n->width = e->width - ( BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT );
		n->height = e->height - ( BORDER_SIZE_TOP + BORDER_SIZE_BOTTOM );
		return;
	}
	n->x = e->x;
	n->y = e->y;
	n->width = e->width;
	n->height = e->height;
}

void DoPropertyNotify( xcb_property_notify_event_t *e ) {
	xcb_get_property_cookie_t cookie;
    xcb_get_property_reply_t *reply;
	node_t *n = GetNodeByWindow( e->window );

	if ( n == NULL ) {
		return;
	}

	if ( e->atom == XCB_ATOM_WM_NAME ) {
		cookie = xcb_get_property( c, 0, e->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 256 );
		if ((reply = xcb_get_property_reply(c, cookie, NULL))) {
			int len = xcb_get_property_value_length(reply);
			if ( len != 0 ) {
				memset( n->name, '\0', 256 );
				strncpy( n->name, (char*)xcb_get_property_value( reply ), len > 255 ? 255 : len );
				DrawFrame( n );
			}
		}	
		free( reply );
	} else {
		printf( "window %x updated unknown atom %s\n", e->window, xcb_get_atom_name_name( xcb_get_atom_name_reply( c, xcb_get_atom_name( c, e->atom ), NULL ) ) );
	}
}

void DoClientMessage( xcb_client_message_event_t *e ) {
	int i;
	xcb_get_atom_name_cookie_t nameCookie;
	xcb_get_atom_name_reply_t* nameReply;

	nameCookie = xcb_get_atom_name( c, e->type );
	nameReply = xcb_get_atom_name_reply( c, nameCookie, NULL );

	printf( "received client message\n" );
	printf( "format: %i\n", e->format );
	printf( "type: %s\n", xcb_get_atom_name_name( nameReply ) );
	if ( !strcmp( xcb_get_atom_name_name( nameReply ), "_NET_WM_STATE" ) ) {
		printf( "message is _NET_WM_STATE\n" );
		for ( i = 0; i < 3; i++ ) {
			if ( e->data.data32[i] == 0 )
				break;
			nameCookie = xcb_get_atom_name( c, e->data.data32[i] );
			nameReply = xcb_get_atom_name_reply( c, nameCookie, NULL );
			printf( "data[i]: %s\n", xcb_get_atom_name_name( nameReply ) );
		}
	}
}

/*
=============
Main function
=============
*/

int main( int argc, char** argv ) {
	signal( SIGTERM, Quit );
	signal( SIGINT, Quit );

	printf( "%s %s, build %s\n\n", PROGRAM_NAME, VERSION_STRING, VERSION_BUILDSTR );

	if ( SulfurInit( NULL ) != 0 ) {
			printf( "Problem starting up. Is X running?\n" );
			Cleanup();
			return 1;
	}
	c = sulfurGetXcbConn();
	screen = sulfurGetXcbScreen();
	if ( BecomeWM() < 0 ) {
		printf( "it looks like another wm is running.\n" );
		printf( "you will need to close it before you can run makron.\n" );
		Cleanup();
		return 1;
	}

	/* initialize the client list to empty */
	windowList = calloc( sizeof ( node_t* ), windowListMax );
	windowList[0] = NULL;

	SetupAtoms();
	SetupColors();
	SetupFonts();
	SetupRoot();
	ReparentExistingWindows();

	while( !xcb_connection_has_error( c ) ) {
		while( ( e = xcb_poll_for_event( c ) ) != NULL ) {
			switch( e->response_type & ~0x80 ) {
				case XCB_BUTTON_PRESS: 		DoButtonPress( (xcb_button_press_event_t *)e ); break;
				case XCB_BUTTON_RELEASE: 	DoButtonRelease( (xcb_button_release_event_t *)e ); break;
				case XCB_MOTION_NOTIFY: 	DoMotionNotify( (xcb_motion_notify_event_t *)e ); break;
				case XCB_EXPOSE: 			DoExpose( (xcb_expose_event_t *)e ); break;
				case XCB_CREATE_NOTIFY:  	DoCreateNotify( (xcb_create_notify_event_t *)e ); break;
				case XCB_DESTROY_NOTIFY: 	DoDestroy( (xcb_destroy_notify_event_t *)e ); break;
				case XCB_MAP_NOTIFY: 		DoMapNotify( (xcb_map_notify_event_t *)e ); break;
				case XCB_MAP_REQUEST: 		DoMapRequest( (xcb_map_request_event_t *)e ); break;
				case XCB_UNMAP_NOTIFY: 		DoUnmapNotify( (xcb_unmap_notify_event_t *)e ); break;
				case XCB_REPARENT_NOTIFY: 	DoReparentNotify( (xcb_reparent_notify_event_t *)e ); break;
				case XCB_CONFIGURE_NOTIFY: 	DoConfigureNotify( (xcb_configure_notify_event_t *)e ); break;
				case XCB_CONFIGURE_REQUEST: DoConfigureRequest( (xcb_configure_request_event_t *)e ); break;
				case XCB_PROPERTY_NOTIFY: 	DoPropertyNotify( (xcb_property_notify_event_t *)e ); break;
				case XCB_CLIENT_MESSAGE: 	DoClientMessage( (xcb_client_message_event_t *)e ); break;
				default: 					dprintf( 1, "warning, unhandled event #%d\n", e->response_type & ~0x80 ); break;
			}
			free( e );
		}
		nanosleep( (const struct timespec[]){{0, 4166666L}}, NULL );
	}
	printf( "connection closed. goodbye!\n" );
	Cleanup();
	return 0;
}
