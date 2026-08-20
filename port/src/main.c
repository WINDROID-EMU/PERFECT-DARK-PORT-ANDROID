#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include <PR/ultrasched.h>
#include <PR/os_message.h>

#include "lib/main.h"
#include "bss.h"
#include "data.h"

#include "video.h"
#include "audio.h"
#include "input.h"
#include "fs.h"
#include "romdata.h"
#include "config.h"
#include "mod.h"
#include "system.h"
#include "utils.h"
#include "texexport.h"

#ifdef HAVE_NETWORKING
#include "net/net.h"
#endif

#include <SDL.h>

#ifdef ANDROID
#include <jni.h>
#include <android/log.h>
#include <SDL_main.h>
#include <unistd.h>
#include "android_system.h"
#else
#ifndef ANDROID_LOG_INFO
#define ANDROID_LOG_INFO 1
#define ANDROID_LOG_ERROR 2
#define ANDROID_LOG_WARN 3
#define __android_log_print(prio, tag, ...) sysLogPrintf(LOG_NOTE, __VA_ARGS__)
#endif
#endif

u32 g_OsMemSize = 0;
s32 g_OsMemSizeMb = 16;
u8 g_Is4Mb = 0;
s8 g_Resetting = false;
OSSched g_Sched;

OSMesgQueue g_MainMesgQueue;
OSMesg g_MainMesgBuf[32];

u8 *g_MempHeap = NULL;
u32 g_MempHeapSize = 0;

u32 g_VmNumTlbMisses = 0;
u32 g_VmNumPageMisses = 0;
u32 g_VmNumPageReplaces = 0;
u8 g_VmShowStats = 0;

s32 g_TickRateDiv = 1;
s32 g_TickExtraSleep = true;

s32 g_SkipIntro = false;

s32 g_FileAutoSelect = -1;

extern s32 g_StageNum;

s32 bootGetMemSize(void)
{
	return (s32)g_OsMemSize;
}

void *bootAllocateStack(s32 threadid, s32 size)
{
	static u8 bruh[0x1000];
	return bruh;
}

void bootCreateSched(void)
{
	osCreateMesgQueue(&g_MainMesgQueue, g_MainMesgBuf, ARRAYCOUNT(g_MainMesgBuf));
	if (osTvType == OS_TV_MPAL) {
		osCreateScheduler(&g_Sched, NULL, OS_VI_MPAL_LAN1, 1);
	} else {
		osCreateScheduler(&g_Sched, NULL, OS_VI_NTSC_LAN1, 1);
	}
}

static void gameInit(void)
{
	osMemSize = g_OsMemSizeMb * 1024 * 1024;

	for (s32 i = 0; i < MAX_PLAYERS; ++i) {
		struct extplayerconfig *cfg = g_PlayerExtCfg + i;
		cfg->fovzoommult = cfg->fovzoom ? cfg->fovy / 60.0f : 1.0f;
	}

	if (g_HudCenter == HUDCENTER_NORMAL) {
		g_HudAlignModeL = G_ASPECT_CENTER_EXT;
		g_HudAlignModeR = G_ASPECT_CENTER_EXT;
	} else if (g_HudCenter == HUDCENTER_WIDE) {
		g_HudAlignModeL = G_ASPECT_LEFT_EXT | G_ASPECT_WIDE_EXT;
		g_HudAlignModeR = G_ASPECT_RIGHT_EXT | G_ASPECT_WIDE_EXT;
	}

#ifdef HAVE_NETWORKING
	netInit();
#endif
}

static u64 lastConfigSaveTime = 0;
static s32 configDirty = 0;

static void cleanup(void)
{
	sysLogPrintf(LOG_NOTE, "shutdown");
	inputSaveBinds();
	configSave(CONFIG_PATH);
	videoShutdown();
	crashShutdown();
	// TODO: actually shut down all subsystems
}

void saveConfigIfNeeded(void)
{
	if (configDirty) {
		u64 currentTime = sysGetMicroseconds();
		// Save at most every 5 seconds to avoid excessive I/O
		if (currentTime - lastConfigSaveTime > 5000000) {
			sysLogPrintf(LOG_NOTE, "Auto-saving config");
			inputSaveBinds();
			configSave(CONFIG_PATH);
			lastConfigSaveTime = currentTime;
			configDirty = 0;
		}
	}
}

void markConfigDirty(void)
{
	configDirty = 1;
}

#ifdef ANDROID
// Forward declaration
int pd_main(int argc, const char **argv);

// Android-specific globals
static char g_data_path[512] = {0};
static int g_initialized = 0;

// JNI functions for Android
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_MainActivity_nativeInit(JNIEnv* env, jobject thiz, jstring dataPath) {
    if (g_initialized) return;
    
    const char* path = (*env)->GetStringUTFChars(env, dataPath, NULL);
    strncpy(g_data_path, path, sizeof(g_data_path) - 1);
    (*env)->ReleaseStringUTFChars(env, dataPath, path);
    
    g_initialized = 1;
    
    sysLogPrintf(LOG_NOTE, "Android native init complete, data path: %s", g_data_path);
    
    // Don't call pd_main here - let SDL2 handle it
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_MainActivity_nativeStartGame(JNIEnv* env, jobject thiz) {
    __android_log_print(ANDROID_LOG_INFO, "PerfectDark", "nativeStartGame called");
    if (g_initialized) {
        char* argv[] = {"pd", NULL};
        pd_main(1, (const char**)argv);
    }
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_MainActivity_nativeDestroy(JNIEnv* env, jobject thiz) {
    // Cleanup when activity is destroyed
    g_initialized = 0;
}

// SDL2 will call this as the main function on Android
int SDL_main(int argc, char* argv[]) {
    // Add some basic logging to see if we get here
    __android_log_print(ANDROID_LOG_INFO, "PerfectDark", "SDL_main started on Android");
    
    // Wait for initialization if needed
    int timeout = 50; // 5 seconds
    while (!g_initialized && timeout > 0) {
        SDL_Delay(100);
        timeout--;
    }
    
    if (!g_initialized) {
        __android_log_print(ANDROID_LOG_ERROR, "PerfectDark", "Android not initialized after timeout");
        return -1;
    }
    
    // Log the data path for debugging
    __android_log_print(ANDROID_LOG_INFO, "PerfectDark", "Android data path: %s", g_data_path);
    
    // Change to the data directory so the game can find files
    if (chdir(g_data_path) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "PerfectDark", "Failed to change to data directory: %s", g_data_path);
    } else {
        __android_log_print(ANDROID_LOG_INFO, "PerfectDark", "Changed working directory to: %s", g_data_path);
    }
    
    return pd_main(argc, (const char**)argv);
}

const char* sysGetDataPath(void) {
    if (g_data_path[0] == '\0') {
        // Use public directory in internal storage that persists after uninstall
        // This is typically /storage/emulated/0/perfect dark/ or /sdcard/perfect dark/
        strcpy(g_data_path, "/storage/emulated/0/perfect dark");
        sysLogPrintf(LOG_NOTE, "Using public Android data path: %s", g_data_path);
    }
    return g_data_path;
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_GameView_nativeSurfaceCreated(JNIEnv* env, jobject thiz) {
    // OpenGL surface created - SDL2 will handle this
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_GameView_nativeSurfaceChanged(JNIEnv* env, jobject thiz, jint width, jint height) {
    // Surface size changed - SDL2 will handle this
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_GameView_nativeDrawFrame(JNIEnv* env, jobject thiz) {
    // Frame drawing - SDL2 will handle this
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_GameView_nativeKeyDown(JNIEnv* env, jobject thiz, jint keyCode) {
    // Key down event
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_GameView_nativeKeyUp(JNIEnv* env, jobject thiz, jint keyCode) {
    // Key up event
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_GameView_nativeTouchEvent(JNIEnv* env, jobject thiz, jint action, jfloat x, jfloat y, jint pointerId) {
    // Touch event
}

// N64 button bitmask constants (from os_cont.h CONT_* defines)
// A_BUTTON=0x8000, B_BUTTON=0x4000, Z_TRIG=0x2000, START=0x1000, R_TRIG=0x0010, L_TRIG=0x0020
#define ANDROID_BTN_A     0x8000u
#define ANDROID_BTN_B     0x4000u
#define ANDROID_BTN_Z     0x2000u
#define ANDROID_BTN_L     0x0020u
#define ANDROID_BTN_R     0x0010u
#define ANDROID_BTN_START 0x1000u

static const u32 androidBtnMap[] = {
    ANDROID_BTN_A,      // 0 = BTN_A
    ANDROID_BTN_B,      // 1 = BTN_B
    ANDROID_BTN_Z,      // 2 = BTN_Z (gatilho)
    ANDROID_BTN_L,      // 3 = BTN_L (shoulder esquerdo)
    ANDROID_BTN_R,      // 4 = BTN_R (shoulder direito)
    ANDROID_BTN_START,  // 5 = BTN_START
};
#define ANDROID_BTN_COUNT (sizeof(androidBtnMap) / sizeof(androidBtnMap[0]))

JNIEXPORT void JNICALL
Java_com_perfectdark_port_TouchControls_nativeStickInput(JNIEnv* env, jobject thiz, jint stick, jfloat x, jfloat y) {
    // Clamp float [-1..1] to s8 [-127..127]
    s8 ix = (s8)(x * 127.0f);
    s8 iy = (s8)(y * 127.0f);
    if (stick == 0) {
        // Left stick — movement
        g_androidStickX = ix;
        g_androidStickY = iy;
    } else if (stick == 1) {
        // Right stick — camera (swipe area)
        g_androidCamX = ix;
        g_androidCamY = iy;
    }
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_TouchControls_nativeButtonDown(JNIEnv* env, jobject thiz, jint button) {
    if (button >= 0 && (u32)button < ANDROID_BTN_COUNT) {
        g_androidButtons |= androidBtnMap[button];
    }
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_TouchControls_nativeButtonUp(JNIEnv* env, jobject thiz, jint button) {
    if (button >= 0 && (u32)button < ANDROID_BTN_COUNT) {
        g_androidButtons &= ~androidBtnMap[button];
    }
}

JNIEXPORT void JNICALL
Java_com_perfectdark_port_SettingsActivity_nativeApplySettings(
    JNIEnv* env, jobject thiz,
    jboolean fullscreen, jboolean vsync, jboolean displayFPS, jboolean detailTextures, jboolean texFilter2D,
    jint framerateLimit, jfloat screenShake,
    jboolean disableMpDeathMusic,
    jboolean uncapTickrate, jboolean geMuzzleFlashes, jint fieldOfView, jint hudCenter,
    jboolean showControls, jfloat leftStickSensitivity, jfloat rightStickSensitivity,
    jfloat leftStickDeadzone, jfloat rightStickDeadzone, jboolean vibration, jfloat vibrationStrength,
    jboolean texDumpEnabled, jboolean texLoadEnabled
) {
    __android_log_print(ANDROID_LOG_INFO, "PerfectDark", "Applying settings from Android");

    // Apply video settings
    videoSetFullscreen(fullscreen ? 1 : 0);
    videoSetVsync(vsync ? 1 : 0); // Convert boolean to int for vsync
    videoSetDisplayFPS(displayFPS ? 1 : 0);
    videoSetDetailTextures(detailTextures ? 1 : 0);
    videoSetTextureFilter2D(texFilter2D ? 1 : 0);
    videoSetFramerateLimit(framerateLimit);
    g_ViShakeIntensityMult = screenShake;

    // Apply audio settings
    g_MusicDisableMpDeath = disableMpDeathMusic ? 1 : 0;

    // Apply game settings
    g_TickRateDiv = uncapTickrate ? 0 : 1;
    g_BgunGeMuzzleFlashes = geMuzzleFlashes ? 1 : 0;
    g_PlayerExtCfg[0].fovy = (f32)fieldOfView;
    g_HudCenter = hudCenter;

    // Apply controls settings
    // NOTE: extcontrols is NOT set here to preserve user's control style choice
    // showControls only affects Android virtual controls visibility
    inputControllerSetAxisScale(0, 0, 0, leftStickSensitivity); // Left stick X
    inputControllerSetAxisScale(0, 0, 1, leftStickSensitivity); // Left stick Y
    inputControllerSetAxisScale(0, 1, 0, rightStickSensitivity); // Right stick X
    inputControllerSetAxisScale(0, 1, 1, rightStickSensitivity); // Right stick Y
    inputControllerSetAxisDeadzone(0, 0, 0, leftStickDeadzone); // Left stick X deadzone
    inputControllerSetAxisDeadzone(0, 0, 1, leftStickDeadzone); // Left stick Y deadzone
    inputControllerSetAxisDeadzone(0, 1, 0, rightStickDeadzone); // Right stick X deadzone
    inputControllerSetAxisDeadzone(0, 1, 1, rightStickDeadzone); // Right stick Y deadzone
    inputRumbleSetStrength(0, vibrationStrength);

    // Apply texture settings
    // g_TexDumpEnabled = 1 means: create trigger file at next texExportCheckAndRun() call.
    g_TexDumpEnabled = texDumpEnabled ? 1 : 0;
    g_TexLoadEnabled = texLoadEnabled ? 1 : 0;

    // Mark config as dirty and save immediately
    markConfigDirty();
    inputSaveBinds();
    configSave(CONFIG_PATH);

    __android_log_print(ANDROID_LOG_INFO, "PerfectDark", "Settings applied successfully");
}

int pd_main(int argc, const char **argv)
#else
int main(int argc, const char **argv)
#endif
{


	sysInitArgs(argc, argv);

	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "Starting initialization sequence");

	if (!sysArgCheck("--no-crash-handler")) {
		crashInit();
	}

	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "sysInit starting");
	sysInit();
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "fsInit starting");
	fsInit();
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "configInit starting");
	configInit();
	
	// Create texture directories automatically
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "Creating texture directories");
	fsCreateDir("$H/texturas");
	fsCreateDir("$H/texturas/dump");
	fsCreateDir("$H/texturas/load");
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "Texture directories created");
	
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "SDL2 init starting");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
		__android_log_print(ANDROID_LOG_ERROR, "PerfectDark", "SDL_Init failed: %s", SDL_GetError());
		return -1;
	}
	
#ifdef ANDROID
	// Set OpenGL ES attributes for Android
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
	
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "videoInit starting");
	videoInit();
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "inputInit starting");
	inputInit();
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "audioInit starting");
	audioInit();
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "romdataInit starting");
	romdataInit();
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "romdataInit complete");

	g_ValidGbcRomFound = romdataCheckGbcRom();
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "GBC ROM check complete");

	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "gameInit starting");
	gameInit();
	__android_log_print(ANDROID_LOG_INFO, "PerfectDark", "gameInit complete");

	// Note: Texture export check removed from auto-start to prevent crash
	// Textures need to be loaded into memory before export can work
	// Users can trigger export manually by creating the trigger file

	if (fsGetModDir()) {
		modConfigLoad(MOD_CONFIG_FNAME);
	}

	atexit(cleanup);

	bootCreateSched();

	g_OsMemSize = osGetMemSize();

	g_MempHeapSize = g_OsMemSize;
	g_MempHeap = sysMemZeroAlloc(g_MempHeapSize);
	if (!g_MempHeap) {
		sysFatalError("Could not alloc %u bytes for memp heap.", g_MempHeapSize);
	}

	sysLogPrintf(LOG_NOTE, "memp heap at %p - %p", g_MempHeap, g_MempHeap + g_MempHeapSize);
	sysLogPrintf(LOG_NOTE, "rom  file at %p - %p", g_RomFile, g_RomFile + g_RomFileSize);

	g_SndDisabled = sysArgCheck("--no-sound");

	g_StageNum = sysArgGetInt("--boot-stage", STAGE_TITLE);

	if (g_StageNum == STAGE_TITLE && (sysArgCheck("--skip-intro") || g_SkipIntro)) {
		// shorthand for --boot-stage 0x26
		g_StageNum = STAGE_CITRAINING;
	} else if (g_StageNum < 0x01 || g_StageNum > 0x5d) {
		// stage num out of range
		g_StageNum = STAGE_TITLE;
	}

	if (g_StageNum != STAGE_TITLE) {
		sysLogPrintf(LOG_NOTE, "boot stage set to 0x%02x", g_StageNum);
	}

	g_FileAutoSelect = sysArgGetInt("--profile", -1);
	if (g_FileAutoSelect >= 0) {
		sysLogPrintf(LOG_NOTE, "player profile set to %d", g_FileAutoSelect);
	}

	mainProc();

	return 0;
}

PD_CONSTRUCTOR static void gameConfigInit(void)
{
	configRegisterInt("Game.MemorySize", &g_OsMemSizeMb, 4, 2048);
	configRegisterInt("Game.CenterHUD", &g_HudCenter, 0, 2);
	configRegisterInt("Game.MenuMouseControl", &g_MenuMouseControl, 0, 1);
	configRegisterFloat("Game.ScreenShakeIntensity", &g_ViShakeIntensityMult, 0.f, 10.f);
	configRegisterInt("Game.TickRateDivisor", &g_TickRateDiv, 0, 10);
	configRegisterInt("Game.ExtraSleep", &g_TickExtraSleep, 0, 1);
	configRegisterInt("Game.SkipIntro", &g_SkipIntro, 0, 1);
	configRegisterInt("Game.DisableMpDeathMusic", &g_MusicDisableMpDeath, 0, 1);
	configRegisterInt("Game.GEMuzzleFlashes", &g_BgunGeMuzzleFlashes, 0, 1);
	configRegisterInt("Game.MaxExplosions", &g_MaxExplosions, 6, 96);
	for (s32 j = 0; j < MAX_PLAYERS; ++j) {
		const s32 i = j + 1;
		configRegisterFloat(strFmt("Game.Player%d.FovY", i), &g_PlayerExtCfg[j].fovy, 5.f, 175.f);
		configRegisterInt(strFmt("Game.Player%d.FovAffectsZoom", i), &g_PlayerExtCfg[j].fovzoom, 0, 1);
		configRegisterInt(strFmt("Game.Player%d.MouseAimMode", i), &g_PlayerExtCfg[j].mouseaimmode, 0, 1);
		configRegisterFloat(strFmt("Game.Player%d.MouseAimSpeedX", i), &g_PlayerExtCfg[j].mouseaimspeedx, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.MouseAimSpeedY", i), &g_PlayerExtCfg[j].mouseaimspeedy, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.RadialMenuSpeed", i), &g_PlayerExtCfg[j].radialmenuspeed, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.CrosshairSway", i), &g_PlayerExtCfg[j].crosshairsway, 0.f, 10.f);
		configRegisterInt(strFmt("Game.Player%d.CrouchMode", i), &g_PlayerExtCfg[j].crouchmode, 0, CROUCHMODE_TOGGLE_ANALOG);
		configRegisterInt(strFmt("Game.Player%d.ExtendedControls", i), &g_PlayerExtCfg[j].extcontrols, 0, 1);
		configRegisterUInt(strFmt("Game.Player%d.CrosshairColour", i), &g_PlayerExtCfg[j].crosshaircolour, 0, 0xFFFFFFFF);
		configRegisterUInt(strFmt("Game.Player%d.CrosshairSize", i), &g_PlayerExtCfg[j].crosshairsize, 0, 4);
		configRegisterInt(strFmt("Game.Player%d.CrosshairHealth", i), &g_PlayerExtCfg[j].crosshairhealth, 0, CROSSHAIR_HEALTH_ON_WHITE);
		configRegisterInt(strFmt("Game.Player%d.UseKeyReloads", i), &g_PlayerExtCfg[j].usereloads, 0, false);
	}
}
