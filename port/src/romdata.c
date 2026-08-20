#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <PR/ultratypes.h>
#include "lib/rzip.h"
#include "romdata.h"
#include "fs.h"
#include "system.h"
#include "preprocess.h"
#include "platform.h"
#include <sys/stat.h>
#if defined(_WIN32) && defined(_MSC_VER)
#include <windows.h>
#else
#include <dirent.h>
#include <strings.h>
#endif

/**
 * asset files and ROM segments can be replaced by optional external files,
 * but asset filenames still have to be either pulled from the ROM or from an
 * external file, so stuff can't be completely custom
 * 
 * all data is assumed to be big endian, so it has to be byteswapped
 * at load time, which is fucking terrible
 */

#define ROMDATA_FILEDIR "files"
#define ROMDATA_SEGDIR "segs"

#define ROMDATA_ROM_NAME "pd." VERSION_ROMID ".z64"
#define ROMDATA_ROM_SIZE 33554432

#if VERSION == VERSION_NTSC_FINAL
#define ROMDATA_ROM_TITLE "Perfect Dark"
#define ROMDATA_ROM_ID "NPDE"
#define ROMDATA_ROM_DESC "NTSC v1.1"
#define ROMDATA_FILES_OFS 0x28080
#define ROMDATA_DATA_OFS 0x39850
#elif VERSION == VERSION_PAL_FINAL
#define ROMDATA_ROM_TITLE "Perfect Dark"
#define ROMDATA_ROM_ID "NPDP"
#define ROMDATA_ROM_DESC "PAL"
#define ROMDATA_FILES_OFS 0x28910
#define ROMDATA_DATA_OFS 0x39850
#elif VERSION == VERSION_JPN_FINAL
#define ROMDATA_ROM_TITLE "PERFECT DARK"
#define ROMDATA_ROM_ID "NPDJ"
#define ROMDATA_ROM_DESC "JPN"
#define ROMDATA_FILES_OFS 0x28800
#define ROMDATA_DATA_OFS 0x39850
#else
#error "This ROM version is unsupported."
#endif

#define ROMDATA_MAX_FILES 2048

#define GBC_ROM_NAME "pd.gbc"
#define GBC_ROM_SIZE 4194304

u8 *g_RomFile;
u32 g_RomFileSize;
const char *g_RomName = ROMDATA_ROM_NAME;

static u8 *romDataSeg;
static u32 romDataSegSize;

enum loadsource {
	SRC_UNLOADED = 0,
	SRC_ROM,
	SRC_EXTERNAL
};

struct romfilepatch {
	u32 ofs;
	u32 len;
	const char *src;
	const char *dst;
};

struct romfile {
	u8 **segstart;
	u8 **segend;
	const char *name;
	u8 *data;
	u32 size;
	preprocessfunc preprocess;
	s32 source; // enum loadsource
	s32 preprocessed;
	const struct romfilepatch *patches;
	u32 numpatches;
};

/* patches for individual files; applied on file load, before preprocFuncs, but */
/* after unzip; only applied when loading from a ROM file                       */
static const struct romfilepatch filePatches[] = {
	/* FILE_USETUPLUE: fixes Jon's double "if what" in Infiltration outro */
	{ 0x92a2, 1, "\x6c", "\x99" },
	{ 0x92b0, 1, "\x6c", "\x99" },
};

static struct romfile fileSlots[ROMDATA_MAX_FILES] = {
	[FILE_USETUPLUE] = { .patches = &filePatches[0], .numpatches = 2 },
};

#define ROMSEG_START(n) _ ## n ## SegmentRomStart
#define ROMSEG_END(n) _ ## n ## SegmentRomEnd

/* segment table for ntsc-final                                                     */
/* size will get calculated automatically if it is 0                                */
/* if there are replacement files in the data dir, they will be loaded instead      */
/* offsets are specified for ntsc-final, pal-final and jpn-final in that order      */
#define ROMSEG_LIST() \
	ROMSEG_DECL_SEG(fontjpnsingle,      0x194b20,  0x180330,  0x0,       0x0,      preprocessJpnFont       ) \
	ROMSEG_DECL_SEG(fontjpnmulti,       0x19fb40,  0x18b340,  0x0,       0x0,      preprocessJpnFont       ) \
	ROMSEG_DECL_SEG(animations,         0x1a15c0,  0x18cdc0,  0x190c50,  0x0,      preprocessAnimations    ) \
	ROMSEG_DECL_SEG(mpconfigs,          0x7d0a40,  0x7bc240,  0x7c00d0,  0x11e0,   preprocessMpConfigs     ) \
	ROMSEG_DECL_SEG(mpstringsE,         0x7d1c20,  0x7bd420,  0x7c12b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsJ,         0x7d5320,  0x7c0b20,  0x7c49b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsP,         0x7d8a20,  0x7c4220,  0x7c80b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsG,         0x7dc120,  0x7c7920,  0x7cb7b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsF,         0x7df820,  0x7cb020,  0x7ceeb0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsS,         0x7e2f20,  0x7ce720,  0x7d25b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsI,         0x7e6620,  0x7d1e20,  0x7d5cb0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(firingrange,        0x7e9d20,  0x7d5520,  0x7d93b0,  0x1550,   NULL                    ) \
	ROMSEG_DECL_SEG(fonttahoma,         0x7f7860,  0x7e3060,  0x7e6ef0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fontnumeric,        0x7f8b20,  0x7e4320,  0x7e81b0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicsm, 0x7f9d30,  0x7e5530,  0x7e93c0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicxs, 0x7fbfb0,  0x7e87b0,  0x7ec640,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicmd, 0x7fdd80,  0x7eae20,  0x7eecb0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothiclg, 0x8008e0,  0x7eee70,  0x7f2d00,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(sfxctl,             0x80a250,  0x7f87e0,  0x7fc670,  0x2fb80,  preprocessALBankFile    ) \
	ROMSEG_DECL_SEG(sfxtbl,             0x839dd0,  0x828360,  0x82c1f0,  0x4c2160, NULL                    ) \
	ROMSEG_DECL_SEG(seqctl,             0xcfbf30,  0xcea4c0,  0xcee350,  0xa060,   preprocessALBankFile    ) \
	ROMSEG_DECL_SEG(seqtbl,             0xd05f90,  0xcf4520,  0xcf83b0,  0x17c070, NULL                    ) \
	ROMSEG_DECL_SEG(sequences,          0xe82000,  0xe70590,  0xe74420,  0x563a0,  preprocessSequences     ) \
	ROMSEG_DECL_SEG(texturesdata,       0x1d65f40, 0x1d5ca20, 0x1d61f90, 0x0,      NULL                    ) \
	ROMSEG_DECL_SEG(textureslist,       0x1ff7ca0, 0x1fee780, 0x1ff68f0, 0x0,      preprocessTexturesList  ) \
	ROMSEG_DECL_SEG(copyright,          0x1ffea20, 0x1ff5500, 0x1ffd6b0, 0xb30,    NULL                    ) \
	ROMSEG_DECL_SEG(fontjpn,            0x0,       0x0,       0x178c40,  0x17920,  preprocessJpnFont       )

// declare the vars first

#undef ROMSEG_DECL_SEG
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) u8 *ROMSEG_START(name), *ROMSEG_END(name);
ROMSEG_LIST()

// this is part of the animations seg and as such does not follow the naming convention
// these are set in preprocessAnimations
u8 *_animationsTableRomStart;
u8 *_animationsTableRomEnd;

// then build the table

#undef ROMSEG_DECL_SEG

#if VERSION == VERSION_NTSC_FINAL
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) { &ROMSEG_START(name), &ROMSEG_END(name), #name, (u8 *)ofs_ntsc, size, preproc },
#elif VERSION == VERSION_PAL_FINAL
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) { &ROMSEG_START(name), &ROMSEG_END(name), #name, (u8 *)ofs_pal, size, preproc },
#elif VERSION == VERSION_JPN_FINAL
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) { &ROMSEG_START(name), &ROMSEG_END(name), #name, (u8 *)ofs_jpn, size, preproc },
#endif

static struct romfile romSegs[] = {
	ROMSEG_LIST()
	{ NULL, NULL, NULL, NULL, 0, NULL },
};

/* the game sets g_LoadType to the type of file it expects,              */
/* so we can hijack that in fileLoad and automatically byteswap the file */
static preprocessfunc filePreprocFuncs[] = {
	/* LOADTYPE_NONE  */ NULL,
	/* LOADTYPE_BG    */ NULL, // loaded in parts
	/* LOADTYPE_TILES */ preprocessTilesFile,
	/* LOADTYPE_LANG  */ preprocessLangFile,
	/* LOADTYPE_SETUP */ preprocessSetupFile,
	/* LOADTYPE_PADS  */ preprocessPadsFile,
	/* LOADTYPE_MODEL */ preprocessModelFile,
	/* LOADTYPE_GUN   */ preprocessGunFile,
};

static inline void romdataWrongRomError(const char *fmt, ...)
{
	char reason[1024];
	reason[0] = '\0';

	va_list args;
	va_start(args, fmt);
	vsnprintf(reason, sizeof(reason), fmt, args);
	va_end(args);

	sysFatalError("Wrong ROM file.\n%s\nEnsure that you have the correct " ROMDATA_ROM_DESC " ROM in z64 format.", reason);
}

static inline void romdataLoadRom(void)
{
	sysLogPrintf(LOG_NOTE, "ROM file: %s", g_RomName);

	g_RomFile = fsFileLoad(g_RomName, &g_RomFileSize);

	if (!g_RomFile) {
		sysFatalError("Could not open ROM file %s.\nEnsure that it is in the %s directory.", g_RomName, fsFullPath(""));
	}

	// zips are not guaranteed to start with PK, but might as well at least try
	if (g_RomFileSize > 2 && (!memcmp(g_RomFile, "PK", 2) || !memcmp(g_RomFile, "Rar", 3) || !memcmp(g_RomFile, "7z", 2))) {
		romdataWrongRomError("Your ROM is in an archive file. Please extract it.");
	}

	if (g_RomFileSize != ROMDATA_ROM_SIZE) {
		romdataWrongRomError("ROM size does not match: expected: %u, got: %u.", ROMDATA_ROM_SIZE, g_RomFileSize);
	}

	if (memcmp(g_RomFile + 0x3b, ROMDATA_ROM_ID, 4) || memcmp(g_RomFile + 0x20, ROMDATA_ROM_TITLE, sizeof(ROMDATA_ROM_TITLE) - 1)) {
		romdataWrongRomError("ROM header does not match.");
	}

	// inflate the compressed data segment since that's where some useful stuff is

	u8 *zipped = g_RomFile + ROMDATA_DATA_OFS;
	if (!rzipIs1173(zipped)) {
		romdataWrongRomError("Data segment is not 1173-compressed.");
	}

	const u32 dataSegLen = ((u32)zipped[2] << 16) | ((u32)zipped[3] << 8) | (u32)zipped[4];
	if (dataSegLen < ROMDATA_FILES_OFS) {
		romdataWrongRomError("Data segment too small (%u), need at least %u.", dataSegLen, ROMDATA_FILES_OFS);
	}

	u8 *dataSeg = sysMemAlloc(dataSegLen);
	if (!dataSeg) {
		sysFatalError("Could not allocate %u bytes for data segment.", dataSegLen);
	}

	u8 scratch[5 * 1024];
	if (rzipInflate(zipped, dataSeg, scratch) < 0) {
		free(dataSeg);
		sysFatalError("Could not inflate data segment.");
	}

	romDataSeg = dataSeg;
	romDataSegSize = dataSegLen;
}

static inline void romdataUpdateSegStartEnd(struct romfile* seg)
{
	if (seg->segstart) {
		*seg->segstart = seg->data;
	}

	if (seg->segend) {
		*seg->segend = seg->data + seg->size;
	}
}

static inline void romdataInitSegment(struct romfile *seg)
{
	if (!seg->data) {
		// unused in this ROM, skip it
		sysLogPrintf(LOG_NOTE, "skipping segment %s", seg->name);
		return;
	}

	if (!seg->size) {
		// size unknown
		if (seg[1].name) {
			// use next segment's base to calculate
			seg->size = seg[1].data - seg->data;
		} else {
			// this is the last segment, calculate based on rom size
			seg->size = (uintptr_t)g_RomFileSize - (uintptr_t)seg->data;
		}
	}

	// check if we have an external replacement and load it if so
	char tmp[FS_MAXPATH];
	snprintf(tmp, sizeof(tmp), ROMDATA_SEGDIR "/%s", seg->name);
	u8 *newData = NULL;
	const s32 extFileSize = fsFileSize(tmp);
	if (extFileSize > 0) {
		newData = fsFileLoad(tmp, &seg->size);
	}

	if (!newData) {
		// no external data, just make it point to the rom
		if (g_RomFile) {
			newData = g_RomFile + (uintptr_t)seg->data;
			seg->source = SRC_ROM;
			sysLogPrintf(LOG_NOTE, "loading segment %s from ROM (offset %08x pointer %p)", seg->name, (uintptr_t)seg->data, newData);
		} else {
			sysFatalError("No ROM or external file for segment:\n%s", seg->name);
		}
	} else {
		// loaded external data
		seg->source = SRC_EXTERNAL;
		sysLogPrintf(LOG_NOTE, "loading segment %s from file (pointer %p)", seg->name, newData);
	}

	seg->data = newData;

	romdataUpdateSegStartEnd(seg);

	// call the post load function if any
	if (seg->preprocess && !seg->preprocessed) {
		newData = seg->preprocess(seg->data, seg->size, &seg->size);

		if (newData) {
			if (seg->source == SRC_EXTERNAL)
				sysMemFree(seg->data);
			seg->data = newData;
			romdataUpdateSegStartEnd(seg);
		}
		
		seg->preprocessed = 1;
	}
}

static inline s32 romdataLoadExternalFileList(void)
{
	romDataSeg = fsFileLoad("filenames.lst", &romDataSegSize); // this null terminates the file by itself
	if (!romDataSeg || !romDataSegSize) {
		return 0;
	}

	s32 n = 1;
	char *p = (char *)romDataSeg;
	while (*p && n < ROMDATA_MAX_FILES) {
		// skip whitespace
		while (*p && isspace(*p)) ++p;
		if (*p) {
			const char *start = p;
			// skip to next whitespace or end of file
			while (*p && !isspace(*p)) ++p;
			// null terminate the name if needed
			if (*p) {
				*p++ = '\0';
			}
			fileSlots[n++].name = start;
		}
	}

	return n - 1;
}

static inline void romdataInitFiles(void)
{
	if (!g_RomFile) {
		// no ROM; try to load the file name list from disk
		if (!romdataLoadExternalFileList()) {
			sysFatalError("No ROM file or external filename table found.");
		}
		return;
	}

	// the file offset table is in the data seg
	const u32 *offsets = (u32 *)(romDataSeg + ROMDATA_FILES_OFS);
	u32 i;
	for (i = 1; offsets[i]; ++i) {
		if (offsets + i + 1 < (u32 *)(romDataSeg + romDataSegSize)) {
			const u32 nextofs = PD_BE32(offsets[i + 1]);
			const u32 ofs = PD_BE32(offsets[i]);
			fileSlots[i].data = g_RomFile + ofs;
			fileSlots[i].size = nextofs - ofs;
			fileSlots[i].source = SRC_UNLOADED;
			fileSlots[i].preprocessed = 0;
		}
	}

	// last offset is to the name table
	const u32 *nameOffsets = (u32 *)(g_RomFile + PD_BE32(offsets[i - 1]));
	for (i = 1; nameOffsets[i]; ++i) {
		const u32 ofs = PD_BE32(nameOffsets[i]);
		fileSlots[i].name = (const char *)nameOffsets + ofs; // ofs is relative to the start of the name table
	}
}

static inline struct romfile *romdataGetSeg(const char *name)
{
	struct romfile *seg = romSegs;
	while (seg->name && strcmp(name, seg->name)) {
		++seg;
	}
	return seg;
}

s32 romdataInit(void)
{
	const char *altRomName = sysArgGetString("--rom-file");
	if (altRomName) {
		g_RomName = altRomName;
	}

	romdataLoadRom();

	// set segments to point to the rom or load them externally
	for (struct romfile *seg = romSegs; seg->name; ++seg) {
		romdataInitSegment(seg);
	}

	// load file table from the files segment
	romdataInitFiles();

	sysLogPrintf(LOG_NOTE, "romdataInit: loaded rom, size = %u", g_RomFileSize);

	return 0;
}

static inline bool romdataCheckGbcRomContents(const u8 *gbcRomFile, const u32 gbcRomSize)
{
	if (gbcRomSize != GBC_ROM_SIZE) {
		return false;
	}

	// ROM title
	if (memcmp(gbcRomFile + 0x134, "PerfDark   VPDE", 15) != 0) {
		return false;
	}

	// Licensee code
	if (memcmp(gbcRomFile + 0x144, "4Y", 2) != 0) {
		return false;
	}

	// Header and global checksums
	if (gbcRomFile[0x14D] != 0xA1 || gbcRomFile[0x14E] != 0xAD || gbcRomFile[0x14F] != 0x0F) {
		return false;
	}

	return true;
}

s32 romdataCheckGbcRom(void)
{
	if (fsFileSize(GBC_ROM_NAME) < 0) {
		// bail early if it doesn't exist to avoid generating error messages
		return false;
	}

	u32 gbcRomSize = 0;
	u8 *gbcRomFile = fsFileLoad(GBC_ROM_NAME, &gbcRomSize);
	if (!gbcRomFile) {
		return false;
	}

	const bool ret = romdataCheckGbcRomContents(gbcRomFile, gbcRomSize);
	sysMemFree(gbcRomFile);

	if (ret) {
		sysLogPrintf(LOG_NOTE, "romdataCheckGbcRom: valid GBC rom found");
	}

	return ret;
}

s32 romdataFileGetSize(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "romdataFileGetSize: invalid file num %d", fileNum);
		return -1;
	}

	// ensure any external files are loaded and we use their size
	if (romdataFileLoad(fileNum, NULL)) {
		return fileSlots[fileNum].size;
	}

	sysLogPrintf(LOG_ERROR, "romdataFileGetSize: could not load file num %d", fileNum);
	return -1;
}

u8 *romdataFileGetData(s32 fileNum)
{
	return romdataFileLoad(fileNum, NULL);
}

struct audio_index_entry {
	char key[64];
	char fullpath[FS_MAXPATH];
};

static struct audio_index_entry *g_AudioIndex = NULL;
static u32 g_AudioIndexCount = 0;
static u32 g_AudioIndexCapacity = 0;
static bool g_AudioIndexBuilt = false;

#if !defined(_WIN32) || !defined(_MSC_VER)
static void romdataIndexScanDir(const char *relDir)
{
	const char *fullDir = fsFullPath(relDir);
	DIR *d = opendir(fullDir);
	if (!d) {
		return;
	}

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
			continue;
		}

		char subRel[FS_MAXPATH];
		snprintf(subRel, sizeof(subRel), "%s/%s", relDir, ent->d_name);

		char subFull[FS_MAXPATH];
		snprintf(subFull, sizeof(subFull), "%s/%s", fullDir, ent->d_name);

		struct stat st;
		if (stat(subFull, &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				// Recursively scan subdirectories
				romdataIndexScanDir(subRel);
			} else if (S_ISREG(st.st_mode)) {
				// Check if it's an MP3 file
				const size_t len = strlen(ent->d_name);
				if (len > 4 && strcasecmp(ent->d_name + len - 4, ".mp3") == 0) {
					char lookupKey[64];
					size_t klen = strlen(ent->d_name);
					if (klen >= sizeof(lookupKey)) klen = sizeof(lookupKey) - 1;
					for (size_t i = 0; i < klen; ++i) {
						lookupKey[i] = tolower((unsigned char)ent->d_name[i]);
					}
					lookupKey[klen] = '\0';

					// Check if key already exists
					bool exists = false;
					for (u32 i = 0; i < g_AudioIndexCount; ++i) {
						if (strcmp(g_AudioIndex[i].key, lookupKey) == 0) {
							exists = true;
							// If existing entry is a simple root audio/ file but new one is from a scene subfolder, replace with scene subfolder
							if (strncmp(g_AudioIndex[i].fullpath, "audio/", 6) == 0 && strchr(g_AudioIndex[i].fullpath + 6, '/') == NULL) {
								strncpy(g_AudioIndex[i].fullpath, subRel, sizeof(g_AudioIndex[i].fullpath) - 1);
								g_AudioIndex[i].fullpath[sizeof(g_AudioIndex[i].fullpath) - 1] = '\0';
							}
							break;
						}
					}

					if (!exists) {
						if (g_AudioIndexCount >= g_AudioIndexCapacity) {
							g_AudioIndexCapacity = (g_AudioIndexCapacity == 0) ? 512 : g_AudioIndexCapacity * 2;
							g_AudioIndex = sysMemRealloc(g_AudioIndex, g_AudioIndexCapacity * sizeof(struct audio_index_entry));
						}
						if (g_AudioIndex) {
							struct audio_index_entry *entry = &g_AudioIndex[g_AudioIndexCount++];
							strncpy(entry->fullpath, subRel, sizeof(entry->fullpath) - 1);
							entry->fullpath[sizeof(entry->fullpath) - 1] = '\0';
							strcpy(entry->key, lookupKey);
						}
					}
				}
			}
		}
	}
	closedir(d);
}
#endif

static void romdataBuildAudioIndex(void)
{
	if (g_AudioIndexBuilt) {
		return;
	}
	g_AudioIndexBuilt = true;

#if !defined(_WIN32) || !defined(_MSC_VER)
	// Scan directories where custom audio or scene subfolders reside (audios_por_cenas has highest priority)
	romdataIndexScanDir("audios_por_cenas");
	romdataIndexScanDir("audio");
	romdataIndexScanDir("files/audio");

	if (g_AudioIndexCount > 0) {
		sysLogPrintf(LOG_NOTE, "romdataBuildAudioIndex: Indexed %u audio files across subdirectories", g_AudioIndexCount);
	}
#endif
}

static const char *romdataFindIndexedAudio(const char *filename)
{
	if (!g_AudioIndexBuilt) {
		romdataBuildAudioIndex();
	}
	if (!g_AudioIndex || g_AudioIndexCount == 0 || !filename || !filename[0]) {
		return NULL;
	}

	char lk[64];
	size_t klen = strlen(filename);
	if (klen >= sizeof(lk)) klen = sizeof(lk) - 1;
	for (size_t i = 0; i < klen; ++i) {
		lk[i] = tolower((unsigned char)filename[i]);
	}
	lk[klen] = '\0';

	for (u32 i = 0; i < g_AudioIndexCount; ++i) {
		if (strcmp(g_AudioIndex[i].key, lk) == 0) {
			return g_AudioIndex[i].fullpath;
		}
	}

	return NULL;
}

static u8 *romdataTryLoadExternalFile(s32 fileNum, u32 *outSize)
{
	const char *name = fileSlots[fileNum].name;
	char tmp[FS_MAXPATH] = { 0 };
	u8 *out = NULL;
	u32 size = 0;

	if (name && name[0]) {
		// 1. Check indexed audio files in subdirectories (audios_por_cenas / subfolders) FIRST
		if (name[0] == 'A' || fileNum > 0) {
			char lookupBuf[64];
			const char *foundPath = NULL;

			// Try <name>.mp3
			snprintf(lookupBuf, sizeof(lookupBuf), "%s.mp3", name);
			foundPath = romdataFindIndexedAudio(lookupBuf);

			// Try <name>
			if (!foundPath) {
				foundPath = romdataFindIndexedAudio(name);
			}

			// Try subname (without 'A' and 'M')
			if (!foundPath && name[0] == 'A') {
				const size_t namelen = strlen(name);
				if (namelen > 2) {
					char subname[64] = { 0 };
					size_t copy_len = namelen - 1;
					if (name[namelen - 1] == 'M' || name[namelen - 1] == 'Z') {
						copy_len -= 1;
					}
					if (copy_len > 0 && copy_len < sizeof(subname)) {
						strncpy(subname, name + 1, copy_len);
						subname[copy_len] = '\0';
						snprintf(lookupBuf, sizeof(lookupBuf), "%s.mp3", subname);
						foundPath = romdataFindIndexedAudio(lookupBuf);
					}
				}
			}

			// Try by decimal number: %04d.mp3
			if (!foundPath) {
				snprintf(lookupBuf, sizeof(lookupBuf), "%04d.mp3", fileNum);
				foundPath = romdataFindIndexedAudio(lookupBuf);
			}

			// Try by hex number: %04x.mp3
			if (!foundPath) {
				snprintf(lookupBuf, sizeof(lookupBuf), "%04x.mp3", fileNum);
				foundPath = romdataFindIndexedAudio(lookupBuf);
			}

			if (foundPath && fsFileSize(foundPath) > 0) {
				out = fsFileLoad(foundPath, &size);
				if (out && size) {
					strncpy(tmp, foundPath, sizeof(tmp) - 1);
					goto success;
				}
			}
		}

		// 2. Direct checks in audio/ root folder
		if (name[0] == 'A') {
			snprintf(tmp, sizeof(tmp), "audio/%s.mp3", name);
			if (fsFileSize(tmp) > 0) {
				out = fsFileLoad(tmp, &size);
				if (out && size) goto success;
			}


			// Check audio/<name> (e.g. audio/Arecep01M)
			snprintf(tmp, sizeof(tmp), "audio/%s", name);
			if (fsFileSize(tmp) > 0) {
				out = fsFileLoad(tmp, &size);
				if (out && size) goto success;
			}

			// Check audio/<name_without_A_and_M>.mp3 (e.g. audio/recep01.mp3)
			const size_t namelen = strlen(name);
			if (namelen > 2) {
				char subname[64] = { 0 };
				size_t copy_len = namelen - 1; // skip leading 'A'
				if (name[namelen - 1] == 'M' || name[namelen - 1] == 'Z') {
					copy_len -= 1; // skip trailing 'M' or 'Z'
				}
				if (copy_len > 0 && copy_len < sizeof(subname)) {
					strncpy(subname, name + 1, copy_len);
					subname[copy_len] = '\0';

					snprintf(tmp, sizeof(tmp), "audio/%s.mp3", subname);
					if (fsFileSize(tmp) > 0) {
						out = fsFileLoad(tmp, &size);
						if (out && size) goto success;
					}
				}
			}

			// Check files/audio/<name>.mp3
			snprintf(tmp, sizeof(tmp), "files/audio/%s.mp3", name);
			if (fsFileSize(tmp) > 0) {
				out = fsFileLoad(tmp, &size);
				if (out && size) goto success;
			}
		}

		// 2. Check by file number in audio/ (e.g. audio/0625.mp3, audio/0271.mp3)
		snprintf(tmp, sizeof(tmp), "audio/%04d.mp3", fileNum);
		if (fsFileSize(tmp) > 0) {
			out = fsFileLoad(tmp, &size);
			if (out && size) goto success;
		}

		snprintf(tmp, sizeof(tmp), "audio/%04x.mp3", fileNum);
		if (fsFileSize(tmp) > 0) {
			out = fsFileLoad(tmp, &size);
			if (out && size) goto success;
		}

		snprintf(tmp, sizeof(tmp), "audio/voice_%04d.mp3", fileNum);
		if (fsFileSize(tmp) > 0) {
			out = fsFileLoad(tmp, &size);
			if (out && size) goto success;
		}

		snprintf(tmp, sizeof(tmp), "audio/voice_%04x.mp3", fileNum);
		if (fsFileSize(tmp) > 0) {
			out = fsFileLoad(tmp, &size);
			if (out && size) goto success;
		}

		// 3. Check general files/<name>
		snprintf(tmp, sizeof(tmp), ROMDATA_FILEDIR "/%s", name);
		if (fsFileSize(tmp) > 0) {
			out = fsFileLoad(tmp, &size);
			if (out && size) goto success;
		}

		// 4. Check files/<name>.mp3
		snprintf(tmp, sizeof(tmp), ROMDATA_FILEDIR "/%s.mp3", name);
		if (fsFileSize(tmp) > 0) {
			out = fsFileLoad(tmp, &size);
			if (out && size) goto success;
		}

		// 5. Check indexed audio files in all subdirectories of audio/ and audios_por_cenas/
		if (name[0] == 'A' || fileNum > 0) {
			char lookupBuf[64];
			const char *foundPath = NULL;

			// Try <name>.mp3
			snprintf(lookupBuf, sizeof(lookupBuf), "%s.mp3", name);
			foundPath = romdataFindIndexedAudio(lookupBuf);

			// Try <name>
			if (!foundPath) {
				foundPath = romdataFindIndexedAudio(name);
			}

			// Try subname (without 'A' and 'M')
			if (!foundPath && name[0] == 'A') {
				const size_t namelen = strlen(name);
				if (namelen > 2) {
					char subname[64] = { 0 };
					size_t copy_len = namelen - 1;
					if (name[namelen - 1] == 'M' || name[namelen - 1] == 'Z') {
						copy_len -= 1;
					}
					if (copy_len > 0 && copy_len < sizeof(subname)) {
						strncpy(subname, name + 1, copy_len);
						subname[copy_len] = '\0';
						snprintf(lookupBuf, sizeof(lookupBuf), "%s.mp3", subname);
						foundPath = romdataFindIndexedAudio(lookupBuf);
					}
				}
			}

			// Try by decimal number: %04d.mp3
			if (!foundPath) {
				snprintf(lookupBuf, sizeof(lookupBuf), "%04d.mp3", fileNum);
				foundPath = romdataFindIndexedAudio(lookupBuf);
			}

			// Try by hex number: %04x.mp3
			if (!foundPath) {
				snprintf(lookupBuf, sizeof(lookupBuf), "%04x.mp3", fileNum);
				foundPath = romdataFindIndexedAudio(lookupBuf);
			}

			// Try by voice_%04d.mp3 and voice_%04x.mp3
			if (!foundPath) {
				snprintf(lookupBuf, sizeof(lookupBuf), "voice_%04d.mp3", fileNum);
				foundPath = romdataFindIndexedAudio(lookupBuf);
			}
			if (!foundPath) {
				snprintf(lookupBuf, sizeof(lookupBuf), "voice_%04x.mp3", fileNum);
				foundPath = romdataFindIndexedAudio(lookupBuf);
			}

			if (foundPath && fsFileSize(foundPath) > 0) {
				out = fsFileLoad(foundPath, &size);
				if (out && size) {
					strncpy(tmp, foundPath, sizeof(tmp) - 1);
					goto success;
				}
			}
		}
	}

	return NULL;

success:
	sysLogPrintf(LOG_NOTE, "file %d (%s) loaded externally from %s (size %u)", fileNum, name ? name : "", tmp, size);
	*outSize = size;
	return out;
}


u8 *romdataFileLoad(s32 fileNum, u32 *outSize)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "romdataFileLoad: invalid file num %d", fileNum);
		return NULL;
	}

	u8 *out = NULL;

	// try to load external file
	if (fileSlots[fileNum].source == SRC_UNLOADED) {
		u32 extSize = 0;
		out = romdataTryLoadExternalFile(fileNum, &extSize);
		if (out && extSize) {
			fileSlots[fileNum].data = out;
			fileSlots[fileNum].size = extSize;
			fileSlots[fileNum].source = SRC_EXTERNAL;
			// external file; do not apply patches to this
			fileSlots[fileNum].numpatches = 0;
		} else {
			// tried and failed, fall back to ROM
			fileSlots[fileNum].source = SRC_ROM;
		}
	}

	if (!out) {
		out = fileSlots[fileNum].data;
	}

	if (out && outSize) {
		*outSize = fileSlots[fileNum].size;
	}

	return out;
}

void romdataFilePreprocess(s32 fileNum, s32 loadType, u8 *data, u32 size, u32 *outSize)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "romdataFilePreprocess: invalid file num %d", fileNum);
		return;
	}

	if (data && size /* && !fileSlots[fileNum].preprocessed*/) {
		if (loadType && loadType < (u32)ARRAYCOUNT(filePreprocFuncs) && filePreprocFuncs[loadType]) {
			// apply patches
			for (u32 i = 0; i < fileSlots[fileNum].numpatches; ++i) {
				const struct romfilepatch *p = &fileSlots[fileNum].patches[i];
				if (!memcmp(data + p->ofs, p->src, p->len)) {
					memcpy(data + p->ofs, p->dst, p->len);
					sysLogPrintf(LOG_NOTE, "file %d (%s) patched at offset 0x%x", fileNum, fileSlots[fileNum].name, p->ofs);
				}
			}
			// then preprocess
			filePreprocFuncs[loadType](data, size, outSize);
			// fileSlots[fileNum].preprocessed = 1;
		}
	}
}

void romdataFileFree(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "fsFileFree: invalid file num %d", fileNum);
		return;
	}

	if (fileSlots[fileNum].source == SRC_EXTERNAL) {
		sysMemFree(fileSlots[fileNum].data);
		fileSlots[fileNum].data = NULL;
	}

	fileSlots[fileNum].source = SRC_UNLOADED;
}

const char *romdataFileGetName(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		return NULL;
	}
	return fileSlots[fileNum].name;
}

s32 romdataFileGetNumForName(const char *name)
{
	if (!name || !name[0]) {
		return -1;
	}

	for (s32 i = 0; i < ROMDATA_MAX_FILES; ++i) {
		if (fileSlots[i].name && !strcmp(fileSlots[i].name, name)) {
			return i;
		}
	}

	return -1;
}

u8 *romdataSegGetData(const char *segName)
{
	return romdataGetSeg(segName)->data;
}

u8 *romdataSegGetDataEnd(const char *segName)
{
	struct romfile *seg = romdataGetSeg(segName);
	return seg->data + seg->size;
}

u32 romdataSegGetSize(const char *segName)
{
	return romdataGetSeg(segName)->size;
}

u32 romdataFileGetEstimatedSize(const u32 size, const u32 loadtype)
{
#ifdef PLATFORM_64BIT
	switch (loadtype) {
	case LOADTYPE_BG:	   return (u32)(size * 1.1f);
	case LOADTYPE_TILES: return (u32)(size * 1.1f);
	case LOADTYPE_LANG:  return (u32)(size * 1.3f);
	case LOADTYPE_SETUP: return (u32)(size * 1.5f);
	case LOADTYPE_PADS:  return (u32)(size * 1.7f);
	case LOADTYPE_MODEL: return (u32)(size * 1.7f);
	case LOADTYPE_GUN: return (u32)(size * 1.7f);
	default:
		sysLogPrintf(LOG_WARNING, "romdataFileGetEstimatedSize: wrong loadtype %d", loadtype);
	}
#endif
	return size;
}
