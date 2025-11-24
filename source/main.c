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

#include <sys/stat.h>

#include <3ds/services/fs.h>

u32 __stacksize__ = 0x100000;


u32 bw;







char* token = NULL;
char* param1 = NULL;
char* param2 = NULL;
char* param3 = NULL;
char* param4 = NULL;
char* param5 = NULL;
char* param6 = NULL;
char* param7 = NULL;

float scrollX = 0.0f;
float velX = 0.0f;
bool touched = false;
float lastTouchX = 0.0f;


volatile bool downloadDone = false;



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

httpcContext context;

u32 size;
u32 siz;

bool download(const char* url_str, const char* path) {
    u32 status = 0;
    cstring url = cstr_new(url_str);
    downloadDone = false;

    while (true) {
        if (CHECK_RESULT("httpcOpenContext", httpcOpenContext(&context, HTTPC_METHOD_GET, url.data, 0))) goto fail;
        if (CHECK_RESULT("httpcSetSSLOpt",   httpcSetSSLOpt(&context, SSLCOPT_DisableVerify))) goto close;
        if (CHECK_RESULT("httpcSetKeepAlive", httpcSetKeepAlive(&context, HTTPC_KEEPALIVE_ENABLED))) goto close;
        if (CHECK_RESULT("httpcAddRequestHeaderField", httpcAddRequestHeaderField(&context, "User-Agent", ""))) goto close;
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
//            printf("Redirecting to url: %s\n", url.data);
            httpcCloseContext(&context);
            continue;
        }
        break;
    }

    if (status != 200) {
//        printf("Returned Code: %lu\n", status);
        goto fail;
    }

    siz = 0;
    if (CHECK_RESULT("httpcGetDownloadSizeState", httpcGetDownloadSizeState(&context, NULL, &siz))) {
        httpcCloseContext(&context);
        goto fail;
    }

//    printf("Downloading %lu bytes.\n", size);

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
    bw = 0;
    u32 readSize;
    Result ret = HTTPC_RESULTCODE_DOWNLOADPENDING;

    while (ret == HTTPC_RESULTCODE_DOWNLOADPENDING) {
        ret = httpcDownloadData(&context, data, 0x4000, &readSize);
        if (CHECK_RESULT("FSFILE_Write", FSFILE_Write(h, NULL, bw, data, readSize, FS_WRITE_FLUSH))) {
            break;
        }
        bw += readSize;
//        printf("%lu/%lu\n", bw, size);
    }

    FSFILE_Close(h);
    FSUSER_CloseArchive(sdmcRoot);
    httpcCloseContext(&context);
    free(data);
    cstr_free(&url);
    return ret == 0 || ret == HTTPC_RESULTCODE_DOWNLOADPENDING; // Success if completed or already done

    downloadDone = true;

fail:
    cstr_free(&url);
    return false;
}


















#define SAMPLE_RATE 48000
#define CHANNELS 2
#define BUFFER_MS 120
#define SAMPLES_PER_BUF (SAMPLE_RATE * BUFFER_MS / 1000)
#define WAVEBUF_SIZE (SAMPLES_PER_BUF * CHANNELS * sizeof(int16_t))

ndspWaveBuf waveBufs[2];
int16_t *audioBuffer = NULL;
LightEvent audioEvent;
volatile bool quit = false;

bool fillBuffer(OggOpusFile *file, ndspWaveBuf *buf) {
    int total = 0;
    while (total < SAMPLES_PER_BUF) {
        int16_t *ptr = buf->data_pcm16 + (total * CHANNELS);
        int ret = op_read_stereo(file, ptr, (SAMPLES_PER_BUF - total) * CHANNELS);
        if (ret <= 0) break;
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

void audioThread(void *arg) {
    OggOpusFile *file = (OggOpusFile*)arg;
    while (!quit) {
        for (int i = 0; i < 2; i++) {
            if (waveBufs[i].status == NDSP_WBUF_DONE) {
                if (!fillBuffer(file, &waveBufs[i])) { quit = true; return; }
            }
        }
        svcSleepThread(10000000L);
//        LightEvent_Wait(&audioEvent);
    }
    return;
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

static bool installoccuring = false;

Result installCIA(const char* path) {
    installoccuring = true;
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
    installoccuring = false;
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





char *content;


char* readFileToBuffer(const char* filePath, u32* outSize) {
    Handle file;
    u64 fileSize = 0;
    char* buffer = NULL;

    // Open file
    Result res = FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, filePath), FS_OPEN_READ, 0);   
    if (R_FAILED(res)) return NULL;

    // Get file size
    FSFILE_GetSize(file, &fileSize);
    if (fileSize == 0) {
        FSFILE_Close(file);
        return NULL;
    }

    // Allocate buffer (+1 for null terminator)
    buffer = (char*)malloc(fileSize + 1);
    if (!buffer) {
        FSFILE_Close(file);
        return NULL;
    }

    // Read file
    u32 bytesRead;
    FSFILE_Read(file, &bytesRead, 0, buffer, fileSize);
    FSFILE_Close(file);

    // Null-terminate
    buffer[bytesRead] = '\0';
    if (outSize) *outSize = bytesRead;

    return buffer;
}






C2D_TextBuf sbuffer;
C2D_Text stext;







void DrawText(char *text, float x, float y, int z, float scaleX, float scaleY, u32 color, bool wordwrap) {
//    if (!sbuffer) {return;}
    C2D_TextBufClear(sbuffer);
    C2D_TextParse(&stext, sbuffer, text);
    C2D_TextOptimize(&stext);

    if (!wordwrap) {
        C2D_DrawText(&stext, C2D_WithColor, x, y, z, scaleX, scaleY, color);
    }
    if (wordwrap) {
        C2D_DrawText(&stext, C2D_WithColor | C2D_WordWrap, x, y, z, scaleX, scaleY, color, 290.0f);
    }
}




void createDirectoryRecursive(const char* path) {
    char temp[256];
    snprintf(temp, sizeof(temp), "%s", path);
    char* p = temp;
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            if (temp[0]) mkdir(temp, 0777);
            *p = '/';
        }
        p++;
    }
    if (temp[0]) mkdir(temp, 0777);
}




char* description = NULL;






















void downloadThreadFunc(void* arg);



// Thread function
void downloadThreadFunc(void* arg) {  // <-- Changed return type to void
    char* url = ((char**)arg)[0];
    char* path = ((char**)arg)[1];
    download(url, path);
    free(arg);
}

// Start non-blocking download
Thread startDownload(const char* url, const char* path) {
    char** args = malloc(2 * sizeof(char*));
    args[0] = strdup(url);
    args[1] = strdup(path);

    Thread thread = threadCreate(downloadThreadFunc, args, 0x8000, 0x38, -2, false);
    if (thread == NULL) {
        printf("Failed to create thread!\n");
        free(args[0]); free(args[1]); free(args);
    }
    return thread;
}





































int main() {
    u32 __stacksize__ = 0x100000;
	fsInit();
	romfsInit();
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    httpcInit(0);
    amInit();
    aptInit();

    APT_SetAppCpuTimeLimit(1);

	spriteSheet = C2D_SpriteSheetLoad("romfs:/gfx/sprites.t3x");
    C2D_SpriteFromImage(&logo, C2D_SpriteSheetGetImage(spriteSheet, 0));

    bool downloadbegun = false;

    bool touched = false;
    float lastTouchX = 0.0f;

    static bool installationStarted = false;
    static bool installationDone = false;

    u32 download_progress = 0;  // 0-100%
    bool download_active = false;

    u32 sampleCount;

    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetRate(0, SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    ndspSetCallback(audioCallback, NULL);

    OggOpusFile *file = op_open_file("romfs:/eshop.opus", NULL);

    sbuffer = C2D_TextBufNew(4096);

    audioBuffer = linearAlloc(WAVEBUF_SIZE * 2);
    memset(waveBufs, 0, sizeof(waveBufs));
    for (int i = 0; i < 2; i++) {
        waveBufs[i].data_pcm16 = audioBuffer + (i * SAMPLES_PER_BUF * CHANNELS);
        waveBufs[i].status = NDSP_WBUF_DONE;
    }

    LightEvent_Init(&audioEvent, RESET_ONESHOT);
    Thread thread = threadCreate(audioThread, file, 32 * 1024, 0x3F, 1, false);

    // chatgpt
    if (!fillBuffer(file, &waveBufs[0]));
    if (!fillBuffer(file, &waveBufs[1]));

//    sfx1 = loadOpusToPCM("romfs:/button.opus", &sfx1_samples);

	int loadingcounter = 0;




//    u32 *soc_buffer = memalign(0x1000, 0x100000);
//    if (!soc_buffer) {
        // placeholder
//    }
//    if (socInit(soc_buffer, 0x100000) != 0) {
        // placeholder
//    }

//    int sock = socket(AF_INET, SOCK_STREAM, 0);
//    if (sock < 0) {
        // placeholder
//    }

//    struct sockaddr_in server;
//    memset(&server, 0, sizeof(server));
//    server.sin_family = AF_INET;
//    server.sin_port = htons(6161); // new niche meme
//    server.sin_addr.s_addr = inet_addr("127.0.0.1"); // 104.236.25.60

//    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) != 0) {
//        // placeholder
//    }

    bool newsdownloaded = false;

    bool contentloaded = false;

    C2D_SpriteSheet apps;

    const float boxPositions[7] = {0.0f, 100.0f, 200.0f, 300.0f, 400.0f, 500.0f, 600.0f};


    const float center = 130.0f;

    int tappedbox;

    bool datagrabbed = false;

    char* description = "hamblyrga";

    float descscroll = 0.0f;

    char* news;

    u32 size;

    int cursection = 1;

    int frameCount = 0;
    float fps = 0.0f;

    char fpsText[256];

    u64 lastTime = osGetTime();

    Thread dl;

    int16_t* samples;




    while (aptMainLoop()) {
        hidScanInput();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);













        C2D_TargetClear(top, C2D_Color32(239, 219, 164, 255));
        C2D_TargetClear(bottom, C2D_Color32(239, 219, 164, 255));








        u64 currentTime = osGetTime();
        frameCount++;

        if (currentTime - lastTime >= 1000) { // 1 second passed
            fps = frameCount;
            frameCount = 0;
            lastTime = currentTime; // Reset timer
        }




        sprintf(fpsText, "MissingNo. FPS: %.0f", fps);
        C2D_SceneBegin(top);
        DrawText(fpsText, 0.0f, 0.0f, 0, 0.5f, 0.5f, C2D_Color32(255, 0, 0, 255), false);












        C2D_SceneBegin(top);

        // Scene 1: Loading
        if (scene == 1) {
            C2D_SceneBegin(top);
            C2D_SpriteSetPos(&logo, 115, 35);
            C2D_DrawSprite(&logo);
            loadingcounter++;
            if (loadingcounter > 300) {
                scene = 2;
            }
            C2D_SceneBegin(bottom);
            DrawText("It is recommended to have a SysNAND Backup as reShop is still in beta.\n\nWe are not responsible for any bricks and/or damage.", 0.0f, 0.0f, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), true);
        }

        // Download news once
        if (!newsdownloaded) {
            createDirectoryRecursive("/3ds/reShop/news");
            download("http://104.236.25.60/reShop/news/today.txt", "/3ds/reShop/news/today.txt");
            news = readFileToBuffer("/3ds/reShop/news/today.txt", &size);
            newsdownloaded = true;
        }

        // Load and parse content once
        if (!contentloaded) {
            createDirectoryRecursive("/3ds/reShop/temp");
            download("http://104.236.25.60/reShop/cdn/section/1/applisting.txt", "/3ds/reShop/temp/applisting.txt");
            download("http://104.236.25.60/reShop/cdn/section/1/apps.t3x", "/3ds/reShop/temp/apps.t3x");
            apps = C2D_SpriteSheetLoad("/3ds/reShop/temp/apps.t3x");
            if (!apps) {
                apps = C2D_SpriteSheetLoad("romfs:/sprites.t3x");
            }
            char* tempContent = readFileToBuffer("/3ds/reShop/temp/applisting.txt", &size);
            if (tempContent) {
                char* token = strtok(tempContent, ",");
                if (token) token = strtok(NULL, ",");
                if (token) param1 = strdup(token);
                token = strtok(NULL, ",");
                if (token) param2 = strdup(token);
                token = strtok(NULL, ",");
                if (token) param3 = strdup(token);
                token = strtok(NULL, ",");
                if (token) param4 = strdup(token);
                token = strtok(NULL, ",");
                if (token) param5 = strdup(token);
                token = strtok(NULL, ",");
                if (token) param6 = strdup(token);
                token = strtok(NULL, ",");
                if (token) param7 = strdup(token);
                free(tempContent); // Avoid leak
            }


            contentloaded = true;
        }

        // Scene 2: Scrolling UI
        C2D_SceneBegin(bottom);
        if (scene == 2) {
            touchPosition touch;
            bool touchActive = hidKeysHeld() & KEY_TOUCH;

            // 7 boxes spaced 100px apart
            const int boxCount = 7;
            float boxPositions[7] = {0.0f, 100.0f, 200.0f, 300.0f, 400.0f, 500.0f, 600.0f};

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

            // Inertia and snap
            if (!touched) {
                scrollX += velX;
                velX *= 0.95f;
                if (fabsf(velX) < 0.1f) velX = 0.0f;

                // Snap to nearest box when fling ends
                if (velX == 0.0f) {
                    float center = 130.0f;
                    float bestDist = INFINITY;
                    float targetBox = 0.0f;
                    for (int i = 0; i < boxCount; i++) {
                        float pos = boxPositions[i] + scrollX;
                        float dist = fabsf(pos - center);
                        if (dist < bestDist) {
                            bestDist = dist;
                            targetBox = boxPositions[i];
                        }
                    }
                    float targetScroll = center - targetBox;
                    scrollX += (targetScroll - scrollX) * 0.2f;
                    if (fabsf(targetScroll - scrollX) < 1.0f) {
                        scrollX = targetScroll;
                    }
                }
            }

            // After snapping logic
            float targetBox = center - scrollX;
            int snappedIndex = -1;
            for (int i = 0; i < boxCount; i++) {
                if (fabsf(boxPositions[i] - targetBox) < 1.0f) {
                    snappedIndex = i;
                    break;
                }
            }

            // Draw label only for the snapped box
            for (int i = 0; i < boxCount; i++) {
                float rectX = boxPositions[i] + scrollX;
                if (rectX > -64 && rectX < 384) {
                    C2D_DrawRectSolid(rectX, 100, 0, 64, 64, C2D_Color32(255, 255, 255, 255));
                    if (apps) {
                        C2D_Sprite sprite;
                        C2D_SpriteFromSheet(&sprite, apps, i);
                        C2D_DrawImageAt(sprite.image, rectX, 100, 0, NULL, 1.0f, 1.0f);
                    }
                    if (i == snappedIndex) { // Only show label when snapped
                        const char* label = NULL;
                        switch(i) {
                            case 0: label = param1; break;
                            case 1: label = param2; break;
                            case 2: label = param3; break;
                            case 3: label = param4; break;
                            case 4: label = param5; break;
                            case 5: label = param6; break;
                            case 6: label = param7; break;
                        }
                        if (label) {
                            DrawText(label, rectX - 20, 50.0f, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), false);
                        }
                    }
                }
            }

            static bool wasTouched = false;
            static bool ignoreTap = false;
            bool isTouched = (hidKeysHeld() & KEY_TOUCH);
            if (isTouched) hidTouchRead(&touch);

            // If scrolling fast, ignore tap
            if (fabsf(velX) > 2.0f) ignoreTap = true;

            for (int i = 0; i < boxCount; i++) {
                float rectX = boxPositions[i] + scrollX;
                float rectY = 100;
                float w = 64, h = 64;
                float left = rectX, right = rectX + w;
                float top = rectY, bottom = rectY + h;

                if (!wasTouched && isTouched) {
                    if (!ignoreTap && touch.px >= left && touch.px <= right && touch.py >= top && touch.py <= bottom) {
                        wasTouched = true;
                        tappedbox = i;
                        if (tappedbox != 0) {
                            scene = 3;
                        }
                        if (tappedbox == 0) {
                            scene = 4;
                        }
                        datagrabbed = false;
                    }
                }
            }

            if (!isTouched) {
                wasTouched = false;
                ignoreTap = false; // Reset when touch ends
            }

            // Draw boxes
 //           for (int i = 0; i < boxCount; i++) {
 //               float rectX = boxPositions[i] + scrollX;
 //               if (rectX > -64 && rectX < 384) { // Cull off-screen
 //                   C2D_DrawRectSolid(rectX, 100, 0, 64, 64, C2D_Color32(255, 255, 255, 255));
 //                   const char* label = NULL;
 //                   switch(i) {
 //                       case 0: label = param1; break;
 //                       case 1: label = param2; break;
  //                      case 2: label = param3; break;
 //                       case 3: label = param4; break;
//                        case 4: label = param5; break;
 //                       case 5: label = param6; break;
 //                       case 6: label = param7; break;
 //                   }
  //                  if (label) DrawText(label, rectX - 20, 50.0f, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), false);
 //               }
    //        }

            // Draw news text
            if (news) {
                C2D_SceneBegin(top);
                DrawText(news, 0.0f, 225.5f, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), false);
            }
        }







        if (scene == 3) {
            if (!datagrabbed) {
                descscroll = 0.0f;
                FILE* f = fopen("/3ds/reShop/temp/appdesc.txt", "w");
                fclose(f);
                char url[256];
                sprintf(url, "http://104.236.25.60/reShop/cdn/section/%d/app%ddesc.txt", cursection, tappedbox);
                download(url, "/3ds/reShop/temp/appdesc.txt");
                u32 size;
                description = readFileToBuffer("/3ds/reShop/temp/appdesc.txt", &size);
                if (!description) {
                    description = "Description failed to load, if you are seeing this make an issue on Github or tell us on Discord.";
                }
                datagrabbed = true;
            }
            if (description) {
                C2D_SceneBegin(top);
                DrawText(description, 0.0f, descscroll + 240, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), true);
                C2D_SceneBegin(bottom);
                DrawText(description, 0.0f, descscroll, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), true);
            }
            if (!description) {
                C2D_SceneBegin(bottom);
                DrawText("Description failed to load, if you are seeing this tell us in our Discord.", 0.0f, descscroll, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), true);
            }

            if (hidKeysHeld() & KEY_DOWN) {
                descscroll -= 3;
            }
            if (hidKeysHeld() & KEY_UP) {
                descscroll += 3;
            }
            if (hidKeysDown() & KEY_B) {
                scene = 2;
            }
            if (hidKeysDown() & KEY_A) {
                downloadbegun = false;
                scene = 5;
//                samples = loadOpusToPCM("romfs:/button.opus", &sampleCount);
            }




            C2D_SceneBegin(bottom);
            DrawText("Press B to leave.", 0.0f, 220.0f, 0, 0.5f, 0.5f, C2D_Color32(255, 0, 0, 255), false);



        }


//        if (samples) {
//            playSFX(samples, sampleCount);
//        }









        if (scene == 4) {
            C2D_SceneBegin(top);
            DrawText("Main Screen", 200.0f, 0.0f, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), false);

            if (news) {
                C2D_SceneBegin(top);
                DrawText(news, 0.0f, 225.5f, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), false);
            }

        }

        if (scene == 5) {
            char* total_size;
            if (!downloadbegun) {
                char url[256];
                sprintf(url, "http://104.236.25.60/reShop/cdn/section/%d/app%dsize.txt", cursection, tappedbox);
                download(url, "/3ds/reShop/temp/appsize.txt");
                u32 size;
                total_size = readFileToBuffer("/3ds/reShop/temp/appsize.txt", &size);
                sprintf(url, "http://104.236.25.60/reShop/cdn/section/%d/app%d.cia", cursection, tappedbox);
                dl = startDownload(url, "/3ds/reShop/temp/install.cia");
               // download(url, "/3ds/reShop/temp/install.cia");
                downloadbegun = true;
            }

            download_progress = (bw * 100) / size;  // Percent

            if (bw >= atoi(total_size) && !installationStarted && !installationDone) {
                installationStarted = true;
                bw = 0;
            }

            if (installationStarted && !installationDone) {
                C2D_SceneBegin(bottom);
                DrawText("Installing...", 0.0f, 220.0f, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), false);
                if (installoccuring == false) {
                    installCIA("3ds/reShop/temp/install.cia");
                }
                scene = 6;
                installationDone = true;
            }



            char progmessage[256];
            sprintf(progmessage, "Progress: %1u", bw);

            C2D_SceneBegin(bottom);

            C2D_SceneBegin(top);
            DrawText(progmessage, 0.0f, 225.5f, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), false);
        }

        if (scene == 6) {
            C2D_SceneBegin(top);
            DrawText("Installation Complete!", 130.0f, 100.0f, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), false);
            DrawText("Press B to leave.", 145.0f, 130.0f, 0, 0.5f, 0.5f, C2D_Color32(0, 0, 0, 255), false);

            if (hidKeysDown() & KEY_B) {
                scene = 2;
            }
        }








        if (waveBufs[0].status == NDSP_WBUF_DONE) {
            if (!fillBuffer(file, &waveBufs[0]));
        }
        if (waveBufs[1].status == NDSP_WBUF_DONE) {
            if (!fillBuffer(file, &waveBufs[1]));
        }





        svcSleepThread(1000000L);


        C3D_FrameEnd(0);
        if (hidKeysDown() & KEY_START) break;
    }

    if (file) op_free(file);
//    if (sfx1) linearFree(sfx1);
    if (audioBuffer) linearFree(audioBuffer);
    amExit();
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