#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <pwd.h>

#include <sulfur/sulfur.h>

#include <iniparser/iniparser.h>

#include "m_common.h"

node_t *rootNode = NULL;
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
unsigned int cursorContext;

xcb_font_t windowFont;
xcb_font_t cursorFont;
xcb_cursor_t cursor;
int lastCursor;

xcb_atom_t WM_DELETE_WINDOW;
xcb_atom_t WM_PROTOCOLS;

typedef enum {
	RESIZE_NONE = 0,
	RESIZE_HORIZONTAL = 1,
	RESIZE_VERTICAL = 2,
} resizeDir_t;

wmState_t wmState = WMSTATE_IDLE;
node_t *dragClient;
bool dragChanged = false;
short dragStartX, dragStartY;
short dragNewX, dragNewY, dragNewW, dragNewH;
short mouseLastKnownX;
short mouseLastKnownY;
short mouseIsOverCloseButton;
resizeDir_t resizeDir;

nodeList_t windowList; // list of all windows, in most recently raised order
nodeList_t redrawList; // list of all windows needing redrawn

char *homedir;
dictionary *dict;

int spawnx = 40, spawny = 40, spawnxdir = 20, spawnydir = 20;

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
	if ( level <= debugLevel ) {
		vprintf( fmt, args );
	}
	va_end( args );
}

void AddNodeToList( node_t* n, nodeList_t* list ) {
	int i;

	for ( i = 0 ; i < list->max; i++ ) {
		if ( list->nodes[i] == NULL || list->nodes[i] == n ) {
			list->nodes[i] = n;
			return;
		}
	}
	dbgprintf( 2, "growing client list (current max is %i\n)\n", list->max );
	list->nodes = realloc( list->nodes, sizeof ( node_t ) * ( list->max += 4 ) );
	if ( list->nodes == NULL ) {
		fprintf( stderr, "failure growing window list\n" );
		Quit( 2 );
	}
	for ( i = list->max - 4; i < list->max; i++ ) {
		list->nodes[i] = NULL;
	}
	dbgprintf( 2, "window list size is %i\n", list->max );
	AddNodeToList( n, list );
}

void RemoveNodeFromList( node_t* n, nodeList_t* list ) {
	int i;

	if ( !list || !n )
		return;

	for ( i = 0 ; i < list->max; i++ ) {
		if ( list->nodes[i] == n ) {
			break;
		}
	}
	if ( i >= list->max ) {
		dbgprintf( 1, "node not found\n" );
		return;
	}
	for ( i++ ; i < list->max; i++ ) {
		list->nodes[i - 1] = list->nodes[i];
		if ( list->nodes[i] == NULL ) {
			break;
		}
	}
	if ( i >= list->max )
		list->nodes[list->max - 1] = NULL;
	if ( i < list->max - 4 ) {
		dbgprintf( 2, "shrinking client list\n" );
		list->nodes = realloc( list->nodes, sizeof( node_t ) * ( list->max -= 4 ) );
		if ( list == NULL ) {
			fprintf( stderr, "failure shrinking client list\n" );
			Quit( 2 );
		}
		dbgprintf( 2, "client list size is %i\n", list->max );
	}
}

node_t* GetParentFrame( node_t* n ) {
	node_t* p;
	for ( p = n; ( p != NULL ) && ( p->type != NODE_FRAME ); p = p->parent )
		;;
	return p;
}

node_t* CreateNode( nodeType_t type, xcb_window_t wnd, node_t* parent, short width, short height, short x, short y ) {
	node_t* n = calloc( 1, sizeof( node_t ) );
	if ( !n )
		return NULL;
	n->children.max = 4;
	n->children.nodes = calloc( n->children.max, sizeof( node_t*) );
	if ( !n->children.nodes ) {
		free(n);
		return NULL;
	}

	strncpy( n->name, "untitled", 256 );
	n->managementState = STATE_INIT;
	n->windowState = STATE_WITHDRAWN;
	n->type = type;
	n->window = wnd;
	n->parent = parent;
	n->width = width;
	n->height = height;
	n->x = x;
	n->y = y;
	return n;
}

void DestroyNode( node_t* n ) {
	int i = 0;

	if ( !n || n->type == NODE_ROOT )
		return;

	// reparent any child windows
	if ( n->children.nodes && n->parent && n->parent->children.nodes ) {
		for ( i = 0; ( i < n->children.max ) && ( n->children.nodes[i] != NULL ); i++ ) {
			xcb_reparent_window( c, n->window, n->parent->window, n->x, n->y );
			AddNodeToList( n->children.nodes[i], &n->parent->children );
			RemoveNodeFromList( n->children.nodes[i], &n->children );
		}
	}

	RemoveNodeFromList( n, &windowList );
	RemoveNodeFromList( n, &redrawList );
	RemoveNodeFromList( n, &n->parent->children );
	xcb_destroy_window( c, n->window );

	// if our parent is a frame or group, and it is empty, it should also be destroyed
	if ( ( n->parent->type == NODE_FRAME ) || ( n->parent->type == NODE_GROUP ) ) {
		if ( ( n->parent->children.nodes ) && ( n->parent->children.nodes[0] == NULL ) ) {
			DestroyNode( n->parent );
		}
	}
	free( n->children.nodes );
	free( n );
}

void Cleanup( void ) {
	if ( !windowList.nodes )
		return;

	while ( windowList.nodes[0] != NULL && windowList.nodes[0] != rootNode ) {
		DestroyNode( windowList.nodes[0] );
	}
	free( windowList.nodes[0]->children.nodes );
	free( windowList.nodes[0] );
	free( windowList.nodes );

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
	node_t *p = GetParentFrame( n );

	if ( n == NULL || n->type == NODE_FRAME )
		return;

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
		width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT + 1, 
		height + BORDER_SIZE_TOP + BORDER_SIZE_BOTTOM + 1, 
		0
	};
	unsigned int cv[3] = {
		width,
		height,
		0
	};
	p->x = nx;
	p->y = ny;
	p->width = pv[2];
	p->height = pv[3];
	n->width = cv[0];
	n->height = cv[1];
	if ( p != NULL && n->parent == p )
		xcb_configure_window( c, p->window, pmask, pv );
	xcb_configure_window( c, n->window, cmask, cv );
}

void DrawFrame( node_t *node ) {
	int i, textLen = 0, textWidth = 0, textPos = 0;
	node_t* frame,* child;

	if( node == NULL || node->managementState == STATE_NO_REDIRECT ) {
		return;
	}

	frame = GetParentFrame( node );
	if ( !frame )
		return;
	child = frame->children.nodes[0];
	if ( !child )
		child = frame;

	textLen = strnlen( child->name, 256 );
	textWidth = textLen * 6;
	textPos = ( ( frame->width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT ) / 2 ) - ( textWidth / 2 );

	if ( frame->children.nodes[0] == windowList.nodes[0] ) {
		SGrafDrawFill( frame->window, colorLightGrey, 0, 0, frame->width - 1, frame->height - 1 );
		SGrafDrawRect( frame->window, colorBlack, 0, 0, frame->width - 1, frame->height - 1 );

		SGrafDrawLine( frame->window, colorBlack, 1, BORDER_SIZE_TOP - 1, frame->width - 2, BORDER_SIZE_TOP - 1 );

		for ( i = 4; i < 16; i += 2 ) {
			SGrafDrawLine( frame->window, colorGrey, 2, i, frame->width - 3, i );
		}

		SGrafDrawLine( frame->window, colorLightAccent, 1, 1, frame->width - 2, 1 );
		SGrafDrawLine( frame->window, colorLightAccent, 1, 1, 1, BORDER_SIZE_TOP - 2 );
		SGrafDrawLine( frame->window, colorAccent, 1, BORDER_SIZE_TOP - 2, frame->width - 2, BORDER_SIZE_TOP - 2 );
		SGrafDrawLine( frame->window, colorAccent, frame->width - 2, 1, frame->width - 2, BORDER_SIZE_TOP - 2 );

		SGrafDrawRect( frame->window, colorLightGrey, 8, 3, 12, 12 );
		SGrafDrawFill( frame->window, colorDarkAccent, 9, 4, 11, 11 );
		if ( ! ( wmState == WMSTATE_CLOSE && mouseIsOverCloseButton ) ) {
			SGrafDrawRect( frame->window, colorLightAccent, 10, 5, 9, 9 );
			SGrafDrawFill( frame->window, colorGrey, 11, 6, 7, 7 );
		}
		SGrafDrawFill( frame->window, colorLightGrey, textPos - 8, 3, textWidth + 16, 12 );
		xcb_image_text_8( c, textLen, frame->window, activeFontContext, textPos, 14, child->name );
	} else {
		SGrafDrawFill( frame->window, colorWhite, 0, 0, frame->width - 1, frame->height - 1 );
		SGrafDrawRect( frame->window, colorDarkGrey, 0, 0, frame->width - 1, frame->height - 1 );

		SGrafDrawLine( frame->window, colorDarkGrey, 1, BORDER_SIZE_TOP - 1, frame->width - 1, BORDER_SIZE_TOP - 1 );
		xcb_image_text_8( c, textLen, frame->window, inactiveFontContext, textPos, 14, child->name );
	}
	return;
}

node_t* GetNodeByWindow( xcb_window_t w ) {
	int i;
	for ( i = 0 ; i < windowList.max; i++ )
		if ( ( windowList.nodes[i] == NULL ) || ( windowList.nodes[i]->window == w ) )
			return windowList.nodes[i];
	return NULL;
}

void RaiseClient( node_t *n ) {
	unsigned short mask = XCB_CONFIG_WINDOW_STACK_MODE;
	unsigned int v[1] = { XCB_STACK_MODE_ABOVE };
	node_t* p = GetParentFrame( n );
	node_t* old = GetParentFrame( windowList.nodes[0] );
	int i;
	
	if ( n == p )
		n = p->children.nodes[0];

	if ( !n )
		return;

	for ( i = 0; ( i < windowList.max ) && ( windowList.nodes[i] != NULL ) && ( windowList.nodes[i] != n ); i++ ) ;;
	if ( i >= windowList.max || windowList.nodes[i] == NULL ) {
		return;
	}

	for ( ; i >= 1 ; i-- )
		windowList.nodes[i] = windowList.nodes[i - 1];

	windowList.nodes[0] = n;

	xcb_set_input_focus( c, XCB_INPUT_FOCUS_POINTER_ROOT, n->window, XCB_CURRENT_TIME );

	AddNodeToList( p, &redrawList );
	AddNodeToList( old, &redrawList );
	if ( p )
		xcb_configure_window( c, p->window, mask, v );
	xcb_configure_window( c, n->window, mask, v );
}

sulfurColor_t GetDarkColor( const char* name ) {
	if ( !strncmp( name, "bluebell", 16 ) ) {
		return SGrafColor( 0xcc * 45 / 100, 0xcc * 45 / 100, 0xff * 45 / 100 );
	} else if ( !strncmp( name, "gold", 16 ) ) {
		return SGrafColor( 0xff * 45 / 100, 0xcc * 45 / 100, 0x99 * 45 / 100 );
	} else if ( !strncmp( name, "green", 16 ) ) {
		return SGrafColor( 0x99 * 45 / 100, 0xcc * 45 / 100, 0x99 * 45 / 100 );
	} else if ( !strncmp( name, "turquoise", 16 ) ) {
		return SGrafColor( 0x99 * 45 / 100, 0xff * 45 / 100, 0xff * 45 / 100 );
	} else if ( !strncmp( name, "red", 16 ) ) {
		return SGrafColor( 0xff * 45 / 100, 0xcc * 45 / 100, 0xcc * 45 / 100 );
	} else if ( !strncmp( name, "pink", 16 ) ) {
		return SGrafColor( 0xff * 45 / 100, 0xcc * 45 / 100, 0xff * 45 / 100 );
	} else if ( !strncmp( name, "blue", 16 ) ) {
		return SGrafColor( 0x99 * 45 / 100, 0xcc * 45 / 100, 0xff * 45 / 100 );
	} else if ( !strncmp( name, "grey", 16 ) || !strncmp( name, "gray", 16 ) ) {
		return SGrafColor( 0xdd * 45 / 100, 0xdd * 45 / 100, 0xdd * 45 / 100 );
	} else {
		return SGrafColor( 0xcc * 45 / 100, 0xcc * 45 / 100, 0xff * 45 / 100 );
	}
}

sulfurColor_t GetColor( const char* name ) {
	if ( !strncmp( name, "bluebell", 16 ) ) {
		return SGrafColor( 0xcc * 85 / 100, 0xcc * 85 / 100, 0xff * 85 / 100 );
	} else if ( !strncmp( name, "gold", 16 ) ) {
		return SGrafColor( 0xff * 85 / 100, 0xcc * 85 / 100, 0x99 * 85 / 100 );
	} else if ( !strncmp( name, "green", 16 ) ) {
		return SGrafColor( 0x99 * 85 / 100, 0xcc * 85 / 100, 0x99 * 85 / 100 );
	} else if ( !strncmp( name, "turquoise", 16 ) ) {
		return SGrafColor( 0x99 * 85 / 100, 0xff * 85 / 100, 0xff * 85 / 100 );
	} else if ( !strncmp( name, "red", 16 ) ) {
		return SGrafColor( 0xff * 85 / 100, 0xcc * 85 / 100, 0xcc * 85 / 100 );
	} else if ( !strncmp( name, "pink", 16 ) ) {
		return SGrafColor( 0xff * 85 / 100, 0xcc * 85 / 100, 0xff * 85 / 100 );
	} else if ( !strncmp( name, "blue", 16 ) ) {
		return SGrafColor( 0x99 * 85 / 100, 0xcc * 85 / 100, 0xff * 85 / 100 );
	} else if ( !strncmp( name, "grey", 16 ) || !strncmp( name, "gray", 16 ) ) {
		return SGrafColor( 0xdd * 85 / 100, 0xdd * 85 / 100, 0xdd * 85 / 100 );
	} else {
		return SGrafColor( 0xcc * 85 / 100, 0xcc * 85 / 100, 0xff * 85 / 100 );
	}
}

sulfurColor_t GetLightColor( const char* name ) {
	if ( !strncmp( name, "bluebell", 16 ) ) {
		return SGrafColor( 0xcc, 0xcc, 0xff );
	} else if ( !strncmp( name, "gold", 16 ) ) {
		return SGrafColor( 0xff, 0xcc, 0x99 );
	} else if ( !strncmp( name, "green", 16 ) ) {
		return SGrafColor( 0x99, 0xcc, 0x99 );
	} else if ( !strncmp( name, "turquoise", 16 ) ) {
		return SGrafColor( 0x99, 0xff, 0xff );
	} else if ( !strncmp( name, "red", 16 ) ) {
		return SGrafColor( 0xff, 0xcc, 0xcc );
	} else if ( !strncmp( name, "pink", 16 ) ) {
		return SGrafColor( 0xff, 0xcc, 0xff );
	} else if ( !strncmp( name, "blue", 16 ) ) {
		return SGrafColor( 0x99, 0xcc, 0xff );
	} else if ( !strncmp( name, "grey", 16 ) || !strncmp( name, "gray", 16 ) ) {
		return SGrafColor( 0xdd, 0xdd, 0xdd );
	} else {
		return SGrafColor( 0xcc, 0xcc, 0xff );
	}
}

void SetupColors() {
	const char* accent = iniparser_getstring( dict, "colors:accent", "bluebell" );
	dbgprintf( 2, "accent color is %s\n", accent );
	colorWhite = SULFUR_COLOR_WHITE;
	colorLightGrey = SGrafColor( 0xef, 0xef, 0xef );
	colorGrey = SGrafColor( 0xa5, 0xa5, 0xa5 );
	colorDarkGrey = SGrafColor( 0x73, 0x73, 0x73 );
	colorBlack = SULFUR_COLOR_BLACK;
	colorLightAccent = GetLightColor( accent );
	colorAccent = GetColor( accent );
	colorDarkAccent = GetDarkColor( accent );
}

void SetupAtoms() {	
	WM_DELETE_WINDOW = xcb_intern_atom_reply( c, xcb_intern_atom( c, 0, strlen( "WM_DELETE_WINDOW" ), "WM_DELETE_WINDOW" ), NULL )->atom;
	WM_PROTOCOLS = xcb_intern_atom_reply( c, xcb_intern_atom( c, 0, strlen( "WM_PROTOCOLS" ), "WM_PROTOCOLS" ), NULL )->atom;
}

void SetupFontGc( xcb_gc_t* ctx, sulfurColor_t fg, sulfurColor_t bg, xcb_font_t font ) {
	unsigned int v[3] = { fg, bg, font };
	*ctx = xcb_generate_id( c );
	xcb_create_gc( c, *ctx, screen->root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, v );
}

void SetupFonts() {
	windowFont = xcb_generate_id( c );
	xcb_open_font( c, windowFont, strnlen( FONT_NAME, 256 ), FONT_NAME );

	cursorFont = xcb_generate_id( c );
	xcb_open_font( c, cursorFont, strlen( "cursor" ), "cursor" );

	SetupFontGc( &activeFontContext, colorBlack, colorLightGrey, windowFont );
	SetupFontGc( &inactiveFontContext, colorDarkGrey, colorWhite, windowFont );
	SetupFontGc( &cursorContext, colorBlack, colorWhite, cursorFont );
}

void SetCursor( int cur ) {
	if ( cur != lastCursor ) {
		cursor = xcb_generate_id( c );
		xcb_create_glyph_cursor ( c, cursor, cursorFont, cursorFont, cur, cur + 1, 0, 0, 0, 65535, 65535, 65535);
		xcb_change_window_attributes ( c, rootNode->window, XCB_CW_CURSOR, &cursor );
		xcb_free_cursor( c, cursor );
		cur = lastCursor;
	}
}

int BecomeWM( void ) {
	unsigned int v[1];
	xcb_void_cookie_t cookie;
	xcb_generic_error_t *error;

	v[0] = XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;
	cookie = xcb_change_window_attributes_checked( c, screen->root, XCB_CW_EVENT_MASK, v );
	error = xcb_request_check( c, cookie );
	if ( error ) {
		free( error );
 		return -1;
	}
	return 0;
} 

void SetRootBackground() {
	return;
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
	xcb_free_pixmap( c, pixmap );
	xcb_free_pixmap( c, fill );
}

void SetupRoot() {
	rootNode = CreateNode( NODE_ROOT, screen->root, NULL, screen->width_in_pixels, screen->height_in_pixels, 0, 0 );
	AddNodeToList(rootNode, &windowList );
	SetRootBackground();
}

void ReparentWindow( xcb_window_t win, xcb_window_t parent, short x, short y, unsigned short width, unsigned short height, unsigned char override_redirect ) {
	node_t* n;
	node_t* p;
	unsigned int v[2] = { 	colorWhite, 
							XCB_EVENT_MASK_EXPOSURE | 
							XCB_EVENT_MASK_BUTTON_PRESS | 
							XCB_EVENT_MASK_BUTTON_RELEASE | 
							XCB_EVENT_MASK_POINTER_MOTION | 
							XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | 
							XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT };

	if ( GetNodeByWindow( win ) != NULL )
		return;
	p = GetNodeByWindow( parent );
	if ( !p )
		p = rootNode;
	n = CreateNode( NODE_CLIENT, win, p, width, height, x, y );

	n->managementState = STATE_WITHDRAWN;

	if ( p == rootNode && !override_redirect ) {
		// create a new window frame and reparent the client to it
		xcb_window_t frame = xcb_generate_id( c );
		int frameWidth = width + BORDER_SIZE_LEFT + BORDER_SIZE_RIGHT + 1;
		int frameHeight = height + BORDER_SIZE_TOP + BORDER_SIZE_BOTTOM + 1;
		xcb_create_window (		c, XCB_COPY_FROM_PARENT, frame, screen->root, 
						0, 0, frameWidth, frameHeight, 
						0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 
						XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, v);
		p = CreateNode( NODE_FRAME, frame, rootNode, frameWidth, frameHeight, x, y );
		xcb_reparent_window( c, n->window, p->window, BORDER_SIZE_LEFT, BORDER_SIZE_TOP );
		n->parent = p;
		AddNodeToList( p, &windowList );
		AddNodeToList( p, &rootNode->children );
		p->managementState = n->managementState = STATE_REPARENTED;
		dbgprintf( 2, "New normal window\n");
	} else if ( p != rootNode ) {
		n->managementState = STATE_CHILD;
		dbgprintf( 2, "New child window %x (child of %x)\n", n->window, p->window );
	} else {
		n->managementState = STATE_NO_REDIRECT;
		dbgprintf( 2, "New unreparented window\n" );
	}

	v[0] = 	XCB_EVENT_MASK_EXPOSURE | 
						XCB_EVENT_MASK_PROPERTY_CHANGE |
						XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | 
						XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;

	xcb_change_window_attributes( c, n->window, XCB_CW_EVENT_MASK, v );
	AddNodeToList( n, &p->children );
	AddNodeToList( n, &windowList );
	RaiseClient( n );
}

void ReparentExistingWindows() {
	xcb_query_tree_cookie_t treecookie;
	xcb_query_tree_reply_t *treereply;
	xcb_get_geometry_cookie_t geocookie;
	xcb_get_geometry_reply_t *georeply;
	xcb_get_window_attributes_cookie_t attrcookie;
	xcb_get_window_attributes_reply_t *attrreply;
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
		attrcookie = xcb_get_window_attributes( c, children[i] );
		attrreply = xcb_get_window_attributes_reply( c, attrcookie, NULL );
		if ( ( georeply != NULL ) && ( attrreply != NULL) && ( attrreply->override_redirect == 0 ) ) {
			ReparentWindow( children[i], screen->root, georeply->x, georeply->y, georeply->width, georeply->height, 0 );
		}
		if ( georeply )
			free( georeply );
		if ( attrreply )
			free ( attrreply );
	}
	free( treereply );
}

/*
==============
Event handlers
==============
*/

void DoButtonPress( xcb_button_press_event_t *e ) {
	node_t *n = GetNodeByWindow( e->event );

	RaiseClient( n );
	if ( n->type == NODE_CLIENT )
		return;

	if ( n->type == NODE_FRAME ) {
		if ( mouseIsOverCloseButton ) {
			wmState = WMSTATE_CLOSE;
			AddNodeToList( n, &redrawList );
			return;
		} else {
			if ( e->event_x > n->width - 8 || e->event_y > n->height - 8 ) {
				wmState = WMSTATE_RESIZE;
				resizeDir = RESIZE_NONE;
				if ( e->event_x > n->width - 8 ) {
					resizeDir |= RESIZE_HORIZONTAL;
				}
				if ( e->event_y > n->height - 1 ) {
					resizeDir |= RESIZE_VERTICAL;
				}
			} else {
				wmState = WMSTATE_DRAG;
			}
			dragClient = n;
			dragStartX = e->event_x;
			dragStartY = e->event_y;
		}
	}
}

void DoButtonRelease( xcb_button_release_event_t *e ) {
	SetCursor( 68 );
	switch ( wmState ) {
		case WMSTATE_IDLE:
			break;
		case WMSTATE_DRAG:
		case WMSTATE_RESIZE:
			if ( dragChanged )
				ConfigureClient( dragClient->children.nodes[0], dragNewX, dragNewY, dragNewW, dragNewH );
			dragChanged = false;
			wmState = WMSTATE_IDLE;
			resizeDir = RESIZE_NONE;
			dragClient = NULL;
			dragStartX = 0;
			dragStartY = 0;
			break;
		case WMSTATE_CLOSE:
			if ( mouseIsOverCloseButton == 1 ) {
				xcb_client_message_event_t *msg = calloc(32, 1);
				msg->response_type = XCB_CLIENT_MESSAGE;
				msg->window = windowList.nodes[0]->window;
				msg->format = 32;
				msg->sequence = 0;
				msg->type = WM_PROTOCOLS;
				msg->data.data32[0] = WM_DELETE_WINDOW;
				msg->data.data32[1] = XCB_CURRENT_TIME;
				xcb_send_event( c, 0, windowList.nodes[0]->window, XCB_EVENT_MASK_NO_EVENT, (char*)msg );
				
				free( msg );
			}
			wmState = WMSTATE_IDLE;
			AddNodeToList( windowList.nodes[0], &redrawList );
			break;
		default:
			wmState = WMSTATE_IDLE;
			break;
	}
}

void DoMotionNotify( xcb_motion_notify_event_t *e ) {
	node_t* n;
	int x, y, w, h;

	mouseLastKnownX = e->root_x;
	mouseLastKnownY = e->root_y;

	mouseIsOverCloseButton = 0;
	if ( e->event_x >= 9 && e->event_x <= 20 ) {
		if ( e->event_y >= 4 && e->event_y <= 15 ) {
			mouseIsOverCloseButton = 1;
		}
	}

	switch ( wmState ) {
		case WMSTATE_CLOSE:
			AddNodeToList( windowList.nodes[0], &redrawList );
			break;
		case WMSTATE_DRAG:
			dragNewX = e->root_x - dragStartX;
			dragNewY =  e->root_y - dragStartY;
			dragNewW = dragClient->children.nodes[0]->width;
			dragNewH = dragClient->children.nodes[0]->height;
			dragChanged = true;
			return;
		case WMSTATE_RESIZE:
			dragNewX = dragClient->x;
			dragNewY = dragClient->y;
			dragNewW = e->root_x - ( dragNewX + BORDER_SIZE_LEFT );
			dragNewH = e->root_y - ( dragNewY + BORDER_SIZE_TOP );
			if ( dragNewH < 16 )
				dragNewH = 16;
			if ( dragNewW < 16 )
				dragNewW = 16;
			dragChanged = true;
			printf("resize w%hi h%hi\n", dragNewW, dragNewH );
			printf( "dcX %hi dcY %hi nX %hi nY %hi\n", dragNewX, dragNewY, dragClient->children.nodes[0]->x, dragClient->children.nodes[0]->y );
			return;
		default:
			n = GetNodeByWindow( e->event );
			if ( ( n != NULL ) && ( n->type == NODE_FRAME ) ) {
				if ( e->event_x > n->width - 8 || e->event_y > n->height - 8 ) {
					if ( e->event_x > n->width - 8 && e->event_y > n->height - 8 ) {
						SetCursor( 14 );
					} else if ( e->event_x > n->width - 8 ) {
						SetCursor( 108 );
					} else {
						SetCursor( 116 );
					}
				}
			} else {
				SetCursor( 68 );
			}
	}
}

void DoExpose( xcb_expose_event_t *e ) {
	AddNodeToList( GetNodeByWindow( e->window ), &redrawList );
}

void DoCreateNotify( xcb_create_notify_event_t *e ) {
	int x = e->x, y = e->y;
	if ( ( e->override_redirect == 0 ) && ( e->x == 0 ) && ( e->y == 0 ) ) {
		x = spawnx;
		y = spawny;
		spawnx += spawnxdir;
		spawny += spawnydir;
		if ( ( spawnx > rootNode->width ) || spawnx < 0 ) {
			spawnxdir = spawnxdir * -1;
			spawnx += ( spawnxdir * 2 );
		}
		if ( ( spawny > rootNode->height ) || spawny < 40 ) {
			spawnydir = spawnydir * -1;
			spawny += ( spawnydir * 2 );
		}
	}
	ReparentWindow( e->window, e->parent, x, y, e->width, e->height, e->override_redirect );
}

void DoDestroy( xcb_destroy_notify_event_t *e ) {
	node_t* node = GetNodeByWindow( e->window );
	if ( node )
		DestroyNode( node );
	else
		fprintf(stderr, "warning: window removed that was not in window list\n");
}

void DoMapRequest( xcb_map_request_event_t *e ) {
	node_t *n = GetNodeByWindow( e->window );
	node_t *p = GetParentFrame( n );

	/* Todo:
		Add ICCCM section 4.1.4 compatibility
		https://tronche.com/gui/x/icccm/sec-4.html#s-4.1.3
	*/
	if ( n == NULL ) {
		return;
	}
	n->windowState = STATE_NORMAL;
	if ( p )
		xcb_map_window( c, p->window );
	xcb_map_window( c, n->window );
	RaiseClient( n );
	dbgprintf( 2, "window %x mapped\n", e->window );
}

//This is all a hack to make ReparentExistingWindows work.
//It relies on the fact that X will unmap then remap windows on reparent,
//if and only if they are already mapped
void DoMapNotify( xcb_map_notify_event_t *e ) {
	node_t *n = GetNodeByWindow( e->window );
	node_t *p = GetParentFrame( n );

	if ( n == NULL ) {
		return;
	}
	if ( n->parentMapped == 0 ) {  
		n->windowState = STATE_NORMAL;
		n->parentMapped = 1;
		if ( p )
			xcb_map_window( c, p->window );
		RaiseClient( n );
	}
}

void DoUnmapNotify( xcb_unmap_notify_event_t *e ) {
	node_t *n = GetNodeByWindow( e->window );
	node_t *p = GetParentFrame( n );

	if ( n == NULL ) {
		return;
	}
	if ( n->parentMapped == 1 ) {
		n->windowState = STATE_WITHDRAWN;
		n->parentMapped = 0;
		if ( p )
			xcb_unmap_window( c, p->window );
	}
}

void DoReparentNotify( xcb_reparent_notify_event_t *e ) {
	node_t *n = GetNodeByWindow( e->window );

	if ( n == NULL ) {
		return;
	}

	n->managementState = STATE_REPARENTED;
	ConfigureClient( n, n->x, n->y, n->width, n->height );
	dbgprintf( 1, "window %x reparented to window %x\n", e->window, e->parent );
	return;
}

void DoConfigureRequest( xcb_configure_request_event_t *e ) {
	node_t* n = GetNodeByWindow( e->window );
	node_t* p;
	int x, y, width, height;
	
	if ( n == NULL )
		return;
	p = GetParentFrame( n );
	if ( p == NULL ) {
		x = n->x;
		y = n->y;
	} else {
		x = p->x;
		y = p->y;
	}
	width = n->width;
	height = n->height;

	if ( ( e->value_mask & XCB_CONFIG_WINDOW_X ) != 0 )
		x = e->x;
	if ( ( e->value_mask & XCB_CONFIG_WINDOW_Y ) != 0 )
		y = e->y;
	if ( ( e->value_mask & XCB_CONFIG_WINDOW_WIDTH ) != 0  )
		width = e->width;
	if ( ( e->value_mask & XCB_CONFIG_WINDOW_HEIGHT ) != 0 )
		height = e->height;

	ConfigureClient( n, x, y, width, height );
}

void DoConfigureNotify( xcb_configure_notify_event_t *e ) {
	return;
}

void DoPropertyNotify( xcb_property_notify_event_t *e ) {
	xcb_get_property_cookie_t cookie;
    xcb_get_property_reply_t *reply;
	node_t *n = GetNodeByWindow( e->window );

	if ( n == NULL )
		return;

	if ( e->atom == XCB_ATOM_WM_NAME ) {
		cookie = xcb_get_property( c, 0, e->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 256 );
		if ((reply = xcb_get_property_reply(c, cookie, NULL))) {
			int len = xcb_get_property_value_length(reply);
			if ( len != 0 ) {
				memset( n->name, '\0', 256 );
				strncpy( n->name, (char*)xcb_get_property_value( reply ), len > 255 ? 255 : len );
				AddNodeToList( n, &redrawList );
			}
		}	
		free( reply );
	} else {
		dbgprintf( 1, "window %x updated unknown atom %s\n", e->window, xcb_get_atom_name_name( xcb_get_atom_name_reply( c, xcb_get_atom_name( c, e->atom ), NULL ) ) );
	}
}

void DoClientMessage( xcb_client_message_event_t *e ) {
	int i;
	xcb_get_atom_name_cookie_t nameCookie;
	xcb_get_atom_name_reply_t* nameReply;

	nameCookie = xcb_get_atom_name( c, e->type );
	nameReply = xcb_get_atom_name_reply( c, nameCookie, NULL );

	dbgprintf( 2, "received client message\n" );
	dbgprintf( 2, "format: %i\n", e->format );
	dbgprintf( 2, "type: %s\n", xcb_get_atom_name_name( nameReply ) );
	if ( !strcmp( xcb_get_atom_name_name( nameReply ), "_NET_WM_STATE" ) ) {
		dbgprintf( 2, "message is _NET_WM_STATE\n" );
		for ( i = 0; i < 3; i++ ) {
			if ( e->data.data32[i] == 0 )
				break;
			nameCookie = xcb_get_atom_name( c, e->data.data32[i] );
			nameReply = xcb_get_atom_name_reply( c, nameCookie, NULL );
			dbgprintf( 2, "data[i]: %s\n", xcb_get_atom_name_name( nameReply ) );
		}
	} else if ( !strcmp( xcb_get_atom_name_name( nameReply ), "_MAKRON_RELOAD" ) ) {
		dbgprintf( 2, "reloading config\n" );
		dict = iniparser_load( ".makronrc" );
		SetupColors();
		iniparser_freedict( dict );
		AddNodeToList( windowList.nodes[0], &redrawList );
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

	printf( "%s %s build %s\n\n", PROGRAM_NAME, VERSION_STRING, VERSION_BUILDSTR );

	if ( SulfurInit( NULL ) != 0 ) {
			fprintf( stderr, "Problem starting up. Is X running?\n" );
			Cleanup();
			return 1;
	}
	c = sulfurGetXcbConn();
	screen = sulfurGetXcbScreen();

	if ( BecomeWM() < 0 ) {
		fprintf( stderr, "it looks like another wm is running.\n" );
		fprintf( stderr, "you will need to close it before you can run makron.\n" );
		Cleanup();
		return 1;
	}

	/* initialize the client list to empty */
	windowList.max = 4;
	windowList.nodes = calloc( windowList.max, sizeof ( node_t* ) );
	redrawList.max = 4;
	redrawList.nodes = calloc( redrawList.max, sizeof ( node_t* ) );

	homedir = getenv( "HOME" );
	if ( !homedir ) {
		struct passwd *pw = getpwuid( getuid() );
		homedir = pw->pw_dir;
	}
	if ( homedir ) {
		chdir( homedir );
	}

	dict = iniparser_load( ".makronrc" );
	if ( !dict ) {
		fprintf( stderr, "couldn't open .makronrc\n" );
	}
	SetupAtoms();
	SetupColors();
	SetupFonts();
	SetupRoot();
	SetCursor( 68 );
	ReparentExistingWindows();
	iniparser_freedict( dict );

	e = xcb_wait_for_event( c );
	while( !xcb_connection_has_error( c ) ) {
		do {
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
				default: 					dbgprintf( 1, "warning, unhandled event #%d\n", e->response_type & ~0x80 ); break;
			}
			free( e );
		} while( !xcb_connection_has_error( c ) && ( ( e = xcb_poll_for_event( c ) ) != NULL ) );
		if ( dragClient && dragChanged ) {
			ConfigureClient( dragClient->children.nodes[0], dragNewX, dragNewY, dragNewW, dragNewH );
			dragChanged = false;
		}
		while ( redrawList.nodes[0] != NULL ) {
			DrawFrame( redrawList.nodes[0] );
			RemoveNodeFromList( redrawList.nodes[0], &redrawList );
		}
		xcb_flush( c );
		e = xcb_wait_for_event( c );
	}
	Cleanup();
	printf( "connection closed. goodbye!\n" );
	return 0;
}