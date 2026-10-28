/* P_main.c */

#include "doomdef.h"
#include "p_local.h"
#include "mars.h"
#include "i_system.h"   /* Saturn-side: Saturn_StageColor / BootText /
                         * PumpFrame for the level-setup sub-stage markers
                         * below (decode table lives in main.c). */
#include "saturn_memmap.h"
#ifdef SATURN_WIN32
#include <stdio.h>
#endif

/* Saturn-side: invalidate the raycaster's per-level caches
 * (saturn_render.c). */
void Saturn_RenderInvalidateLevel(void);
/* Saturn-side: per-level PVS blob load (saturn_segs.c, P4). NULL
 * argument = drop the previous level's row pointer. */
void Saturn_PvsLevelLoad(const char *lumpname);
/* Saturn-side: per-level LEAF blob load (saturn_leaf.c, M2). NULL
 * argument = drop the previous level's blob pointers. */
void Saturn_LeafLevelLoad(const char *lumpname);
void Saturn_BakedFlats_Init(const char *lumpname);
void Saturn_BakedSegs_Init(const char *lumpname);
void Saturn_BakedTextures_Init(const char *lumpname);
void Saturn_BakedSprites_Init(const char *lumpname);
int  Saturn_Pak_LevelLoad(const char *lumpname);

#define MAX_AUX_TEXTURES 32

VINT		numvertexes;
mapvertex_t	*vertexes;

VINT		numsegs;
seg_t		*segs;

VINT		numsectors;
sector_t	*sectors;

VINT		numsubsectors;
subsector_t	*subsectors;

VINT		numnodes;
node_t		*nodes;

VINT		numlines;
line_t		*lines;

VINT 		numlinetags;
VINT 		*linetags; // [HASH_SIZE]{line,tag,line,tag...}

VINT 		numsectortags;
VINT 		*sectortags; // [HASH_SIZE]{sector,tag,sector,tag...}

VINT		numsides;
side_t		*sides;

short		*blockmaplump;			/* offsets in blockmap are from here */
VINT		bmapwidth, bmapheight;	/* in mapblocks */
fixed_t		bmaporgx, bmaporgy;		/* origin of block map */
SPTR		*blocklinks;			/* for thing chains */

byte		*rejectmatrix;			/* for fast sight rejection */

VINT 		segspage = -1;

mapthing_t	*deathmatchstarts, *deathmatch_p;

VINT		*validcount;			/* increment every time a check is made */

mapthing_t	playerstarts[MAXPLAYERS];

VINT		numthings;
spawnthing_t* spawnthings;

int16_t 	worldbbox[4];

/*
=================
=
= P_LoadVertexes
=
=================
*/

void P_LoadVertexes (int lump)
{
	byte		*data;
	int			i;
	mapvertex_t	*ml;
	mapvertex_t	*li;
	
	numvertexes = W_LumpLength (lump) / sizeof(mapvertex_t);
	vertexes = Z_Malloc (numvertexes*sizeof(mapvertex_t) + 16,PU_LEVEL);
	vertexes = (void*)(((uintptr_t)vertexes + 15) & ~15); // aline on cacheline boundary
	data = W_GetLumpData(lump);

	ml = (mapvertex_t *)data;
	li = vertexes;
	for (i=0 ; i<numvertexes ; i++, li++, ml++)
	{
		li->x = LITTLESHORT(ml->x);
		li->y = LITTLESHORT(ml->y);
	}

	worldbbox[BOXLEFT] = INT16_MAX;
	worldbbox[BOXRIGHT] = INT16_MIN;
	worldbbox[BOXBOTTOM] = INT16_MAX;
	worldbbox[BOXTOP] = INT16_MIN;

	li = vertexes;
	for (i=0 ; i<numvertexes ; i++, li++)
	{
		mapvertex_t *v = li;
		if (v->x < worldbbox[BOXLEFT])
			worldbbox[BOXLEFT] = v->x;
		if (v->x > worldbbox[BOXRIGHT])
			worldbbox[BOXRIGHT] = v->x;
		if (v->y < worldbbox[BOXBOTTOM])
			worldbbox[BOXBOTTOM] = v->y;
		if (v->y > worldbbox[BOXTOP])
			worldbbox[BOXTOP] = v->y;
	}
}


/*
=================
=
= P_LoadSegs
=
=================
*/

void P_LoadSegs (int lump)
{
	byte		*data;
	int			i;
	mapseg_t	*ml;
	seg_t		*li;
	int			linedef, side;

#ifdef USE_SMALL_LUMPS
	numsegs = W_LumpLength (lump) / sizeof(seg_t);

	// FIXME: this is strictly big-endian for now
	if (W_IsIWad(lump))
	{
		segs = W_POINTLUMPNUM(lump);
		return;
	}

	segs = Z_Malloc (numsegs*sizeof(seg_t)+16,PU_LEVEL);
	segs = (void*)(((uintptr_t)segs + 15) & ~15); // aline on cacheline boundary
	W_ReadLump(lump, segs);
	return;
#endif

	numsegs = W_LumpLength (lump) / sizeof(mapseg_t);
	segs = Z_Malloc (numsegs*sizeof(seg_t)+16,PU_LEVEL);
	segs = (void*)(((uintptr_t)segs + 15) & ~15); // aline on cacheline boundary
	D_memset (segs, 0, numsegs*sizeof(seg_t));
	data = W_GetLumpData(lump);

	ml = (mapseg_t *)data;
	li = segs;
	for (i=0 ; i<numsegs ; i++, li++, ml++)
	{
		li->v1 = (unsigned)LITTLESHORT(ml->v1);
		li->v2 = (unsigned)LITTLESHORT(ml->v2);

		linedef = (unsigned)LITTLESHORT(ml->linedef);

		li->linedef = linedef;

		side = (unsigned)LITTLESHORT(ml->side);

		SEG_PACK(li, side);
	}
}


/*
=================
=
= P_LoadSubsectors
=
=================
*/

void P_LoadSubsectors (int lump)
{
	byte			*data;
	int				i;
	int 			count;
	mapsubsector_t	*ms;
	subsector_t		*ss;

#ifdef USE_SMALL_LUMPS
	int16_t 		*cn;
	int 			curline;

	numsubsectors = W_LumpLength (lump) / sizeof(int16_t);
	count = numsubsectors+1;
	subsectors = Z_Malloc (count*sizeof(subsector_t),PU_LEVEL);
	data = W_GetLumpData(lump);

	cn = (int16_t *)data;
	D_memset (subsectors,0, count*sizeof(subsector_t));

	if (!numsubsectors)
		return;

	curline = 0;
	ss = subsectors;
	for (i=0 ; i<numsubsectors ; i++, ss++, cn++)
	{
		count = BIGSHORT(*cn);
		ss->firstline = curline;
		curline += count;
	}

	ss->firstline = curline;
	return;
#endif

	numsubsectors = W_LumpLength (lump) / sizeof(mapsubsector_t);
	count = numsubsectors+1;
	if ((size_t)count * sizeof(subsector_t) <= 0x4000u)
		subsectors = (subsector_t *)((uintptr_t)SATURN_WRAM_L_DIVLUT_ADDR + 0x2000u);
	else
		subsectors = Z_Malloc (count*sizeof(subsector_t),PU_LEVEL);
	data = W_GetLumpData(lump);

	ms = (mapsubsector_t *)data;
	D_memset (subsectors,0, count*sizeof(subsector_t));

	if (!numsubsectors)
		return;

	ss = subsectors;
	for (i=0 ; i<numsubsectors ; i++, ss++, ms++)
	{
		count = LITTLESHORT(ms->numsegs);
		ss->firstline = LITTLESHORT(ms->firstseg);
	}

	--ss;
	(ss + 1)->firstline = ss->firstline + count;
}


/*
=================
=
= P_LoadSectors
=
=================
*/

void P_LoadSectors (int lump)
{
	byte			*data;
	int				i;
	mapsector_t		*ms;
	sector_t		*ss;
	int 			tag;
			
	numsectors = W_LumpLength (lump) / sizeof(mapsector_t);
	if ((size_t)numsectors * sizeof(sector_t) <= SATURN_WRAM_L_SECSHADOW_SIZE)
	{
		sectors = (sector_t *)SATURN_WRAM_L_SECSHADOW_ADDR;
	}
	else
	{
		sectors = Z_Malloc (numsectors*sizeof(sector_t) + 16,PU_LEVEL);
		sectors = (void*)(((uintptr_t)sectors + 15) & ~15); // aline on cacheline boundary
	}
	D_memset (sectors, 0, numsectors*sizeof(sector_t));
	data = W_GetLumpData(lump);
	numsectortags = 0;

	ms = (mapsector_t *)data;
	ss = sectors;
	for (i=0 ; i<numsectors ; i++, ss++, ms++)
	{
		int lightlevel, special;

		ss->floorheight = LITTLESHORT(ms->floorheight)<<FRACBITS;
		ss->ceilingheight = LITTLESHORT(ms->ceilingheight)<<FRACBITS;
		ss->floorpic = R_FlatNumForName(ms->floorpic);
		if (!D_strncasecmp (ms->ceilingpic,"F_SKY1",6) )
			ss->ceilingpic = -1;
		else
		{
			ss->ceilingpic = R_FlatNumForName(ms->ceilingpic);
		}
		ss->thinglist = (SPTR)0;

		lightlevel = LITTLESHORT(ms->lightlevel);
		if (lightlevel < 0)
			lightlevel = 0;
		else if (lightlevel > 255)
			lightlevel = 255;
		ss->lightlevel = lightlevel;

		special = LITTLESHORT(ms->special);
		if (special < 0 || special > 255)
			special = 0;
		ss->special = special;

		tag = LITTLESHORT(ms->tag);
		if (tag != 0) {
			numsectortags++;
		}
	}

	// load tags into hash table

	if (!numsectortags)
		return;

	numsectortags = (numsectortags + LINETAGS_HASH_SIZE - 1) & ~(LINETAGS_HASH_SIZE - 1);
	sectortags = Z_Malloc(sizeof(*sectortags)*numsectortags*2, PU_LEVEL);
	D_memset(sectortags, -1, sizeof(*sectortags)*numsectortags*2);

	ms = (mapsector_t *)data;
	for (i=0 ; i<numsectors ; i++,  ms++)
	{
		tag = LITTLESHORT(ms->tag);
		if (tag) {
			P_SetSectorTag(i, tag);
		}
	}
}


/*
=================
=
= P_LoadNodes
=
=================
*/


static int P_EncodeBBoxSide(int16_t *b, int16_t *outerbbox, int pc, int nc)
{
	int length, unit;
	int nu, pu;

	length = outerbbox[pc] - outerbbox[nc] + 15;
	if (length < 16)
		return 0;
	unit = length / 16;

	// negative corner is increasing
	nu = 0;
	length = b[nc] - outerbbox[nc];
	if (length > 0) {
		nu = length / unit;
		if (nu > 15)
			nu = 15;
		b[nc] = outerbbox[nc] + nu * unit;
	}

	// positive corner is decreasing
	pu = 0;
	length = outerbbox[pc] - b[pc];
	if (length > 0) {
		pu = length / unit;
		if (pu > 15)
			pu = 15;
		b[pc] = outerbbox[pc] - pu * unit;
	}

	return (pu << 4) | nu;
}

// encodes bbox as the number of 1/16th units of parent bbox on each side
static int P_EncodeBBox(int16_t *cb, int16_t *outerbbox)
{
	int encbbox;
	encbbox = P_EncodeBBoxSide(cb, outerbbox, BOXRIGHT, BOXLEFT);
	encbbox <<= 8;
	encbbox |= P_EncodeBBoxSide(cb, outerbbox, BOXTOP, BOXBOTTOM);
	return encbbox;
}

static void P_EncodeNodeBBox_r(int nn, int16_t *bboxes, int16_t *outerbbox)
{
	int 		j;
	node_t 		*n;

	if (nn & NF_SUBSECTOR)
		return;
	if (nn < 0 || nn >= numnodes) {
#ifdef SATURN_WIN32
		printf("P_EncodeNodeBBox_r: invalid nn=%d (numnodes=%d)\n", nn, numnodes); fflush(stdout);
#endif
		return;
	}

	n = nodes + nn;
	for (j=0 ; j<2 ; j++)
	{
		int16_t *bbox = &bboxes[nn*8+j*4];
		n->encbbox[j] = P_EncodeBBox(bbox, outerbbox);
		P_EncodeNodeBBox_r(n->children[j], bboxes, bbox);
	}
}

// set the world's bounding box
// recursively encode bounding boxes for all BSP nodes
static void P_EncodeNodesBBoxes(int16_t *bboxes)
{
#ifdef SATURN_WIN32
	printf("P_EncodeNodesBBoxes: numnodes=%d, worldbbox=[%d,%d,%d,%d]\n",
		numnodes, worldbbox[0], worldbbox[1], worldbbox[2], worldbbox[3]);
	fflush(stdout);
#endif
	P_EncodeNodeBBox_r(numnodes-1, bboxes, worldbbox);
}

void P_LoadNodes (int lump)
{
	byte		*data;
	int			i,j,k;
	node_t		*no;
	mapnode_t 	*mn;
	int16_t 	*bboxes;

#ifdef USE_SMALL_LUMPS
	numnodes = W_LumpLength (lump) / sizeof(node_t);

	// FIXME: this is strictly big-endian for now
	if (W_IsIWad(lump))
	{
		int oldpage = 255;
		I_SetBankPage(oldpage);

		nodes = W_POINTLUMPNUM(lump);

		// re-use nodes from ROM if possible
		const int page = I_GetBankPage();
		if (page == oldpage)
		{
			// not in bank-switched area
			return;
		}

		if (segspage < 0 || page == segspage) {
			segspage = page;
			return;
		}
	}

	nodes = Z_Malloc (numnodes*sizeof(node_t) + 16,PU_LEVEL);
	nodes = (void*)(((uintptr_t)nodes + 15) & ~15); // aline on cacheline boundary
	W_ReadLump(lump, nodes);
	return;
#endif

	numnodes = W_LumpLength (lump) / sizeof(*mn);
#ifdef SATURN_WIN32
	printf("P_LoadNodes: lump=%d, numnodes=%d\n", lump, numnodes); fflush(stdout);
#endif
	data = W_GetLumpData(lump);
#ifdef SATURN_WIN32
	printf("P_LoadNodes: data=%p, allocating nodes\n", data); fflush(stdout);
#endif
	if ((size_t)numnodes * sizeof(node_t) <= 0x4000u)
	{
		nodes = (node_t *)((uintptr_t)SATURN_WRAM_L_SKY_ADDR + 0x8000u);
	}
	else
	{
		nodes = Z_Malloc (numnodes*sizeof(node_t) + 16,PU_LEVEL);
		nodes = (void*)(((uintptr_t)nodes + 15) & ~15); // aline on cacheline boundary
	}
	bboxes = (void *)I_FrameBuffer();

	extern int16_t (*hw_node_bboxes)[2][4];
	if ((size_t)numnodes * sizeof(int16_t) * 8 <= 0x4000u)
		hw_node_bboxes = (int16_t (*)[2][4])((uintptr_t)SATURN_WRAM_L_SKY_ADDR + 0xC000u);
	else
		hw_node_bboxes = Z_Malloc(numnodes * sizeof(int16_t) * 8, PU_LEVEL);

	mn = (mapnode_t *)data;
	no = nodes;
	for (i=0 ; i<numnodes ; i++, no++, mn++)
	{
		no->x = LITTLESHORT(mn->x);
		no->y = LITTLESHORT(mn->y);
		no->dx = LITTLESHORT(mn->dx);
		no->dy = LITTLESHORT(mn->dy);

		for (j=0 ; j<2 ; j++)
		{
			no->children[j] = LITTLESHORT(mn->children[j]);
			for (k=0 ; k<4 ; k++)
			{
				int16_t b = LITTLESHORT(mn->bbox[j][k]);
				bboxes[i*8+j*4+k] = b;
				if (hw_node_bboxes)
					hw_node_bboxes[i][j][k] = b;
			}
		}
	}

#ifdef SATURN_WIN32
	printf("P_LoadNodes: node loop finished. Calling P_EncodeNodesBBoxes\n"); fflush(stdout);
#endif
	P_EncodeNodesBBoxes(bboxes);
#ifdef SATURN_WIN32
	printf("P_LoadNodes: P_EncodeNodesBBoxes done\n"); fflush(stdout);
#endif
}

/*
=================
=
= P_LoadThings
=
=================
*/

void P_LoadThings (int lump, boolean *havebossspit)
{
	byte			*data;
	int				i;
	mapthing_t		*mt;
	spawnthing_t	*st;
	int 			numthingsreal, numstaticthings;

	numthings = W_LumpLength (lump) / sizeof(mapthing_t);
#ifdef SATURN_WIN32
	printf("P_LoadThings: lump=%d, numthings=%d\n", lump, numthings); fflush(stdout);
#endif
	data = I_TempBuffer(numthings*sizeof(mapthing_t));
	W_ReadLump(lump, data);
	numthingsreal = 0;
	numstaticthings = 0;
	*havebossspit = false;

	I_SetBankPage(segspage);

	mt = (mapthing_t *)data;
	for (i=0 ; i<numthings ; i++, mt++)
	{
		mt->x = LITTLESHORT(mt->x);
		mt->y = LITTLESHORT(mt->y);
		mt->angle = LITTLESHORT(mt->angle);
		mt->type = LITTLESHORT(mt->type);
		mt->options = LITTLESHORT(mt->options);
	}

	if (netgame == gt_deathmatch)
	{
		spawnthings = Z_Malloc(numthings * sizeof(*spawnthings), PU_LEVEL);
		st = spawnthings;
		mt = (mapthing_t*)data;
		for (i = 0; i < numthings; i++, mt++)
		{
			st->x = mt->x, st->y = mt->y;
			st->angle = mt->angle;
			st->type = mt->type;
			st++;
		}
	}

	mt = (mapthing_t *)data;
	for (i=0 ; i<numthings ; i++, mt++)
	{
		switch (P_MapThingSpawnsMobj(mt))
		{
			case 0:
				break;
			case 1:
				if (mt->type == mobjinfo[MT_BOSSSPIT].doomednum)
					*havebossspit = true;
				numthingsreal++;
				break;
			case 2:
				numstaticthings++;
				break;
		}
	}

	// preallocate a few mobjs for puffs and projectiles
	numthingsreal += 40;
#ifdef SATURN_WIN32
	printf("P_LoadThings: calling P_PreSpawnMobjs(real=%d, static=%d)\n", numthingsreal, numstaticthings); fflush(stdout);
#endif
	P_PreSpawnMobjs(numthingsreal, numstaticthings);

#ifdef SATURN_WIN32
	printf("P_LoadThings: calling P_SpawnMapThing loop (numthings=%d)\n", numthings); fflush(stdout);
#endif
	mt = (mapthing_t *)data;
	for (i=0 ; i<numthings ; i++, mt++)
	{
#ifdef SATURN_WIN32
		if (i >= 80) {
			printf("P_LoadThings: thing %d: type=%d at (%d, %d)\n", i, mt->type, mt->x, mt->y);
			fflush(stdout);
		}
#endif
		P_SpawnMapThing (mt, i);
	}
#ifdef SATURN_WIN32
	printf("P_LoadThings: P_SpawnMapThing loop done\n"); fflush(stdout);
#endif
}

/*
=================
=
= P_LoadLineDefs
=
= Also counts secret lines for intermissions
=================
*/

void P_LoadLineDefs (int lump)
{
	byte			*data;
	int				i;
	maplinedef_t	*mld;
	line_t			*ld;

	numlines = W_LumpLength (lump) / sizeof(maplinedef_t);
	lines = Z_Malloc (numlines*sizeof(line_t)+16,PU_LEVEL);
	lines = (void*)(((uintptr_t)lines + 15) & ~15); // aline on cacheline boundary
	D_memset (lines, 0, numlines*sizeof(line_t));
	data = W_GetLumpData(lump);
	numlinetags = 0;

	mld = (maplinedef_t *)data;
	ld = lines;
	for (i=0 ; i<numlines ; i++, mld++, ld++)
	{
		int tag;

		ld->flags = LITTLESHORT(mld->flags);
		ld->special = LITTLESHORT(mld->special);
		tag = LITTLESHORT(mld->tag);
		ld->v1 = LITTLESHORT(mld->v1);
		ld->v2 = LITTLESHORT(mld->v2);

		ld->sidenum[0] = LITTLESHORT(mld->sidenum[0]);
		ld->sidenum[1] = LITTLESHORT(mld->sidenum[1]);

		if (tag)
			numlinetags++;

		// HACK HACK HACK
		// backwards compatibility with JagDoom specials
		// that use values typically reserved for Doom2 maps
		if (tag)
			continue;

		switch (ld->special)
		{
		case 99:
			/* Blue Skull Door Open */
			ld->special = 32;
			break;
		case 100:
			/* Red Skull Door Open */
			ld->special = 33;
			break;
		case 105:
			/* Yellow Skull Door Open */
			ld->special = 34;
			break;
		case 106:
			/* Blue Skull Door Raise */
		case 107:
			/* Red Skull Door Raise */
		case 108:
			/* Yellow Skull Door Raise */
			ld->special = ld->special - 80;
			break;
		}
	}

	// load tags into hash table

	if (!numlinetags)
		return;

	numlinetags = (numlinetags + LINETAGS_HASH_SIZE - 1) & ~(LINETAGS_HASH_SIZE - 1);
	linetags = Z_Malloc(sizeof(*linetags)*numlinetags*2, PU_LEVEL);
	D_memset(linetags, -1, sizeof(*linetags)*numlinetags*2);

	mld = (maplinedef_t *)data;
	for (i=0 ; i<numlines ; i++, mld++)
	{
		int tag = LITTLESHORT(mld->tag);
		if (tag) {
			P_SetLineTag(i, tag);
		}
	}
}


/*
=================
=
= P_LoadSideDefs
=
=================
*/

void P_LoadSideDefs (int lump)
{
	byte			*data;
	int				i;
	mapsidedef_t	*msd;
	side_t			*sd;

#ifndef MARS
	for (i=0 ; i<numtextures ; i++)
		textures[i].usecount = 0;
#endif	
	numsides = W_LumpLength (lump) / sizeof(mapsidedef_t);
	sides = Z_Malloc (numsides*sizeof(side_t)+16,PU_LEVEL);
	sides = (void*)(((uintptr_t)sides + 15) & ~15); // aline on cacheline boundary
	D_memset (sides, 0, numsides*sizeof(side_t));
	data = W_GetLumpData(lump);

	msd = (mapsidedef_t *)data;
	sd = sides;
	for (i=0 ; i<numsides ; i++, msd++, sd++)
	{
		int rowoffset, textureoffset;
		textureoffset = LITTLESHORT(msd->textureoffset);
		rowoffset = LITTLESHORT(msd->rowoffset);
		sd->toptexture = R_TextureNumForName(msd->toptexture);
		sd->bottomtexture = R_TextureNumForName(msd->bottomtexture);
		sd->midtexture = R_TextureNumForName(msd->midtexture);
		sd->sector = LITTLESHORT(msd->sector);
		sd->rowoffset = rowoffset & 0xff;
		sd->textureoffset = (textureoffset & 0xfff) | ((rowoffset & 0x0f00) << 4);
#ifndef MARS
		textures[sd->toptexture].usecount++;
		textures[sd->bottomtexture].usecount++;
		textures[sd->midtexture].usecount++;
#endif
	}
}


/*
=================
=
= P_LoadRejectMatrix
=
=================
*/

void P_LoadRejectMatrix (int lump)
{
	int i, j;
	uint8_t *data, *out;
	int outsize;
	unsigned outbit, outbyte;

	// compress the reject table, assuming it is symmetrical
	outsize = (numsectors + 1) * numsectors / 2;
	outsize = (outsize + 7) / 8;

	rejectmatrix = Z_Malloc (outsize,PU_LEVEL);
	D_memset(rejectmatrix, 0, outsize);
	data = W_GetLumpData(lump);

	outbit = 1;
	outbyte = 0;
	out = rejectmatrix;

	for (i = 0; i < numsectors; i++) {
		unsigned k = i*numsectors;
		unsigned bit = 1 << ((k + i) & 7);

		for (j = i; j < numsectors; j++) {
			unsigned bytenum = (k + j) / 8;

			if (data[bytenum] & bit)
			{
				out[outbyte] |= outbit;
			}

			bit <<= 1;
			if (bit > 0xff) {
				bit = 1;
			}

			outbit <<= 1;
			if (outbit > 0xff) {
				outbyte++;
				outbit = 1;
			}
		}
	}
}

/*
=================
=
= P_LoadBlockMap
=
=================
*/

void P_LoadBlockMap (int lump)
{
	int		i;
	int		count;

	blockmaplump = Z_Malloc (W_LumpLength (lump),PU_LEVEL);
	W_ReadLump (lump,blockmaplump);
	count = W_LumpLength (lump)/2;
	for (i=0 ; i<count ; i++)
		blockmaplump[i] = LITTLESHORT(blockmaplump[i]);

	bmaporgx = blockmaplump[0]<<FRACBITS;
	bmaporgy = blockmaplump[1]<<FRACBITS;
	bmapwidth = blockmaplump[2];
	bmapheight = blockmaplump[3];

/* clear out mobj chains */
	count = sizeof(*blocklinks)* bmapwidth*bmapheight;
	blocklinks = Z_Malloc (count,PU_LEVEL);
	D_memset (blocklinks, 0, count);
}




/*
=================
=
= P_GroupLines
=
= Builds sector line lists and subsector sector numbers
= Finds block bounding boxes for sectors
=================
*/

void P_GroupLines (void)
{
	VINT		*linebuffer;
	int			i, total;
	sector_t	*sector;
	subsector_t	*ss;
	seg_t		*seg;
	line_t		*li;

	I_SetBankPage(segspage);

/* look up sector number for each subsector */
	ss = subsectors;
	for (i=0 ; i<numsubsectors ; i++, ss++)
	{
		int side;
		side_t *sidedef;
		line_t* linedef;

		seg = &segs[ss->firstline];
		linedef = &lines[SEG_UNPACK_LINEDEF(seg)];
		side = SEG_UNPACK_SIDE(seg);
		sidedef = &sides[linedef->sidenum[side]];
		if (side == 1)
		{
			// set the guard flag for the back side, otherwise it
			// may be set to -1 in the loop below
			linedef->flags |= ML_TWOSIDED;
		}
		ss->sector = sidedef->sector;
	}

	/* if the two-sided flag isn't set, set the back side to -1 */
	li = lines;
	for (i=0 ; i<numlines ; i++, li++)
	{
		if (li->sidenum[1] >= 0 && !(li->flags & ML_TWOSIDED)) {
			li->sidenum[1] = -1;
		}
		li->flags &= ~ML_TWOSIDED;
	}

/* count number of lines in each sector */
	li = lines;
	total = 0;
	for (i=0 ; i<numlines ; i++, li++)
	{
		sector_t *front = LD_FRONTSECTOR(li);
		sector_t *back = LD_BACKSECTOR(li);

		front->linecount++;
		total++;
		if (back && back != front)
		{
			back->linecount++;
			total++;
		}
	}
	
/* build line tables for each sector	 */
	linebuffer = Z_Malloc (total*sizeof(*linebuffer), PU_LEVEL);
	sector = sectors;
	for (i=0 ; i<numsectors ; i++, sector++)
	{
		sector->lines = linebuffer;
		linebuffer += sector->linecount;
		sector->linecount = 0;
	}

	li = lines;
	for (i=0 ; i<numlines ; i++, li++)
	{
		sector_t *front = LD_FRONTSECTOR(li);
		sector_t *back = LD_BACKSECTOR(li);

		front->lines[front->linecount++] = i;
		if (back && back != front)
		{
			back->lines[back->linecount++] = i;
		}
	}
}

/*============================================================================= */


/*
=================
=
= P_LoadingPlaque
=
=================
*/

void P_LoadingPlaque (void)
{
#ifndef MARS
	jagobj_t	*pl;

	pl = W_CacheLumpName ("loading", PU_STATIC);	
	DrawPlaque (pl);
	Z_Free (pl);
#endif
}	

/*============================================================================= */

static void P_isortsegs(seg_t * segs, int len)
{
	int i, j;
	for (i = 1; i < len; i++)
	{
		seg_t t;
		t.linedef = segs[i].linedef;
		t.v1 = segs[i].v1;
		t.v2 = segs[i].v2;
		for (j = i - 1; j >= 0; j--)
		{
			if (segs[j].linedef <= t.linedef)
				break;
			segs[j+1].linedef = segs[j].linedef;
			segs[j+1].v1 = segs[j].v1;
			segs[j+1].v2 = segs[j].v2;
		}
		segs[j+1].linedef = t.linedef;
		segs[j+1].v1 = t.v1;
		segs[j+1].v2 = t.v2;
	}
}

// Sorts segments in subsectors by linedef in ascending order
static void P_SortSegs(void)
{
	int i;

	for (i = 0; i < numsubsectors; i++)
	{
		subsector_t *sub = &subsectors[i];
		int count = SSEC_NUMLINES(sub);
		int firstline = sub->firstline;
		P_isortsegs(&segs[firstline], count);
	}
}

/*
=================
=
= P_SetupLevel
=
=================
*/
void P_SetupLevel (const char *lumpname, skill_t skill, const char *sky)
{
	int i;
#ifndef MARS
	mobj_t	*mobj;
#endif
	extern VINT	cy;
	VINT lumpnum, lumps[ML_BLOCKMAP+1+MAX_AUX_TEXTURES];
	lumpinfo_t li[ML_BLOCKMAP+1+MAX_AUX_TEXTURES];
	int skytexture;
	boolean havebossspit = false;
	int gamezonemargin;
	jagobj_t *skyheader;
	boolean havesky;

	M_ClearRandom ();
#ifdef SATURN_WIN32
	printf("P_SetupLevel: clearing PVS/LEAF\n"); fflush(stdout);
#endif

	/* P4: drop the PREVIOUS level's PVS row pointer first thing — the
	 * blob's zone block died with the last Z_FreeTags(PU_LEVEL) and no
	 * frame may dereference it. (The fresh load happens after the
	 * nine map-struct loads, once numsubsectors/numnodes/numsegs are
	 * final.) Same for the M2 LEAF blob's pointers. */
	Saturn_PvsLevelLoad(NULL);
	Saturn_LeafLevelLoad(NULL);

	/* r67 init breadcrumbs (saturn_init_site, decode in g_game.c) â€”
	 * the real-HW wedge sits somewhere in this pre-DARK-YELLOW
	 * window with no stamped GFS call in flight. */
	{
		extern volatile unsigned int saturn_init_site;

		saturn_init_site = 3;
		Saturn_GsubStamp(23);
	}

	Saturn_StageColor(JO_COLOR_SATURN_RGB(0, 31, 12));
	P_LoadingPlaque ();

#ifdef SATURN_WIN32
	printf("P_SetupLevel: P_LoadingPlaque done. Calling P_InitThinkers\n"); fflush(stdout);
#endif
D_printf ("P_SetupLevel(%s,%i)\n",lumpname,skill);

	Saturn_StageColor(JO_COLOR_SATURN_RGB(31, 0, 18));
	P_InitThinkers ();
#ifdef SATURN_WIN32
	printf("P_SetupLevel: P_InitThinkers done. Calling R_ClearTextures\n"); fflush(stdout);
#endif

	{
		extern volatile unsigned int saturn_init_site;

		saturn_init_site = 4;
		Saturn_GsubStamp(24);
	}

	Saturn_StageColor(JO_COLOR_SATURN_RGB(10, 0, 31));
	R_ClearTextures();
#ifdef SATURN_WIN32
	printf("P_SetupLevel: R_ClearTextures done. Calling W_LoadPWAD\n"); fflush(stdout);
#endif

	/* r95b: post-R_ClearTextures split â€” VIOLET's window held both the
	 * call and the site-5/sky-default lines. WHITE now covers only the
	 * site-5 GsubStamp + sky-default block; with the bounded StageColor
	 * hold, a frozen VIOLET can only be R_ClearTextures itself and a
	 * frozen WHITE the block after it. (WHITE is transiently used by
	 * the early-boot report stage â€” in THIS window a frozen white is
	 * unambiguous.) */
	Saturn_StageColor(JO_COLOR_White);

	{
		extern volatile unsigned int saturn_init_site;

		saturn_init_site = 5;
		Saturn_GsubStamp(25);
	}

	if (!sky || !*sky) {
		sky = "SKY1";
	}

	skyheader = NULL;
	skytexturep = NULL;
	skytexture = -1;
	skycolormaps = NULL;

	Saturn_StageColor(JO_COLOR_SATURN_RGB(10, 31, 0));
	W_LoadPWAD(PWAD_CD);
#ifdef SATURN_WIN32
	printf("P_SetupLevel: W_LoadPWAD done. Checking lump %s\n", lumpname); fflush(stdout);
#endif

	{
		extern volatile unsigned int saturn_init_site;

		saturn_init_site = 6;
		Saturn_GsubStamp(26);
	}

	Saturn_StageColor(JO_COLOR_SATURN_RGB(31, 14, 6));
	lumpnum = W_CheckNumForName(lumpname);
#ifdef SATURN_WIN32
	printf("P_SetupLevel: W_CheckNumForName(%s) = %d\n", lumpname, lumpnum); fflush(stdout);
#endif

	{
		extern volatile unsigned int saturn_init_site;

		saturn_init_site = 7;
		Saturn_GsubStamp(27);
	}
	if (lumpnum < 0)
	{
		/* Diagnostic edition of the error: verdicts in LETTERS ONLY
		 * (OK/NO tokens â€” readable even if digits never render), a
		 * literal 0123456789 line as a font digit probe, and three
		 * numeric bonus lines that carry exact values when digits DO
		 * render. Lines are <=15 chars (the 2x font pitch is 20 px,
		 * 15 columns from x=8). Cross-reads:
		 *   FMT=NO          Saturn_Format's %d is broken (the root
		 *                   cause if PICK=NO too â€” pick_lump_name
		 *                   used to depend on it)
		 *   PICK=NO,FMT=OK  the hand-built name path broke (should be
		 *                   impossible after the 2026-08-30 rewrite)
		 *   LIT=NO,ENT=OK   a literal "E1M1" lookup fails on a good
		 *                   directory entry -> scan range (see CNT=)
		 *   CNT=NO          numlumps != 2306 (directory truncation)
		 *   ENT=NO          entry 7's name isn't E1M1 (contents)
		 *   POS=NO          entry 7 filepos != 0xE474 (COLD_BIAS
		 *                   masked out; a hot-cart rewrite reads NO)
		 * The 0123456789 line crossing against FMT= splits font
		 * trouble (digits absent there too) from formatter trouble
		 * (digits visible but %d empty). */
		lumpinfo_t *li7 = &wadfile[0].lumpinfo[7];
		char        fmtbuf[16];
		int         idx     = W_CheckNumForName("E1M1");
		int         pick_ok, fmt_ok, lit_ok, cnt_ok, ent_ok, pos_ok;
		Saturn_Format(fmtbuf, (int)sizeof(fmtbuf), "E%dM%d", 1, 1);
		pick_ok = (lumpname[0]=='E' && lumpname[1]=='1' && lumpname[2]=='M'
		           && lumpname[3]=='1' && lumpname[4]==0);
		fmt_ok  = (fmtbuf[0]=='E' && fmtbuf[1]=='1' && fmtbuf[2]=='M'
		           && fmtbuf[3]=='1' && fmtbuf[4]==0);
		lit_ok  = (idx >= 0);
		cnt_ok  = (wadfile[0].numlumps == 2306);
		ent_ok  = (li7->name[0]=='E' && li7->name[1]=='1' && li7->name[2]=='M'
		           && li7->name[3]=='1' && li7->name[4]==0);
		pos_ok  = (((unsigned)li7->filepos & 0x7fffffffu) == 0xe474u);
		/* 8 lines max, <=15 chars each, commas/dashes not spaces (the
		 * font has no space glyph), 2x scale in I_Error's draw loop.
		 * Expected: NOTFOUND-E1M1, all six verdicts OK, NLUMPS=2306.902,
		 * IDX=7, E7=45314D31.0. */
		I_Error("NOTFOUND-%s\n0123456789\nPICK=%s,FMT=%s\nLIT=%s,CNT=%s\nENT=%s,POS=%s\nNLUMPS=%d.%X\nIDX=%d\nE7=%X.%X",
			lumpname,
			pick_ok?"OK":"NO", fmt_ok?"OK":"NO",
			lit_ok?"OK":"NO", cnt_ok?"OK":"NO",
			ent_ok?"OK":"NO", pos_ok?"OK":"NO",
			wadfile[0].numlumps, wadfile[0].numlumps, idx,
			*(int *)&li7->name[0], *(int *)&li7->name[4]);
	}

	/* Saturn debug sub-stage (decode table in main.c): DARK YELLOW =
	 * the map lump was found in the directory; the P_Load* chain and
	 * everything above this line are done. The r95 wedge ladder above
	 * front-runs each pre-map call with its own colour â€” frozen TEAL/
	 * PINK/VIOLET/CHARTREUSE/SALMON names the exact call (decode in
	 * main.c); still CYAN = wedge before P_LoadingPlaque. */
	Saturn_StageColor(JO_COLOR_DarkYellow);
	Saturn_SetupTag( "MAP-FOUND");
	Saturn_PumpFrame();
	Saturn_GsubStamp(28);

	/* build a temp in-memory PWAD */
	for (i = 0; i < ML_BLOCKMAP+1+MAX_AUX_TEXTURES; i++)
		lumps[i] = lumpnum+i;

#ifdef SATURN_WIN32
	printf("P_SetupLevel: calling W_CacheWADLumps\n"); fflush(stdout);
#endif
	W_CacheWADLumps(li, ML_BLOCKMAP+1+MAX_AUX_TEXTURES, lumps, true);

	lumpnum = W_GetNumForName(lumpname);
#ifdef SATURN_WIN32
	printf("P_SetupLevel: W_GetNumForName(%s) = %d. Loading map lumps\n", lumpname, lumpnum); fflush(stdout);
#endif

#if defined(USE_SMALL_LUMPS) && defined(ENABLE_SSF_MAPPER)
	if (W_IsIWad(lumpnum+ML_SEGS))
	{
		int oldpage = 255;
		I_SetBankPage(oldpage);

		W_POINTLUMPNUM(lumpnum+ML_SEGS);

		segspage = I_GetBankPage();
		if (segspage == oldpage) {
			// not in bank-switched area
			segspage = -1;
		}
	}
	else
	{
		segspage = -1;
	}
#endif

/* note: most of this ordering is important	 */
	/* Saturn debug sub-stage: MEDIUM PURPLE + the lump name while each map
	 * struct load runs (each is a COLD lump streamed from CD through the
	 * i_wad.c shims, so any one of them can hang in GFS). A hang leaves
	 * the guilty lump's tag on screen â€” e.g. MEDIUM PURPLE + "LOAD-SEGS"
	 * means the SEGS pread never came back. */
	Saturn_StageColor(JO_COLOR_MediumPurple);
	{
		static const char *const tags[] = {
			"LOAD-LINEDEFS", "LOAD-SIDEDEFS", "LOAD-VERTEXES",
			"LOAD-SEGS", "LOAD-SSECTORS", "LOAD-NODES",
			"LOAD-SECTORS", "LOAD-REJECT", "LOAD-BLOCKMAP"
		};
		int t;
		for (t = 0; t < 9; t++)
		{
			Saturn_SetupTag( tags[t]);
			Saturn_PumpFrame();
#ifdef SATURN_WIN32
			printf("P_SetupLevel: loading tag %s\n", tags[t]); fflush(stdout);
#endif
			switch (t)
			{
				case 0: P_LoadLineDefs (lumpnum+ML_LINEDEFS); break;
				case 1: P_LoadSideDefs (lumpnum+ML_SIDEDEFS); break;
				case 2: P_LoadVertexes (lumpnum+ML_VERTEXES); break;
				case 3: P_LoadSegs (lumpnum+ML_SEGS); break;
				case 4: P_LoadSubsectors (lumpnum+ML_SSECTORS); break;
				case 5: P_LoadNodes (lumpnum+ML_NODES); break;
				case 6: P_LoadSectors (lumpnum+ML_SECTORS); break;
				case 7: P_LoadRejectMatrix (lumpnum+ML_REJECT); break;
				case 8: P_LoadBlockMap (lumpnum+ML_BLOCKMAP); break;
			}
		}
	}

	/* P4: load the E1M1 PVS blob now that numsubsectors/numnodes/
	 * numsegs are final (it validates its own counts against them).
	 * Any failure leaves the seg renderer's PVS gate off = the plain
	 * BSP walk. */
	Saturn_SetupTag( "PAK-LOAD");
	Saturn_PumpFrame();
#ifdef SATURN_WIN32
	printf("P_SetupLevel: loading PAK/baked files for %s\n", lumpname); fflush(stdout);
#endif
	if (!Saturn_Pak_LevelLoad(lumpname))
	{
		/* Fallback to individual loose files if PAK is absent */
		Saturn_SetupTag( "PVS-LOAD");
		Saturn_PumpFrame();
#ifdef SATURN_WIN32
		printf("P_SetupLevel: Saturn_PvsLevelLoad\n"); fflush(stdout);
#endif
		Saturn_PvsLevelLoad(lumpname);

		Saturn_SetupTag( "LEAF-LOAD");
		Saturn_PumpFrame();
#ifdef SATURN_WIN32
		printf("P_SetupLevel: Saturn_LeafLevelLoad\n"); fflush(stdout);
#endif
		Saturn_LeafLevelLoad(lumpname);
#ifdef SATURN_WIN32
		printf("P_SetupLevel: Saturn_BakedFlats_Init\n"); fflush(stdout);
#endif
		Saturn_BakedFlats_Init(lumpname);
#ifdef SATURN_WIN32
		printf("P_SetupLevel: Saturn_BakedSegs_Init\n"); fflush(stdout);
#endif
		Saturn_BakedSegs_Init(lumpname);
#ifdef SATURN_WIN32
		printf("P_SetupLevel: Saturn_BakedTextures_Init\n"); fflush(stdout);
#endif
		Saturn_BakedTextures_Init(lumpname);
#ifdef SATURN_WIN32
		printf("P_SetupLevel: Saturn_BakedSprites_Init\n"); fflush(stdout);
#endif
		Saturn_BakedSprites_Init(lumpname);
	}
#ifdef SATURN_WIN32
	printf("P_SetupLevel: Baked files loaded. Allocating validcount\n"); fflush(stdout);
#endif

	if ((size_t)(numlines + 1) * sizeof(*validcount) <= 0x2000u)
		validcount = (VINT *)((uintptr_t)SATURN_WRAM_L_DIVLUT_ADDR + 0x6000u);
	else
		validcount = Z_Malloc((numlines + 1) * sizeof(*validcount), PU_LEVEL);
	D_memset(validcount, 0, (numlines + 1) * sizeof(*validcount));
	validcount[0] = 1; // cpu 0

#ifdef SATURN_WIN32
	printf("P_SetupLevel: validcount allocated (numlines=%d). Calling P_GroupLines\n", numlines); fflush(stdout);
#endif
	// G_DeathMatchSpawnPlayer is going to need this
	I_SetThreadLocalVar(DOOMTLS_VALIDCOUNT, &validcount[0]);

	/* Saturn debug sub-stage: SEMI GREEN = all nine map-struct loads
	 * returned (every COLD lump streamed off the CD OK). */
	Saturn_StageColor(JO_COLOR_SemiGreen);
	Saturn_SetupTag( "MAP-DATA-OK");
	Saturn_PumpFrame();

	P_GroupLines ();
#ifdef SATURN_WIN32
	printf("P_SetupLevel: P_GroupLines done. Calling P_SortSegs\n"); fflush(stdout);
#endif

	/* Saturn debug sub-stage: SEMI YELLOW = P_GroupLines done (BSP line
	 * grouping / bounding boxes â€” CPU-only, hangs here mean bad linedef
	 * or sidedef data). */
	Saturn_StageColor(JO_COLOR_SemiYellow);
	Saturn_SetupTag( "GROUPLINES-OK");
	Saturn_PumpFrame();

	P_SortSegs();
#ifdef SATURN_WIN32
	printf("P_SetupLevel: P_SortSegs done. Calling P_LoadThings\n"); fflush(stdout);
#endif

	/* Saturn debug sub-stage: MEDIUM GREEN = P_SortSegs done. */
	Saturn_StageColor(JO_COLOR_MediumGreen);
	Saturn_SetupTag( "SORTSEGS-OK");
	Saturn_PumpFrame();

	if (netgame == gt_deathmatch)
	{
		itemrespawnque = Z_Malloc(sizeof(*itemrespawnque) * ITEMQUESIZE, PU_LEVEL);
		itemrespawntime = Z_Malloc(sizeof(*itemrespawntime) * ITEMQUESIZE, PU_LEVEL);
		deathmatchstarts = Z_Malloc(sizeof(*deathmatchstarts) * MAXDMSTARTS, PU_LEVEL);
	}
	else
	{
		itemrespawnque = NULL;
		itemrespawntime = NULL;
		deathmatchstarts = NULL;
	}

	havesky = false;
	for (i = 0; i < numsectors; i++) {
		if (sectors[i].ceilingpic == -1) {
			havesky = true;
			break;
		}
	}

	bodyqueslot = 0;
	deathmatch_p = deathmatchstarts;
	P_LoadThings (lumpnum+ML_THINGS, &havebossspit);
#ifdef SATURN_WIN32
	printf("P_SetupLevel: P_LoadThings done\n"); fflush(stdout);
#endif

	/* Saturn debug sub-stage: GRAY = P_LoadThings done â€” every THINGS
	 * entry parsed and every mobj spawned. */
	Saturn_StageColor(JO_COLOR_Gray);
	Saturn_SetupTag( "THINGS-OK");
	Saturn_PumpFrame();

	// load custom replacement textures from CD to RAM
	{
		boolean istexture = false;
		boolean isflat = false;

		for (i = 0; i < MAX_AUX_TEXTURES; i++)
		{
			int j, k;
			uint8_t *data;
			lumpinfo_t *l;
			char name[9];

			k = ML_BLOCKMAP + 1 + i;
			l = &li[k];
			if (!l->name[0])
				break;

			D_memcpy(name, l->name, 8);
			name[0] = name[0] & ~0x80;
			name[8] = 0;

			if (!l->size)
			{
				if (!istexture && !D_strcasecmp(name, "T_START"))
					istexture = true;
				else if (istexture && !D_strcasecmp(name, "T_END"))
					istexture = false;
				else if (!istexture && !D_strcasecmp(name, "F_START"))
					isflat = true;
				else if (isflat && !D_strcasecmp(name, "F_END"))
					isflat = false;
				else
					break;
				continue;
			}

			// check if it's a texture
			if (istexture) {
				j = R_CheckTextureNumForName (name);
				if (j >= 0)
				{
					int n;
					int lumpnum2 = textures[j].lumpnum;
					data = Z_Malloc (l->size, PU_LEVEL);
					W_ReadLump(lumpnum+k, data);
					for (n = 0; n < numtextures; n++)
					{
						if (textures[n].lumpnum == lumpnum2)
							R_SetTextureData(&textures[n], data, l->size, true);
					}
				}
				continue;
			}

			// a flat?
			if (isflat) {
				j = R_FlatNumForName(name);
				if (j >= 0) {
					data = Z_Malloc (l->size, PU_LEVEL);
					W_ReadLump(lumpnum + k, data);
					R_SetFlatData(j, data, l->size);
					continue;
				}
			}

			// a sky?
			if (havesky && !D_strcasecmp(name, sky)) {
				data = Z_Malloc (l->size, PU_LEVEL);
				W_ReadLump(lumpnum + k, data);
				skytexturep = R_SkipJagObjHeader(data, l->size, 256, 128, &skyheader);
				continue;
			}
		}
	}

	W_LoadPWAD(PWAD_NONE);

	if (havesky && !skytexturep) {
		skytexture = W_CheckNumForName(sky);
		if (skytexture == -1) {
			sky = "SKY1";
			skytexture = W_CheckNumForName(sky);
		}

		if (skytexture != -1) {
			if (W_IsCompressed(skytexture))
			{
				skytexturep = Z_Malloc (W_LumpLength(skytexture),PU_LEVEL);
				W_ReadLump(skytexture, skytexturep);
			}
			else
				/* Saturn: the sky lump is often COLD â€” W_POINTLUMPNUM
				 * would alias the shared CD scratch, so keep a stable
				 * copy instead. */
				skytexturep = I_PointStableLump(skytexture);

			skydepth = 3;
			skytexturep = R_SkipJagObjHeader(skytexturep, W_LumpLength(skytexture), 256, 128, &skyheader);
		}
	}

	if (skyheader && BIGSHORT(skyheader->depth) == 2) {
		// 4-bit sky has a custom colormap appended at the end of the file
		skydepth = 2;
		skycolormaps = (int8_t *)skytexturep + 64*256;
	}

	/* Saturn debug sub-stage: DARK GREEN = the aux-texture / sky-lump
	 * post-pass finished (any cold reads for replacement textures and the
	 * sky resolved). Next is R_FixupTextures + specials + ST_Init. */
	Saturn_StageColor(JO_COLOR_DarkGreen);
	Saturn_SetupTag( "SKY-OK");
	Saturn_PumpFrame();

	/* Sub-stage: MEDIUM BLUE = entering R_FixupTextures (texture loop;
	 * its own MEDIUM PURPLE FIX-FLAT marker fires between the texture
	 * and flat loops). Persistent MEDIUM BLUE = the hang is in the
	 * texture loop of R_FixupTextures. */
	Saturn_StageColor(JO_COLOR_MediumBlue);
	Saturn_SetupTag( "FIX-TEX");
	Saturn_PumpFrame();

	R_FixupTextures();

/* */
/* if deathmatch, randomly spawn the active players */
/* */
	if (netgame == gt_deathmatch)
	{
		int i;
		for (i=0 ; i<MAXPLAYERS ; i++)
			if (playeringame[i])
			{	/* must give a player spot before deathmatchspawn */
				mobj_t *mobj = P_SpawnMobj (deathmatchstarts[0].x<<16
				,deathmatchstarts[0].y<<16,0, MT_PLAYER);
				mobj->player = i + 1;
				players[i].mo = mobj;
				G_DeathMatchSpawnPlayer (i);
				P_RemoveMobj (mobj);
				bodyqueslot = 0; // bodyque shouldn't he holding a reference to removed mobj
			}
	}

/* set up world state */
	/* Sub-stage: SEMI CYAN = R_FixupTextures returned; entering
	 * P_SpawnSpecials. Persistent SEMI CYAN = the hang is inside
	 * P_SpawnSpecials (sector/line special thinkers). */
	Saturn_StageColor(JO_COLOR_SemiCyan);
	Saturn_SetupTag( "SPECIALS");
	Saturn_PumpFrame();
	P_SpawnSpecials ();

	/* Sub-stage: MEDIUM WHITE = specials done; entering status-bar
	 * init (first level only allocates the stbar array). Persistent
	 * MEDIUM WHITE = the hang is inside ST_InitEveryLevel. */
	Saturn_StageColor(JO_COLOR_MediumWhite);
	Saturn_SetupTag( "STBAR");
	Saturn_PumpFrame();
	ST_InitEveryLevel ();

	/* Saturn debug sub-stage: WHITE = specials + status-bar init done â€”
	 * the very last thing before P_SetupLevel returns. d_main paints
	 * DARK CYAN + IN-LOOP next; anything else from here on means the
	 * hang is in D_DoomLoop (first background fill / first render). */
	Saturn_PaintNow(JO_COLOR_Red);
	
/*I_Error("free memory: 0x%x\n", Z_FreeMemory(mainzone)); */
/*printf ("free memory: 0x%x\n", Z_FreeMemory(mainzone)); */

	cy = 4;

#ifdef JAGUAR
{
extern byte *debugscreen;
	D_memset (debugscreen,0,32*224);
	
}
#endif

	iquehead = iquetail = 0;
	gamepaused = false;

	gamezonemargin = DEFAULT_GAME_ZONE_MARGIN;
	if (havebossspit)
		gamezonemargin *= 2;

	/* Saturn raycaster: drop per-level caches (line lengths) built from
	 * the PREVIOUS level's line/vertex tables before this level's
	 * renderer state is used. */
	Saturn_RenderInvalidateLevel();
	R_SetupLevel(gamezonemargin);

#ifdef MARS
	Mars_CommSlaveClearCache();
#endif
	Saturn_PaintNow(JO_COLOR_MediumGreen);
}


/*
=================
=
= P_Init
=
=================
*/

void P_Init (void)
{
#ifdef SATURN_WIN32
    printf("P_Init: Z_Mallocs\n"); fflush(stdout);
#endif
	anims = Z_Malloc(sizeof(*anims) * MAXANIMS, PU_STATIC);
	switchlist = Z_Malloc(sizeof(*switchlist)* MAXSWITCHES * 2, PU_STATIC);
	buttonlist = Z_Malloc(sizeof(*buttonlist) * MAXBUTTONS, PU_STATIC);
	linespeciallist = Z_Malloc(sizeof(*linespeciallist) * MAXLINEANIMS, PU_STATIC);

	activeplats = Z_Malloc(sizeof(*activeplats) * MAXPLATS, PU_STATIC);
	activeceilings = Z_Malloc(sizeof(*activeceilings) * MAXCEILINGS, PU_STATIC);

#ifdef SATURN_WIN32
    printf("P_Init: P_InitSwitchList\n"); fflush(stdout);
#endif
	P_InitSwitchList ();
#ifdef SATURN_WIN32
    printf("P_Init: P_InitPicAnims\n"); fflush(stdout);
#endif
	P_InitPicAnims ();
#ifdef SATURN_WIN32
    printf("P_Init: Done\n"); fflush(stdout);
#endif
#ifndef MARS
	pausepic = W_CacheLumpName ("PAUSED",PU_STATIC);
#endif
}



