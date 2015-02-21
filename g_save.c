/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 * Copyright (C) 2000-2002 Mr. Hyde and Mad Dog
 * Copyright (C) 2011 Knightmare
 * Copyright (C) 2011 Yamagi Burmeister
 * Copyright (C) 2014 Luke Groeninger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * The savegame system.
 *
 * =======================================================================
 */

/*
 * This is the Quake 2 savegame system, fixed by Yamagi
 * based on an idea by Knightmare of kmquake2. This major
 * rewrite of the original g_save.c is much more robust
 * and portable since it doesn't use any function pointers.
 *
 * Inner workings:
 * When the game is saved all function pointers are
 * translated into human readable function definition strings.
 * The same way all mmove_t pointers are translated. This
 * human readable strings are then written into the file.
 * At game load the human readable strings are retranslated
 * into the actual function pointers and struct pointers. The
 * pointers are generated at each compilation / start of the
 * client, thus the pointers are always correct.
 *
 * Limitations:
 * While savegames survive recompilations of the game source
 * and bigger changes in the source, there are some limitation
 * which a nearly impossible to fix without a object orientated
 * rewrite of the game.
 *  - If functions or mmove_t structs that a referencenced
 *    inside savegames are added or removed (e.g. the files
 *    in tables/ are altered) the load functions cannot
 *    reconnect all pointers and thus not restore the game.
 *  - If the operating system is changed internal structures
 *    may change in an unrepairable way.
 *  - If the architecture is changed pointer length and
 *    other internal datastructures change in an
 *    incompatible way.
 *  - If the edict_t struct is changed, savegames
 *    will break.
 * This is not so bad as it looks since functions and
 * struct won't be added and edict_t won't be changed
 * if no big, sweeping changes are done. The operating
 * system and architecture are in the hands of the user.
 */

#include "g_local.h"
/*
*
* When ever the savegame version
* is changed, q2 will refuse to
* load older savegames. This
* should be bumped if the files
* in tables/ are changed, otherwise
* strange things may happen.
*/
#define SAVEGAMEVER "Q2VR-1"


/*
 * This macros are used to
 * prohibit loading of savegames
 * created on other systems or
 * architectures. This will
 * crash q2 in spectacular
 * ways
 */
#if defined(__APPLE__)
#define OS "MacOS X"
#elif defined(__FreeBSD__)
#define OS "FreeBSD"
#elif defined(__OpenBSD__)
#define OS "OpenBSD"
#elif defined(__linux__)
#define OS "Linux"
#elif defined(_WIN32)
#define OS "Windows"
#else
#define OS "Unknown"
#endif

#if (defined _M_IX86 || defined __i386__)
#define ARCH "x86"
#elif defined(_M_X64) || defined(__x86_64__)
#define ARCH "x86-64"
#else
#define ARCH "unknown"
#endif

/*
 * Connects a human readable
 * function signature with
 * the corresponding pointer
 */
typedef struct
{
    char *funcStr;
    byte *funcPtr;
#ifdef Q2VR_ENGINE_MOD
    hash128_t funcHash;
#endif
} functionList_t;

/*
 * Connects a human readable
 * mmove_t string with the
 * corresponding pointer
 * 
 */
typedef struct
{
    char	*mmoveStr;
    mmove_t *mmovePtr;
#ifdef Q2VR_ENGINE_MOD
    hash128_t mmoveHash;
#endif
} mmoveList_t;

/* ========================================================= */

/*
 * Prototypes for forward
 * declaration for all game
 * functions.
 */
#include "tables/gamefunc_decs.h"

/*
 * List with function pointer
 * to each of the functions
 * prototyped above.
 */
functionList_t functionList[] = {
#include "tables/gamefunc_list.h"
};

/*
 * Prototypes for forward
 * declaration for all game
 * mmove_t functions.
 */
#include "tables/gamemmove_decs.h"

/*
 * List with pointers to
 * each of the mmove_t
 * functions prototyped
 * above.
 */
mmoveList_t mmoveList[] = {
#include "tables/gamemmove_list.h"
};

/*
 * Fields to be saved
 */
field_t fields[] = {
#include "tables/fields.h"
};

/*
 * Level fields to
 * be saved
 */
field_t levelfields[] = {
#include "tables/levelfields.h"
};

/*
 * Client fields to
 * be saved
 */
field_t clientfields[] = {
#include "tables/clientfields.h"
};


static uint32_t funcListSize = 0;

void siftFuncDown(functionList_t *list, int32_t start, int32_t end) {
    int32_t root = start;
    while (root * 2 + 1 <= end) {
        int32_t child = root * 2 + 1;
        int32_t swap = root;
        if (list[swap].funcPtr < list[child].funcPtr) {
            swap = child;
        }
        if (child +1 <= end && list[swap].funcPtr < list[child+1].funcPtr) {
            swap = child + 1;
        }
        if (swap == root) {
            return;
        } else {
            functionList_t tmp = functionList[root];
            functionList[root] = functionList[swap];
            functionList[swap] = tmp;
            root = swap;
        }
    }
}

void sortFunctionAddresses(void) {
    funcListSize = sizeof(functionList) / sizeof(functionList[0]) - 1;
    int32_t start = floor((funcListSize - 2) / 2.0);
    int32_t end = funcListSize - 1;
    while (start >= 0) {
        siftFuncDown(functionList, start, funcListSize - 1);
        start -= 1;
    }
    while (end > 0) {
        functionList_t tmp = functionList[end];
        functionList[end] = functionList[0];
        functionList[0] = tmp;
        end -= 1;
        siftFuncDown(functionList, 0, end);
    }
}


static uint32_t mmoveListSize = 0;

void siftMoveDown(mmoveList_t *list, int32_t start, int32_t end) {
    int32_t root = start;
    while (root * 2 + 1 <= end) {
        int32_t child = root * 2 + 1;
        int32_t swap = root;
        if (list[swap].mmovePtr < list[child].mmovePtr) {
            swap = child;
        }
        if (child +1 <= end && list[swap].mmovePtr < list[child+1].mmovePtr) {
            swap = child + 1;
        }
        if (swap == root) {
            return;
        } else {
            mmoveList_t tmp = mmoveList[root];
            mmoveList[root] = mmoveList[swap];
            mmoveList[swap] = tmp;
            root = swap;
        }
    }
}

void sortMoveAddresses(void) {
    mmoveListSize = (sizeof(mmoveList) / sizeof(mmoveList[0])) - 1;
    int32_t start = floor((mmoveListSize - 2) / 2.0);
    int32_t end = mmoveListSize - 1;
    while (start >= 0) {
        siftMoveDown(mmoveList, start, mmoveListSize - 1);
        start -= 1;
    }
    while (end > 0) {
        mmoveList_t tmp = mmoveList[end];
        mmoveList[end] = mmoveList[0];
        mmoveList[0] = tmp;
        end -= 1;
        siftMoveDown(mmoveList, 0, end);
    }
}

/*
============
InitGame

This will be called when the dll is first loaded, which
only happens when a new game is started or a save game
is loaded.
============
*/
#include <inttypes.h>


void InitGame (void)
{

	gi.dprintf ("\n==== InitGame (Lazarus) ====\n");
	gi.dprintf("by Mr. Hyde & Mad Dog\ne-mail: rascal@vicksburg.com\n\n");

#ifdef Q2VR_ENGINE_MOD
    {
        int i;
        gi.dprintf("Initializing hash tables...");
        
        for (i = 0; mmoveList[i].mmoveStr; i++)
        {
            mmoveList[i].mmoveHash = gi.Hash128(mmoveList[i].mmoveStr, strlen(mmoveList[i].mmoveStr));
        }
        
        for (i = 0; functionList[i].funcStr; i++)
        {
            functionList[i].funcHash = gi.Hash128(functionList[i].funcStr, strlen(functionList[i].funcStr));
        }
        
        gi.dprintf(" Done!\n");
    }
#endif
  
    gi.dprintf("Sorting tables...");
    
    sortFunctionAddresses();
    
    sortMoveAddresses();
    
    gi.dprintf(" Done!\n");
    
    // Knightmare- init cvars
	lithium_defaults();

	gun_x = gi.cvar ("gun_x", "0", 0);
	gun_y = gi.cvar ("gun_y", "0", 0);
	gun_z = gi.cvar ("gun_z", "0", 0);

	//FIXME: sv_ prefix is wrong for these
	sv_rollspeed = gi.cvar ("sv_rollspeed", "200", 0);
	sv_rollangle = gi.cvar ("sv_rollangle", "2", 0);
	sv_maxvelocity = gi.cvar ("sv_maxvelocity", "2000", 0);
	sv_gravity = gi.cvar ("sv_gravity", "800", 0);

	// noset vars
	dedicated = gi.cvar ("dedicated", "0", CVAR_NOSET);

	// latched vars
	sv_cheats = gi.cvar ("cheats", "0", CVAR_SERVERINFO|CVAR_LATCH);
	gi.cvar ("gamename", GAMEVERSION , CVAR_SERVERINFO | CVAR_LATCH);
	gi.cvar ("gamedate", __DATE__ , CVAR_SERVERINFO | CVAR_LATCH);

	maxclients = gi.cvar ("maxclients", "4", CVAR_SERVERINFO | CVAR_LATCH);
	maxspectators = gi.cvar ("maxspectators", "4", CVAR_SERVERINFO);
	deathmatch = gi.cvar ("deathmatch", "0", CVAR_LATCH);
	coop = gi.cvar ("coop", "0", CVAR_LATCH);
	skill = gi.cvar ("skill", "1", CVAR_LATCH);

	// Knightmare- increase maxentities
	//maxentities = gi.cvar ("maxentities", "1024", CVAR_LATCH);
	maxentities = gi.cvar ("maxentities", va("%i",MAX_EDICTS), CVAR_LATCH);

	// change anytime vars
	dmflags = gi.cvar ("dmflags", "0", CVAR_SERVERINFO);
	fraglimit = gi.cvar ("fraglimit", "0", CVAR_SERVERINFO);
	timelimit = gi.cvar ("timelimit", "0", CVAR_SERVERINFO);
//ZOID
	capturelimit = gi.cvar ("capturelimit", "0", CVAR_SERVERINFO);
	instantweap = gi.cvar ("instantweap", "0", CVAR_SERVERINFO);
//ZOID
	password = gi.cvar ("password", "", CVAR_USERINFO);
	spectator_password = gi.cvar ("spectator_password", "", CVAR_USERINFO);
	needpass = gi.cvar ("needpass", "0", CVAR_SERVERINFO);
	filterban = gi.cvar ("filterban", "1", 0);

	g_select_empty = gi.cvar ("g_select_empty", "0", CVAR_ARCHIVE);

	run_pitch = gi.cvar ("run_pitch", "0.002", 0);
	run_roll = gi.cvar ("run_roll", "0.005", 0);
	bob_up  = gi.cvar ("bob_up", "0.005", 0);
	bob_pitch = gi.cvar ("bob_pitch", "0.002", 0);
	bob_roll = gi.cvar ("bob_roll", "0.002", 0);

	// flood control
	flood_msgs = gi.cvar ("flood_msgs", "4", 0);
	flood_persecond = gi.cvar ("flood_persecond", "4", 0);
	flood_waitdelay = gi.cvar ("flood_waitdelay", "10", 0);

	// dm map list
	sv_maplist = gi.cvar ("sv_maplist", "", 0);

	// Lazarus
	actorchicken = gi.cvar("actorchicken", "1", CVAR_LATCH);
	actorjump = gi.cvar("actorjump", "1", CVAR_LATCH);
	actorscram = gi.cvar("actorscram", "1", CVAR_LATCH);
	alert_sounds = gi.cvar("alert_sounds", "0", CVAR_ARCHIVE);
	allow_fog = gi.cvar ("allow_fog", "1", CVAR_ARCHIVE);

	// set to 0 to bypass target_changelevel clear inventory flag
	// because some user maps have this erroneously set
	allow_clear_inventory = gi.cvar ("allow_clear_inventory", "1", CVAR_ARCHIVE);

	cd_loopcount = gi.cvar("cd_loopcount","4",0);
	cl_gun = gi.cvar("cl_gun", "1", 0);
	cl_thirdperson = gi.cvar(CLIENT_THIRDPERSON_CVAR, "0", 0); // Knightmare added
	corpse_fade = gi.cvar("corpse_fade", "0", CVAR_ARCHIVE);
	corpse_fadetime = gi.cvar("corpse_fadetime", "20", 0);
	crosshair = gi.cvar("crosshair", "1", 0);
	footstep_sounds = gi.cvar("footstep_sounds", "0", CVAR_SERVERINFO|CVAR_LATCH);
	fov = gi.cvar("fov", "90", 0);
	hand = gi.cvar("hand", "0", 0);
	jetpack_weenie = gi.cvar("jetpack_weenie", "0", CVAR_CHEAT);
	joy_pitchsensitivity = gi.cvar("joy_pitchsensitivity", "1", 0);
	joy_yawsensitivity = gi.cvar("joy_yawsensitivity", "-1", 0);
	jump_kick = gi.cvar("jump_kick", "0", CVAR_SERVERINFO|CVAR_LATCH);
	lights = gi.cvar("lights", "1", 0);
	lightsmin = gi.cvar("lightsmin", "a", CVAR_SERVERINFO);
	m_pitch = gi.cvar("m_pitch", "0.022", 0);
	m_yaw = gi.cvar("m_yaw", "0.022", 0);
	monsterjump = gi.cvar("monsterjump", "1", CVAR_SERVERINFO|CVAR_LATCH);
	rocket_strafe = gi.cvar("rocket_strafe", "0", 0);
#ifdef KMQUAKE2_ENGINE_MOD
	sv_maxgibs = gi.cvar("sv_maxgibs", "160", CVAR_SERVERINFO);
#else
	sv_maxgibs = gi.cvar("sv_maxgibs", "20", CVAR_SERVERINFO);
#endif
	turn_rider = gi.cvar("turn_rider", "1", CVAR_CHEAT);
	zoomrate = gi.cvar("zoomrate", "80", CVAR_ARCHIVE);
	zoomsnap = gi.cvar("zoomsnap", "20", CVAR_ARCHIVE);

	// shift_ and rotate_distance only used for debugging stuff - this is the distance
	// an entity will be moved by "item_left", "item_right", etc.
	shift_distance = gi.cvar("shift_distance", "1", CVAR_CHEAT);
	rotate_distance = gi.cvar("rotate_distance", "1", CVAR_CHEAT);

	// GL stuff
	gl_clear = gi.cvar("gl_clear", "0", 0);

	// Lazarus saved cvars that we may or may not manipulate, but need to
	// restore to original values upon map exit.
	lazarus_cd_loop = gi.cvar("lazarus_cd_loop", "0", 0);
	lazarus_gl_clear= gi.cvar("lazarus_gl_clear","0", 0);
	lazarus_pitch   = gi.cvar("lazarus_pitch",   "0", 0);
	lazarus_yaw     = gi.cvar("lazarus_yaw",     "0", 0);
	lazarus_joyp    = gi.cvar("lazarus_joyp",    "0", 0);
	lazarus_joyy    = gi.cvar("lazarus_joyy",    "0", 0);
	lazarus_cl_gun  = gi.cvar("lazarus_cl_gun",  "0", 0);
	lazarus_crosshair = gi.cvar("lazarus_crosshair", "0", 0);

	/*if(lazarus_gl_clear->value)
		gi.cvar_forceset("gl_clear",         va("%d",lazarus_gl_clear->value));
	else
		gi.cvar_forceset("lazarus_gl_clear", va("%d",gl_clear->value));*/

	if(!deathmatch->value && !coop->value)
	{
		/*if(lazarus_pitch->value) {
			gi.cvar_forceset("cd_loopcount",         va("%d",(int)(lazarus_cd_loop->value)));
			gi.cvar_forceset("m_pitch",              va("%f",lazarus_pitch->value));
			gi.cvar_forceset("m_yaw",                va("%f",lazarus_yaw->value));
			gi.cvar_forceset("cl_gun",               va("%d",(int)(lazarus_cl_gun->value)));
			gi.cvar_forceset("crosshair",            va("%d",(int)(lazarus_crosshair->value)));
		} else {*/
			gi.cvar_forceset("lazarus_cd_loop",        va("%d",(int)(cd_loopcount->value)));
#ifndef KMQUAKE2_ENGINE_MOD // engine has zoom autosensitivity
			gi.cvar_forceset("lazarus_pitch",          va("%f",m_pitch->value));
			gi.cvar_forceset("lazarus_yaw",            va("%f",m_yaw->value));
			gi.cvar_forceset("lazarus_joyp",           va("%f",joy_pitchsensitivity->value));
			gi.cvar_forceset("lazarus_joyy",           va("%f",joy_yawsensitivity->value));
#endif
			gi.cvar_forceset("lazarus_cl_gun",         va("%d",(int)(cl_gun->value)));
			gi.cvar_forceset("lazarus_crosshair",      va("%d",(int)(crosshair->value)));
		//}
	}

	tpp = gi.cvar ("tpp", "0", CVAR_ARCHIVE);
	tpp_auto = gi.cvar ("tpp_auto", "1", 0);
	crossh = gi.cvar ("crossh", "1", 0);
	allow_download = gi.cvar("allow_download", "0", 0);

	blaster_color = gi.cvar("blaster_color", "1", 0); // Knightmare added

	// If this is an SP game and "readout" is not set, force allow_download off
	// so we don't get the annoying "Refusing to download path with .." messages
	// due to misc_actor sounds.
	if(allow_download->value && !readout->value && !deathmatch->value)
		gi.cvar_forceset("allow_download", "0");

	bounce_bounce = gi.cvar("bounce_bounce", "0.5", 0);
	bounce_minv   = gi.cvar("bounce_minv",   "60",  0);

	// items
	InitItems ();

	Com_sprintf (game.helpmessage1, sizeof(game.helpmessage1), "");

	Com_sprintf (game.helpmessage2, sizeof(game.helpmessage2), "");

	// initialize all entities for this game
	game.maxentities = maxentities->value;
	g_edicts =  gi.TagMalloc (game.maxentities * sizeof(g_edicts[0]), TAG_GAME);
	globals.edicts = g_edicts;
	globals.max_edicts = game.maxentities;

	// initialize all clients for this game
	game.maxclients = maxclients->value;
	game.clients = gi.TagMalloc (game.maxclients * sizeof(game.clients[0]), TAG_GAME);
	globals.num_edicts = game.maxclients+1;

//ZOID
	CTFInit();
//ZOID
}
/* ========================================================= */

/*
 * Helper function to get
 * the human readable function
 * definition by an address.
 * Called by WriteField1 and
 * WriteField2.
 */
functionList_t *
GetFunctionByAddress(byte *adr)
{
    int min = 0;
    int max = funcListSize - 1;
    while (max >= min) {
        int mid = floor((max - min) / 2.0) + min;
        if (functionList[mid].funcPtr == adr)
        {
            return &functionList[mid];
        } else if (functionList[mid].funcPtr < adr) {
            min = mid + 1;
        } else {
            max = mid - 1;
        }
    }
    return NULL;
}

/*
 * Helper function to get the
 * pointer to a function by
 * it's human readable name.
 * Called by WriteField1 and
 * WriteField2.
 */
byte *
FindFunctionByName(char *name)
{
    int i;
#ifdef Q2VR_ENGINE_MOD
    hash128_t nameHash = gi.Hash128(name, strlen(name));
#endif
    for (i = 0; functionList[i].funcStr; i++)
    {
#ifdef Q2VR_ENGINE_MOD
        if (!gi.HashCompare128(nameHash, functionList[i].funcHash) && !strcmp(name, functionList[i].funcStr))
#else
        if (!strcmp(name, functionList[i].funcStr))
#endif
        {
            return functionList[i].funcPtr;
        }
    }
    
    return NULL;
}

/*
 * Helper function to get the
 * human readable definition of
 * a mmove_t struct by a pointer.
 */
mmoveList_t *
GetMmoveByAddress(mmove_t *adr)
{
    int min = 0;
    int max = mmoveListSize - 1;
    while (max >= min) {
        int mid = floor((max - min) / 2.0) + min;
        if (mmoveList[mid].mmovePtr == adr)
        {
            return &mmoveList[mid];
        } else if (mmoveList[mid].mmovePtr < adr) {
            min = mid + 1;
        } else {
            max = mid - 1;
        }
    }
    return NULL;
}

/*
 * Helper function to get the
 * pointer to a mmove_t struct
 * by a human readable definition.
 */
mmove_t *
FindMmoveByName(char *name)
{
    int i;
#ifdef Q2VR_ENGINE_MOD
    hash128_t nameHash = gi.Hash128(name, strlen(name));
#endif
    for (i = 0; mmoveList[i].mmoveStr; i++)
    {
#ifdef Q2VR_ENGINE_MOD
        if (!gi.HashCompare128(nameHash, mmoveList[i].mmoveHash) && !strcmp(name, mmoveList[i].mmoveStr))
#else
        if (!strcmp(name, mmoveList[i].mmoveStr))
#endif
        {
            return mmoveList[i].mmovePtr;
        }
    }
    
    return NULL;
}


/* ========================================================= */

/*
 * The following two functions are
 * doing the dirty work to write the
 * data generated by the functions
 * below this block into files.
 */
void
WriteField1(FILE *f, field_t *field, byte *base)
{
    void *p;
    int len;
    int index;
    functionList_t *func;
    mmoveList_t *mmove;
    
    if (field->flags & FFL_SPAWNTEMP)
    {
        return;
    }
    
    p = (void *)(base + field->ofs);
    
    switch (field->type)
    {
        case F_INT:
        case F_FLOAT:
        case F_ANGLEHACK:
        case F_VECTOR:
        case F_IGNORE:
            break;
            
        case F_LSTRING:
        case F_GSTRING:
            
            if (*(char **)p)
            {
                len = strlen(*(char **)p) + 1;
            }
            else
            {
                len = 0;
            }
            
            *(int *)p = len;
            break;
        case F_EDICT:
            
            if (*(edict_t **)p == NULL)
            {
                index = -1;
            }
            else
            {
                index = *(edict_t **)p - g_edicts;
            }
            
            *(int *)p = index;
            break;
        case F_CLIENT:
            
            if (*(gclient_t **)p == NULL)
            {
                index = -1;
            }
            else
            {
                index = *(gclient_t **)p - game.clients;
            }
            
            *(int *)p = index;
            break;
        case F_ITEM:
            
            if (*(edict_t **)p == NULL)
            {
                index = -1;
            }
            else
            {
                index = *(gitem_t **)p - itemlist;
            }
            
            *(int *)p = index;
            break;
        case F_FUNCTION:
            
            if (*(byte **)p == NULL)
            {
                len = 0;
            }
            else
            {
                func = GetFunctionByAddress (*(byte **)p);
                
                if (!func)
                {
                    gi.error ("WriteField1: function not in list, can't save game");
                }
                
                len = strlen(func->funcStr)+1;
            }
            
            *(int *)p = len;
            break;
        case F_MMOVE:
            
            if (*(byte **)p == NULL)
            {
                len = 0;
            }
            else
            {
                mmove = GetMmoveByAddress (*(mmove_t **)p);
                
                if (!mmove)
                {
                    gi.error ("WriteField1: mmove not in list, can't save game");
                }
                
                len = strlen(mmove->mmoveStr)+1;
            }
            
            *(int *)p = len;
            break;
        default:
            gi.error("WriteEdict: unknown field type");
    }
}

void
WriteField2(FILE *f, field_t *field, byte *base)
{
    int len;
    void *p;
    functionList_t *func;
    mmoveList_t *mmove;
    
    if (field->flags & FFL_SPAWNTEMP)
    {
        return;
    }
    
    p = (void *)(base + field->ofs);
    
    switch (field->type)
    {
        case F_LSTRING:
            
            if (*(char **)p)
            {
                len = strlen(*(char **)p) + 1;
                fwrite(*(char **)p, len, 1, f);
            }
            
            break;
        case F_FUNCTION:
            
            if (*(byte **)p)
            {
                func = GetFunctionByAddress (*(byte **)p);
                
                if (!func)
                {
                    gi.error ("WriteField2: function not in list, can't save game");
                }
                
                len = strlen(func->funcStr)+1;
                fwrite (func->funcStr, len, 1, f);
            }
            
            break;
        case F_MMOVE:
            
            if (*(byte **)p)
            {
                mmove = GetMmoveByAddress (*(mmove_t **)p);
                if (!mmove)
                {
                    gi.error ("WriteField2: mmove not in list, can't save game");
                }
                
                len = strlen(mmove->mmoveStr)+1;
                fwrite (mmove->mmoveStr, len, 1, f);
            }
            
            break;
        default:
            break;
    }
}

/* ========================================================= */

/*
 * This function does the dirty
 * work to read the data from a
 * file. The processing of the
 * data is done in the functions
 * below
 */
void
ReadField(FILE *f, field_t *field, byte *base)
{
    void *p;
    int len;
    int index;
    char funcStr[2048];
    
    if (field->flags & FFL_SPAWNTEMP)
    {
        return;
    }
    
    p = (void *)(base + field->ofs);
    
    switch (field->type)
    {
        case F_INT:
        case F_FLOAT:
        case F_ANGLEHACK:
        case F_VECTOR:
        case F_IGNORE:
            break;
            
        case F_LSTRING:
            len = *(int *)p;
            
            if (!len)
            {
                *(char **)p = NULL;
            }
            else
            {
                *(char **)p = gi.TagMalloc(32 + len, TAG_LEVEL);
                fread(*(char **)p, len, 1, f);
            }
            
            break;
        case F_EDICT:
            index = *(int *)p;
            
            if (index == -1)
            {
                *(edict_t **)p = NULL;
            }
            else
            {
                *(edict_t **)p = &g_edicts[index];
            }
            
            break;
        case F_CLIENT:
            index = *(int *)p;
            
            if (index == -1)
            {
                *(gclient_t **)p = NULL;
            }
            else
            {
                *(gclient_t **)p = &game.clients[index];
            }
            
            break;
        case F_ITEM:
            index = *(int *)p;
            
            if (index == -1)
            {
                *(gitem_t **)p = NULL;
            }
            else
            {
                *(gitem_t **)p = &itemlist[index];
            }
            
            break;
        case F_FUNCTION:
            len = *(int *)p;
            
            if (!len)
            {
                *(byte **)p = NULL;
            }
            else
            {
                if (len > sizeof(funcStr))
                {
                    gi.error ("ReadField: function name is longer than buffer (%i chars)",
                              sizeof(funcStr));
                }
                
                fread (funcStr, len, 1, f);
                
                if ( !(*(byte **)p = FindFunctionByName (funcStr)) )
                {
                    gi.error ("ReadField: function %s not found in table, can't load game", funcStr);
                }
                
            }
            break;
        case F_MMOVE:
            len = *(int *)p;
            
            if (!len)
            {
                *(byte **)p = NULL;
            }
            else
            {
                if (len > sizeof(funcStr))
                {
                    gi.error ("ReadField: mmove name is longer than buffer (%i chars)",
                              sizeof(funcStr));
                }
                
                fread (funcStr, len, 1, f);
                
                if ( !(*(mmove_t **)p = FindMmoveByName (funcStr)) )
                {
                    gi.error ("ReadField: mmove %s not found in table, can't load game", funcStr);
                }
            }
            break;
            
        default:
            gi.error("ReadEdict: unknown field type");
    }
}

/* ========================================================= */

/*
 * Write the client struct into a file.
 */
void
WriteClient(FILE *f, gclient_t *client)
{
    field_t *field;
    gclient_t temp;
    
    /* all of the ints, floats, and vectors stay as they are */
    temp = *client;
    
    /* change the pointers to indexes */
    for (field = clientfields; field->name; field++)
    {
        WriteField1(f, field, (byte *)&temp);
    }
    
    /* write the block */
    fwrite(&temp, sizeof(temp), 1, f);
    
    /* now write any allocated data following the edict */
    for (field = clientfields; field->name; field++)
    {
        WriteField2(f, field, (byte *)client);
    }
}

/*
 * Read the client struct from a file
 */
void
ReadClient(FILE *f, gclient_t *client)
{
    field_t *field;
    
    fread(client, sizeof(*client), 1, f);
    
    for (field = clientfields; field->name; field++)
    {
        ReadField(f, field, (byte *)client);
    }
}

/* ========================================================= */

/*
 * Writes the game struct into
 * a file. This is called when
 * ever the games goes to e new
 * level or the user saves the
 * game. Saved informations are:
 * - cross level data
 * - client states
 * - help computer info
 */
void
WriteGame(const char *filename, qboolean autosave)
{
    FILE *f;
    int i;
    char str_ver[32];
    char str_game[32];
    char str_os[32];
    char str_arch[32];
    
    if (!autosave)
    {
        SaveClientData();
    }
    
    f = fopen(filename, "wb");
    
    if (!f)
    {
        gi.error("Couldn't open %s", filename);
    }
    
    /* Savegame identification */
    memset(str_ver, 0, sizeof(str_ver));
    memset(str_game, 0, sizeof(str_game));
    memset(str_os, 0, sizeof(str_os));
    memset(str_arch, 0, sizeof(str_arch));
    
    Q_strlcpy(str_ver, SAVEGAMEVER, sizeof(str_ver));
    Q_strlcpy(str_game, GAMEVERSION, sizeof(str_game));
    Q_strlcpy(str_os, OS, sizeof(str_os));
    Q_strlcpy(str_arch, ARCH, sizeof(str_arch));
    
    fwrite(str_ver, sizeof(str_ver), 1, f);
    fwrite(str_game, sizeof(str_game), 1, f);
    fwrite(str_os, sizeof(str_os), 1, f);
    fwrite(str_arch, sizeof(str_arch), 1, f);
    
    game.autosaved = autosave;
    fwrite(&game, sizeof(game), 1, f);
    game.autosaved = false;
    
    for (i = 0; i < game.maxclients; i++)
    {
        WriteClient(f, &game.clients[i]);
    }
    
    fclose(f);
}

/*
 * Read the game structs from
 * a file. Called when ever a
 * savegames is loaded.
 */
void
ReadGame(const char *filename)
{
    FILE *f;
    int i;
    char str_ver[32];
    char str_game[32];
    char str_os[32];
    char str_arch[32];
    
    gi.FreeTags(TAG_GAME);
    
    f = fopen(filename, "rb");
    
    if (!f)
    {
        gi.error("Couldn't open %s", filename);
    }
    
    /* Sanity checks */
    fread(str_ver, sizeof(str_ver), 1, f);
    fread(str_game, sizeof(str_game), 1, f);
    fread(str_os, sizeof(str_os), 1, f);
    fread(str_arch, sizeof(str_arch), 1, f);
    
    if (strcmp(str_ver, SAVEGAMEVER))
    {
        fclose(f);
        gi.error("Savegame from an incompatible version.\n");
    }
    else if (strcmp(str_game, GAMEVERSION))
    {
        fclose(f);
        gi.error("Savegame from an other game.so.\n");
    }
    else if (strcmp(str_os, OS))
    {
        fclose(f);
        gi.error("Savegame from an other os.\n");
    }
    
    else if (strcmp(str_arch, ARCH))
    {
        fclose(f);
        gi.error("Savegame from an other architecure.\n");
    }
    
    g_edicts = gi.TagMalloc(game.maxentities * sizeof(g_edicts[0]), TAG_GAME);
    globals.edicts = g_edicts;
    
    fread(&game, sizeof(game), 1, f);
    game.clients = gi.TagMalloc(game.maxclients * sizeof(game.clients[0]),
                                TAG_GAME);
    
    for (i = 0; i < game.maxclients; i++)
    {
        ReadClient(f, &game.clients[i]);
    }
    
    fclose(f);
}

/* ========================================================== */

/*
 * Helper function to write the
 * edict into a file. Called by
 * WriteLevel.
 */
void
WriteEdict(FILE *f, edict_t *ent)
{
    field_t *field;
    edict_t temp;
    
    /* all of the ints, floats, and vectors stay as they are */
    temp = *ent;
    
    /* change the pointers to lengths or indexes */
    for (field = fields; field->name; field++)
    {
        WriteField1(f, field, (byte *)&temp);
    }
    
    /* write the block */
    fwrite(&temp, sizeof(temp), 1, f);
    
    /* now write any allocated data following the edict */
    for (field = fields; field->name; field++)
    {
        WriteField2(f, field, (byte *)ent);
    }
}

/*
 * Helper function to write the
 * level local data into a file.
 * Called by WriteLevel.
 */
void
WriteLevelLocals(FILE *f)
{
    field_t *field;
    level_locals_t temp;
    
    /* all of the ints, floats, and vectors stay as they are */
    temp = level;
    
    /* change the pointers to lengths or indexes */
    for (field = levelfields; field->name; field++)
    {
        WriteField1(f, field, (byte *)&temp);
    }
    
    /* write the block */
    fwrite(&temp, sizeof(temp), 1, f);
    
    /* now write any allocated data following the edict */
    for (field = levelfields; field->name; field++)
    {
        WriteField2(f, field, (byte *)&level);
    }
}

/*
 * Writes the current level
 * into a file.
 */
void
WriteLevel(const char *filename)
{
    int i;
    edict_t *ent;
    FILE *f;
    
    f = fopen(filename, "wb");
    
    if (!f)
    {
        gi.error("Couldn't open %s", filename);
    }
    
    /* write out edict size for checking */
    i = sizeof(edict_t);
    fwrite(&i, sizeof(i), 1, f);
    
    /* write out level_locals_t */
    WriteLevelLocals(f);
    
    /* write out all the entities */
    for (i = 0; i < globals.num_edicts; i++)
    {
        ent = &g_edicts[i];
        
        if (!ent->inuse)
        {
            continue;
        }
        
        fwrite(&i, sizeof(i), 1, f);
        WriteEdict(f, ent);
    }
    
    i = -1;
    fwrite(&i, sizeof(i), 1, f);
    
    fclose(f);
}

/* ========================================================== */

/*
 * A helper function to
 * read the edict back
 * into the memory. Called
 * by ReadLevel.
 */
void
ReadEdict(FILE *f, edict_t *ent)
{
    field_t *field;
    
    fread(ent, sizeof(*ent), 1, f);
    
    for (field = fields; field->name; field++)
    {
        ReadField(f, field, (byte *)ent);
    }
}

/*
 * A helper function to
 * read the level local
 * data from a file.
 * Called by ReadLevel.
 */
void
ReadLevelLocals(FILE *f)
{
    field_t *field;
    
    fread(&level, sizeof(level), 1, f);
    
    for (field = levelfields; field->name; field++)
    {
        ReadField(f, field, (byte *)&level);
    }
}

/*
 * Reads a level back into the memory.
 * SpawnEntities were already called
 * in the same way when the level was
 * saved. All world links were cleared
 * before this function was called. When
 * this function is called, no clients
 * are connected to the server.
 */
void
ReadLevel(const char *filename)
{
    int entnum;
    FILE *f;
    int i;
    edict_t *ent;
    
    f = fopen(filename, "rb");
    
    if (!f)
    {
        gi.error("Couldn't open %s", filename);
    }
    
    /* free any dynamic memory allocated by
     loading the level  base state */
    gi.FreeTags(TAG_LEVEL);
    
    /* wipe all the entities */
    memset(g_edicts, 0, game.maxentities * sizeof(g_edicts[0]));
    globals.num_edicts = maxclients->value + 1;
    
    /* check edict size */
    fread(&i, sizeof(i), 1, f);
    
    if (i != sizeof(edict_t))
    {
        fclose(f);
        gi.error("ReadLevel: mismatched edict size");
    }
    
    /* load the level locals */
    ReadLevelLocals(f);
    
    /* load all the entities */
    while (1)
    {
        if (fread(&entnum, sizeof(entnum), 1, f) != 1)
        {
            fclose(f);
            gi.error("ReadLevel: failed to read entnum");
        }
        
        if (entnum == -1)
        {
            break;
        }
        
        if (entnum >= globals.num_edicts)
        {
            globals.num_edicts = entnum + 1;
        }
        
        ent = &g_edicts[entnum];
        ReadEdict(f, ent);
        
        /* let the server rebuild world links for this ent */
        memset(&ent->area, 0, sizeof(ent->area));
        gi.linkentity(ent);
    }
    
    fclose(f);
    
    /* mark all clients as unconnected */
    for (i = 0; i < maxclients->value; i++)
    {
        ent = &g_edicts[i + 1];
        ent->client = game.clients + i;
        ent->client->pers.connected = false;
    }
    
    /* do any load time things at this point */
    for (i = 0; i < globals.num_edicts; i++)
    {
        ent = &g_edicts[i];
        
        if (!ent->inuse)
        {
            continue;
        }
        
        /* fire any cross-level triggers */
        if (ent->classname)
        {
            if (strcmp(ent->classname, "target_crosslevel_target") == 0)
            {
                ent->nextthink = level.time + ent->delay;
            }
        }
    }
}
