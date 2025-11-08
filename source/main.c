#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <stdlib.h>
#include <opusfile.h>

#include <3ds/applets/swkbd.h>

#include <citro2d.h>



#define SAMPLE_RATE 48000
#define CHANNELS 2
#define BUFFER_MS 120
#define SAMPLES_PER_BUF (SAMPLE_RATE * BUFFER_MS / 1000)
#define WAVEBUF_SIZE (SAMPLES_PER_BUF * CHANNELS * sizeof(int16_t))






typedef struct {
    char* data;
    size_t len;
    size_t cap;
} cstring;

cstring cstr_new(const char* str) {
    cstring s = {0};
    if (str) {
        s.len = strlen(str);
        s.cap = s.len + 1;
        s.data = malloc(s.cap);
        if (s.data) strcpy(s.data, str);
    }
    return s;
}

void cstr_free(cstring* s) {
    if (s->data) free(s->data);
    s->data = NULL;
    s->len = s->cap = 0;
}

bool CHECK_RESULT(const char* name, Result res) {
    bool failed = R_FAILED(res);
    printf("%s: %s! (0x%08lX)\n", name, failed ? "failed" : "success", res);
    return failed;
}

bool download(const char* url_str, const char* path) {
    httpcContext context;
    u32 status = 0;
    cstring url = cstr_new(url_str);

    while (true) {
        if (CHECK_RESULT("httpcOpenContext", httpcOpenContext(&context, HTTPC_METHOD_GET, url.data, 0))) goto fail;
        if (CHECK_RESULT("httpcSetSSLOpt",   httpcSetSSLOpt(&context, SSLCOPT_DisableVerify))) goto close;
        if (CHECK_RESULT("httpcSetKeepAlive", httpcSetKeepAlive(&context, HTTPC_KEEPALIVE_ENABLED))) goto close;
        if (CHECK_RESULT("httpcAddRequestHeaderField", httpcAddRequestHeaderField(&context, "User-Agent", "reShop-client/1.0.0"))) goto close;
        if (CHECK_RESULT("httpcAddRequestHeaderField", httpcAddRequestHeaderField(&context, "Connection", "Keep-Alive"))) goto close;

        if (CHECK_RESULT("httpcBeginRequest", httpcBeginRequest(&context))) {
        close:
            httpcCloseContext(&context);
            goto fail;
        }

        if (CHECK_RESULT("httpcGetResponsestatus", httpcGetResponseStatusCode(&context, &status))) {
            httpcCloseContext(&context);
            goto fail;
        }

        if ((status >= 301 && status <= 303) || (status >= 307 && status <= 308)) {
            char newurl[0x1000];
            if (CHECK_RESULT("httpcGetResponseHeader", httpcGetResponseHeader(&context, "Location", newurl, sizeof(newurl)))) {
                httpcCloseContext(&context);
                goto fail;
            }
            cstr_free(&url);
            url = cstr_new(newurl);
            printf("Redirecting to url: %s\n", url.data);
            httpcCloseContext(&context);
            continue;
        }
        break;
    }

    if (status != 200) {
        printf("Returned Code: %lu\n", status);
        goto fail;
    }

    u32 size = 0;
    if (CHECK_RESULT("httpcGetDownloadSizeState", httpcGetDownloadSizeState(&context, NULL, &size))) {
        httpcCloseContext(&context);
        goto fail;
    }

    printf("Downloading %lu bytes.\n", size);

    FS_Archive sdmcRoot;
    if (CHECK_RESULT("FSUSER_OpenArchive", FSUSER_OpenArchive(&sdmcRoot, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        httpcCloseContext(&context);
        goto fail;
    }

    Handle h;
    FS_Path filePath = fsMakePath(PATH_ASCII, path);
    if (CHECK_RESULT("FSUSER_OpenFile", FSUSER_OpenFile(&h, sdmcRoot, filePath, FS_OPEN_CREATE | FS_OPEN_READ | FS_OPEN_WRITE, FS_ATTRIBUTE_ARCHIVE))) {
        FSUSER_CloseArchive(sdmcRoot);
        httpcCloseContext(&context);
        goto fail;
    }

    if (CHECK_RESULT("FSFILE_SetSize", FSFILE_SetSize(h, 0))) {
        FSFILE_Close(h);
        FSUSER_CloseArchive(sdmcRoot);
        httpcCloseContext(&context);
        goto fail;
    }

    u8* data = malloc(0x4000);
    u32 bw = 0, readSize;
    Result ret = HTTPC_RESULTCODE_DOWNLOADPENDING;

    while (ret == HTTPC_RESULTCODE_DOWNLOADPENDING) {
        ret = httpcDownloadData(&context, data, 0x4000, &readSize);
        if (CHECK_RESULT("FSFILE_Write", FSFILE_Write(h, NULL, bw, data, readSize, FS_WRITE_FLUSH))) {
            break;
        }
        bw += readSize;
        printf("%lu/%lu\n", bw, size);
    }

    FSFILE_Close(h);
    FSUSER_CloseArchive(sdmcRoot);
    httpcCloseContext(&context);
    free(data);
    cstr_free(&url);
    return ret == 0 || ret == HTTPC_RESULTCODE_DOWNLOADPENDING; // Success if completed or already done

fail:
    cstr_free(&url);
    return false;
}




















// Audio
ndspWaveBuf waveBufs[2];
int16_t *audioBuffer = NULL;
LightEvent audioEvent;
volatile bool quit = false;
OggOpusFile *musicFile = NULL;

// Scrolling
float scrollX = 0.0f, velX = 0.0f;
bool touched = false;
float lastTouchX = 0.0f;

// SFX
int16_t* sfx1 = NULL;
u32 sfx1_samples = 0;

bool fillBuffer(OggOpusFile *file, ndspWaveBuf *buf) {
    int total = 0;
    while (total < SAMPLES_PER_BUF) {
        int16_t *ptr = buf->data_pcm16 + (total * CHANNELS);
        int ret = op_read_stereo(file, ptr, (SAMPLES_PER_BUF - total) * CHANNELS);
        if (ret <= 0) {
            op_pcm_seek(file, 0);
            continue;
        }
        total += ret;
    }
    if (total == 0) return false;
    buf->nsamples = total;
    DSP_FlushDataCache(buf->data_pcm16, total * CHANNELS * sizeof(int16_t));
    ndspChnWaveBufAdd(0, buf);
    return true;
}

void audioCallback(void *arg) {
    if (!quit) LightEvent_Signal(&audioEvent);
}

void playSFX(int16_t* samples, u32 nsamples) {
    ndspChnReset(1);
    ndspChnSetRate(1, SAMPLE_RATE);
    ndspChnSetFormat(1, NDSP_FORMAT_STEREO_PCM16);
    ndspChnWaveBufClear(1);

    ndspWaveBuf waveBuf;
    memset(&waveBuf, 0, sizeof(waveBuf));
    waveBuf.data_pcm16 = samples;
    waveBuf.nsamples = nsamples;
    waveBuf.looping = false;
    waveBuf.status = NDSP_WBUF_DONE;

    DSP_FlushDataCache(samples, nsamples * 4);
    ndspChnWaveBufAdd(1, &waveBuf);
}

int16_t* loadOpusToPCM(const char* path, u32* sampleCount);










bool isTitleInstalled(u64 titleId, FS_MediaType mediaType) {
    u32 titleCount = 0;
    AM_GetTitleCount(mediaType, &titleCount);

    u64* titleList = malloc(titleCount * sizeof(u64));
    if (!titleList) return false;

    Result res = AM_GetTitleList(NULL, mediaType, titleCount, titleList);
    if (R_FAILED(res)) {
        free(titleList);
        return false;
    }

    bool installed = false;
    for (u32 i = 0; i < titleCount; i++) {
        if (titleList[i] == titleId) {
            installed = true;
            break;
        }
    }

    free(titleList);
    return installed;
}

Result installCIA(const char* path) {
    Handle ciaHandle;
    FILE* file = fopen(path, "rb");
    if (!file) return -1;

    Result res = AM_StartCiaInstall(MEDIATYPE_SD, &ciaHandle);
    if (R_FAILED(res)) {
        fclose(file);
        return res;
    }

    u8 buffer[0x1000];
    size_t bytesRead;
    u64 totalBytes = 0;

    while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        u32 bytesWritten;
        res = FSFILE_Write(ciaHandle, &bytesWritten, totalBytes, buffer, bytesRead, FS_WRITE_FLUSH);
        if (R_FAILED(res) || bytesRead != bytesWritten) {
            AM_CancelCIAInstall(ciaHandle);
            fclose(file);
            return res ? res : -1;
        }
        totalBytes += bytesRead;
    }

    fclose(file);
    res = AM_FinishCiaInstall(ciaHandle);
    return res;
}

bool isSpriteTapped(C2D_Sprite* sprite, float scaleX, float scaleY) {
    static bool wasTouched = false;
    bool isTouched = (hidKeysHeld() & KEY_TOUCH);

    if (!wasTouched && isTouched) {
        touchPosition touch;
        hidTouchRead(&touch);
        float w = sprite->image.subtex->width * scaleX;
        float h = sprite->image.subtex->height * scaleY;
        float x = sprite->params.pos.x;
        float y = sprite->params.pos.y;
        float left = x - w/2, right = x + w/2;
        float top = y - h/2, bottom = y + h/2;

        if (touch.px >= left && touch.px <= right && touch.py >= top && touch.py <= bottom) {
            wasTouched = true;
            return true;
        }
    }

    if (!isTouched) wasTouched = false;
    return false;
}















int scene = 1;

C2D_SpriteSheet spriteSheet;
C2D_Sprite logo;
C2D_Sprite loadingbags;






int main() {
	fsInit();
	romfsInit();
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    httpcInit(0);

	spriteSheet = C2D_SpriteSheetLoad("romfs:/gfx/sprites.t3x");
    C2D_SpriteFromImage(&logo, C2D_SpriteSheetGetImage(spriteSheet, 0));

    download("https://raw.githubusercontent.com/VirtuallyExisting/DISC-DB/refs/heads/main/basic", "/3ds/blurga.txt");

    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetRate(0, SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    ndspSetCallback(audioCallback, NULL);

    LightEvent_Init(&audioEvent, RESET_ONESHOT);

    musicFile = op_open_file("romfs:/eshop.opus", NULL);

    audioBuffer = linearAlloc(WAVEBUF_SIZE * 2);
    memset(waveBufs, 0, sizeof(waveBufs));
    for (int i = 0; i < 2; i++) {
        waveBufs[i].data_pcm16 = audioBuffer + (i * SAMPLES_PER_BUF * CHANNELS);
        waveBufs[i].status = NDSP_WBUF_DONE;
    }

    fillBuffer(musicFile, &waveBufs[0]);
    fillBuffer(musicFile, &waveBufs[1]);

    sfx1 = loadOpusToPCM("romfs:/button.opus", &sfx1_samples);

	int loadingcounter = 0;

    while (aptMainLoop()) {
        hidScanInput();










		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        C2D_TargetClear(top, C2D_Color32(0, 0, 0, 255));




		if (scene == 1) {
			C2D_SceneBegin(top);
			C2D_SpriteSetPos(&logo, 100, 50);
			C2D_DrawSprite(&logo);
			loadingcounter++;
			if (loadingcounter > 300) {
				scene = 2;
			}
		}















		if (scene == 2) {
			touchPosition touch;
			bool touchActive = hidKeysHeld() & KEY_TOUCH;

			if (touchActive) {
				hidTouchRead(&touch);
				if (!touched) {
					lastTouchX = touch.px;
					touched = true;
				} else {
					float delta = touch.px - lastTouchX;
					velX = delta * 0.5f;
					scrollX += delta;
					lastTouchX = touch.px;
				}
			} else if (touched) {
				touched = false;
			}

			if (!touched) {
				scrollX += velX;
				velX *= 0.95f;
				if (fabsf(velX) < 0.1f) velX = 0.0f;
			}

			if (hidKeysDown() & KEY_A && sfx1) playSFX(sfx1, sfx1_samples);

			float rectX = 100.0f + scrollX;
			rectX = fmaxf(fminf(rectX, 320.0f), -200.0f);
			float rect2X = 0.0f + scrollX;
			rect2X = fmaxf(fminf(rect2X, 320.0f), -200.0f);

			C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(bottom);

			C2D_DrawRectSolid(rectX, 100, 0, 64, 64, C2D_Color32(255, 255, 255, 255));
			C2D_DrawRectSolid(rect2X, 100, 0, 64, 64, C2D_Color32(255, 255, 255, 255));
		}

        C3D_FrameEnd(0);
        if (hidKeysDown() & KEY_START) break;
    }

    if (musicFile) op_free(musicFile);
    if (sfx1) linearFree(sfx1);
    if (audioBuffer) linearFree(audioBuffer);
    C2D_Fini();
    ndspExit();
    httpcExit();
    C3D_Fini();
    gfxExit();
    return 0;
}

int16_t* loadOpusToPCM(const char* path, u32* sampleCount) {
    OggOpusFile* of = op_open_file(path, NULL);
    if (!of) return NULL;

    int totalSamples = op_pcm_total(of, -1) * 2;
    int16_t* buffer = linearAlloc(totalSamples * sizeof(int16_t));
    if (!buffer) { op_free(of); return NULL; }

    int offset = 0;
    while (offset < totalSamples) {
        int ret = op_read_stereo(of, buffer + offset, totalSamples - offset);
        if (ret <= 0) break;
        offset += ret;
    }

    op_free(of);
    *sampleCount = offset;
    return buffer;
}