
#define PROGRAM_NAME "makron"

#define VERSION_STRING "0.2.0-pre0"
#define VERSION_BUILDSTR "43"

#define BORDER_SIZE_LEFT 1
#define BORDER_SIZE_RIGHT 1
#define BORDER_SIZE_TOP 19
#define BORDER_SIZE_BOTTOM 1

#define FONT_NAME "fixed"

typedef enum {
	STATE_WITHDRAWN = 0,
	STATE_ICON = 1,
	STATE_NORMAL = 3,
} clientWindowState_t;

typedef enum {
	STATE_INIT,
	STATE_NO_REDIRECT, //override redirect
	STATE_REPARENTED,
	STATE_CHILD,
	STATE_TRANSIENT
} clientManagementState_t;

typedef enum {
	WMSTATE_IDLE,
	WMSTATE_DRAG,
	WMSTATE_RESIZE,
	WMSTATE_CLOSE
} wmState_t;

typedef enum {
	NODE_ROOT,
	NODE_CLIENT,
	NODE_FRAME,
	NODE_GROUP,
} nodeType_t;

struct node_s;

typedef struct nodeList_s {
	struct node_s** nodes;
	int max;
} nodeList_t;

typedef struct node_s {
	nodeType_t type;
	xcb_window_t window;
	char name[256];
	short x, y, width, height;

	struct node_s* parent;
	struct nodeList_s children;
	char parentMapped;
	
	clientWindowState_t windowState;
	clientManagementState_t managementState;
	//todo: gravity
} node_t;