#include <3ds.h>
#include <citro2d.h>
#include "qrcodegen.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

namespace {

constexpr int PORT = 8080;
constexpr size_t MAX_HEADER = 16384;
constexpr size_t IO_CHUNK = 16 * 1024;
constexpr long long MAX_UPLOAD = 64LL * 1024LL * 1024LL;
constexpr int QR_MAX_VERSION = 8;
constexpr int QR_QUIET_ZONE = 4;

constexpr const char* APP_DIR = "sdmc:/3ds/3DS-Link";
constexpr const char* INBOX_DIR = "sdmc:/3ds/3DS-Link/inbox";
constexpr const char* CAMERA_DIR = "sdmc:/3ds/3DS-Link/camera";
constexpr int CAMERA_WIDTH = 400;
constexpr int CAMERA_HEIGHT = 240;
constexpr size_t CAMERA_FRAME_BYTES = CAMERA_WIDTH * CAMERA_HEIGHT * 2;
constexpr u64 CAMERA_WAIT_TIMEOUT = 1000000000ULL;

constexpr u32 COLOR_BG_TOP      = C2D_Color32(233, 240, 245, 255);
constexpr u32 COLOR_BG_BOTTOM   = C2D_Color32(240, 244, 247, 255);
constexpr u32 COLOR_BLUE        = C2D_Color32(69, 158, 214, 255);
constexpr u32 COLOR_BLUE_DARK   = C2D_Color32(29, 109, 169, 255);
constexpr u32 COLOR_BLUE_LIGHT  = C2D_Color32(126, 201, 238, 255);
constexpr u32 COLOR_TEXT        = C2D_Color32(18, 22, 25, 255);
constexpr u32 COLOR_MUTED       = C2D_Color32(58, 66, 71, 255);
constexpr u32 COLOR_WHITE       = C2D_Color32(255, 255, 255, 255);
constexpr u32 COLOR_GREEN       = C2D_Color32(57, 176, 103, 255);
constexpr u32 COLOR_RED         = C2D_Color32(202, 73, 79, 255);
constexpr u32 COLOR_ORANGE      = C2D_Color32(176, 91, 18, 255);
constexpr u32 COLOR_SHADOW      = C2D_Color32(65, 80, 92, 42);
constexpr u32 COLOR_LINE        = C2D_Color32(197, 210, 219, 255);

u32* socBuffer = nullptr;
int serverSocket = -1;
int clientSocket = -1;
bool socStarted = false;
bool serverReady = false;
bool clientSeen = false;

unsigned int requestCount = 0;
unsigned int uploadCount = 0;
unsigned int pinCode = 0000;

std::string localIp = "0.0.0.0";
bool qrReady = false;
uint8_t qrTemp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
uint8_t qrData[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
std::string lastClient = "Aucun iPhone connecte";
std::string statusMessage = "Initialisation du reseau...";
std::string lastAction = "En attente d'une action iPhone";
std::string lastText = "";
std::string lastRemote = "-";
bool cameraMode = false;
bool cameraCaptureRequested = false;
unsigned int cameraPhotoCount = 0;
std::string lastPhotoName = "";
std::string cameraStatus = "Pret a prendre une photo";
int homeSelection = 0;
constexpr int HOME_ITEM_COUNT = 5;
const char* HOME_NAMES[HOME_ITEM_COUNT] = {
    "Fichiers", "Clavier", "Remote", "Camera Link", "Infos"
};
const char* HOME_DESCRIPTIONS[HOME_ITEM_COUNT] = {
    "Echange de fichiers avec l'iPhone",
    "Saisie rapide depuis l'iPhone",
    "Telecommande de 3DS Link",
    "Appareil photo et transfert",
    "Etat de la connexion locale"
};

bool cameraActive = false;
bool cameraHasFrame = false;
Handle cameraReceiveEvent = 0;
Handle cameraErrorEvent = 0;
u32 cameraTransferBytes = 0;
u8* cameraRawBuffer = nullptr;
bool cameraCaptureInterrupted = false;
unsigned int cameraWarmupFrames = 0;
std::vector<u8> lastCapturedFrame(CAMERA_FRAME_BYTES, 0);
bool lastCapturedFrameReady = false;


C3D_RenderTarget* topTarget = nullptr;
C3D_RenderTarget* bottomTarget = nullptr;
C2D_TextBuf textBuffer = nullptr;

struct FileEntry {
    std::string name;
    long long size = 0;
};

std::vector<FileEntry> cameraSessionPhotos;

std::string connectionUrl() {
    return "http://" + localIp + ":" + std::to_string(PORT);
}

std::string pairingUrl() {
    // Format compact pour réduire fortement la densité du QR.
    // Exemple : https://gagnonthe.github.io/3DS-Link/#192.168.0.103,5892
    return "https://gagnonthe.github.io/3DS-Link/#" +
           localIp + "," + std::to_string(pinCode);
}

void generateConnectionQr() {
    const std::string url = pairingUrl();
    qrReady = qrcodegen_encodeText(
        url.c_str(),
        qrTemp,
        qrData,
        qrcodegen_Ecc_MEDIUM,
        qrcodegen_VERSION_MIN,
        QR_MAX_VERSION,
        qrcodegen_Mask_AUTO,
        false
    );
}

void drawConnectionQr(float centerX, float centerY, float maxSize) {
    if (!qrReady) return;

    const int qrSize = qrcodegen_getSize(qrData);
    if (qrSize <= 0) return;

    const int fullModules = qrSize + QR_QUIET_ZONE * 2;
    int module = static_cast<int>(maxSize / static_cast<float>(fullModules));
    if (module < 1) module = 1;

    const float fullSize = static_cast<float>(fullModules * module);
    const float startX = centerX - fullSize * 0.5f;
    const float startY = centerY - fullSize * 0.5f;

    C2D_DrawRectSolid(startX, startY, 0.45f, fullSize, fullSize, COLOR_WHITE);

    const float qrX = startX + static_cast<float>(QR_QUIET_ZONE * module);
    const float qrY = startY + static_cast<float>(QR_QUIET_ZONE * module);

    for (int y = 0; y < qrSize; ++y) {
        for (int x = 0; x < qrSize; ++x) {
            if (qrcodegen_getModule(qrData, x, y)) {
                C2D_DrawRectSolid(
                    qrX + static_cast<float>(x * module),
                    qrY + static_cast<float>(y * module),
                    0.55f,
                    static_cast<float>(module),
                    static_cast<float>(module),
                    COLOR_TEXT
                );
            }
        }
    }
}

void drawRoundedRect(float x, float y, float w, float h, float radius, u32 color, float depth = 0.1f) {
    C2D_DrawRectSolid(x + radius, y, depth, w - 2.0f * radius, h, color);
    C2D_DrawRectSolid(x, y + radius, depth, w, h - 2.0f * radius, color);
    C2D_DrawCircleSolid(x + radius, y + radius, depth, radius, color);
    C2D_DrawCircleSolid(x + w - radius, y + radius, depth, radius, color);
    C2D_DrawCircleSolid(x + radius, y + h - radius, depth, radius, color);
    C2D_DrawCircleSolid(x + w - radius, y + h - radius, depth, radius, color);
}

void drawText(const std::string& value, float x, float y, float scale, u32 color) {
    C2D_Text text;
    C2D_TextParse(&text, textBuffer, value.c_str());
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.8f, scale, scale, color);
}

void drawCenteredText(const std::string& value, float centerX, float y, float scale, u32 color) {
    C2D_Text text;
    C2D_TextParse(&text, textBuffer, value.c_str());
    C2D_TextOptimize(&text);

    float width = 0.0f;
    float height = 0.0f;
    C2D_TextGetDimensions(&text, scale, scale, &width, &height);

    C2D_DrawText(
        &text,
        C2D_WithColor,
        centerX - width / 2.0f,
        y,
        0.8f,
        scale,
        scale,
        color
    );
}

bool sendAll(int sock, const char* data, size_t length) {
    size_t sent = 0;

    while (sent < length) {
        const ssize_t result = send(sock, data + sent, length - sent, 0);
        if (result <= 0) return false;
        sent += static_cast<size_t>(result);
    }

    return true;
}

void sendSimple(
    int sock,
    int code,
    const char* reason,
    const std::string& body,
    const char* contentType = "text/plain; charset=utf-8"
) {
    char header[512];
    const int headerLength = std::snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        code,
        reason,
        contentType,
        static_cast<unsigned int>(body.size())
    );

    if (headerLength > 0) {
        sendAll(sock, header, static_cast<size_t>(headerLength));
        sendAll(sock, body.c_str(), body.size());
    }
}

void closeClient() {
    if (clientSocket >= 0) {
        close(clientSocket);
        clientSocket = -1;
    }
}

void ensureDirectories() {
    mkdir(APP_DIR, 0777);
    mkdir(INBOX_DIR, 0777);
    mkdir(CAMERA_DIR, 0777);
}

std::string urlDecode(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            const char hex[3] = { input[i + 1], input[i + 2], '\0' };
            char* end = nullptr;
            const long value = std::strtol(hex, &end, 16);

            if (end && *end == '\0') {
                output.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        }

        output.push_back(input[i] == '+' ? ' ' : input[i]);
    }

    return output;
}

std::string sanitizeFilename(const std::string& raw) {
    std::string name = raw;

    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);

    std::string clean;
    clean.reserve(name.size());

    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == ' ' ||
            c >= 0x80) {
            clean.push_back(static_cast<char>(c));
        } else {
            clean.push_back('_');
        }
    }

    while (clean.find("..") != std::string::npos) {
        clean.replace(clean.find(".."), 2, "__");
    }

    if (clean.empty() || clean == "." || clean == "..") {
        clean = "fichier.bin";
    }

    if (clean.size() > 120) clean.resize(120);
    return clean;
}

std::string jsonEscape(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 16);

    for (unsigned char c : input) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c < 0x20) {
                    char temp[7];
                    std::snprintf(temp, sizeof(temp), "\\u%04x", c);
                    output += temp;
                } else {
                    output.push_back(static_cast<char>(c));
                }
                break;
        }
    }

    return output;
}

void writeLe16(FILE* f, unsigned int value) {
    fputc(value & 0xFF, f);
    fputc((value >> 8) & 0xFF, f);
}

void writeLe32(FILE* f, unsigned int value) {
    fputc(value & 0xFF, f);
    fputc((value >> 8) & 0xFF, f);
    fputc((value >> 16) & 0xFF, f);
    fputc((value >> 24) & 0xFF, f);
}

u8 expand5(unsigned int value) {
    return static_cast<u8>((value * 255u + 15u) / 31u);
}

u8 expand6(unsigned int value) {
    return static_cast<u8>((value * 255u + 31u) / 63u);
}

// La caméra 3DS renvoie OUTPUT_RGB_565 avec R dans les 5 bits faibles
// et B dans les 5 bits forts (même disposition que l'exemple officiel devkitPro).
void unpackCameraPixel(u16 data, u8& r, u8& g, u8& b) {
    // CAMU OUTPUT_RGB_565 : le mot 16 bits est 0bRRRRRGGGGGGBBBBB.
    // Le framebuffer 3DS masque cette différence car son ordre d'octets RGB8
    // n'est pas celui d'un BMP/Safari. Pour les fichiers Web, R et B doivent
    // donc être lus dans cet ordre.
    r = expand5((data >> 11) & 0x1F);
    g = expand6((data >> 5) & 0x3F);
    b = expand5(data & 0x1F);
}

bool saveCameraBmp(const std::string& path, const u8* rgb565) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;

    const unsigned int rowBytes = CAMERA_WIDTH * 3;
    const unsigned int padding = (4 - (rowBytes % 4)) % 4;
    const unsigned int stride = rowBytes + padding;
    const unsigned int pixelBytes = stride * CAMERA_HEIGHT;
    const unsigned int fileBytes = 54 + pixelBytes;

    writeLe16(f, 0x4D42);
    writeLe32(f, fileBytes);
    writeLe16(f, 0);
    writeLe16(f, 0);
    writeLe32(f, 54);

    writeLe32(f, 40);
    writeLe32(f, CAMERA_WIDTH);
    writeLe32(f, CAMERA_HEIGHT);
    writeLe16(f, 1);
    writeLe16(f, 24);
    writeLe32(f, 0);
    writeLe32(f, pixelBytes);
    writeLe32(f, 2835);
    writeLe32(f, 2835);
    writeLe32(f, 0);
    writeLe32(f, 0);

    const u16* pixels = reinterpret_cast<const u16*>(rgb565);
    const u8 zeroes[3] = {0, 0, 0};

    // BMP stocke les lignes du bas vers le haut.
    for (int y = CAMERA_HEIGHT - 1; y >= 0; --y) {
        for (int x = 0; x < CAMERA_WIDTH; ++x) {
            u8 r, g, b;
            unpackCameraPixel(pixels[y * CAMERA_WIDTH + x], r, g, b);
            fputc(b, f);
            fputc(g, f);
            fputc(r, f);
        }
        if (padding) fwrite(zeroes, 1, padding, f);
    }

    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

void stopCameraStream() {
    if (cameraReceiveEvent) {
        svcCloseHandle(cameraReceiveEvent);
        cameraReceiveEvent = 0;
    }

    if (cameraErrorEvent) {
        svcCloseHandle(cameraErrorEvent);
        cameraErrorEvent = 0;
    }

    if (cameraActive) {
        CAMU_StopCapture(PORT_CAM1);
        CAMU_Activate(SELECT_NONE);
        camExit();
        cameraActive = false;
    }

    if (cameraRawBuffer) {
        free(cameraRawBuffer);
        cameraRawBuffer = nullptr;
    }

    cameraCaptureInterrupted = false;
    cameraHasFrame = false;
    cameraWarmupFrames = 0;
}

bool startCameraStream() {
    if (cameraActive) return true;

    cameraStatus = "Initialisation CAMU...";

    if (!cameraRawBuffer) {
        // Même stratégie d'allocation que l'exemple officiel devkitPro.
        cameraRawBuffer = static_cast<u8*>(malloc(CAMERA_FRAME_BYTES));
        if (!cameraRawBuffer) {
            cameraStatus = "Memoire camera insuffisante";
            return false;
        }
        memset(cameraRawBuffer, 0, CAMERA_FRAME_BYTES);
    }

    Result result = camInit();
    if (R_FAILED(result)) {
        cameraStatus = "camInit a echoue";
        free(cameraRawBuffer);
        cameraRawBuffer = nullptr;
        return false;
    }

    bool ok = true;

    // Configuration volontairement proche de camera/video de devkitPro.
    if (R_FAILED(CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A))) ok = false;
    if (ok && R_FAILED(CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A))) ok = false;
    if (ok && R_FAILED(CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30))) ok = false;
    if (ok && R_FAILED(CAMU_SetNoiseFilter(SELECT_OUT1, true))) ok = false;
    if (ok && R_FAILED(CAMU_SetAutoExposure(SELECT_OUT1, true))) ok = false;
    if (ok && R_FAILED(CAMU_SetAutoWhiteBalance(SELECT_OUT1, true))) ok = false;
    if (ok && R_FAILED(CAMU_SetTrimming(PORT_CAM1, false))) ok = false;

    if (ok && R_FAILED(CAMU_GetMaxBytes(
        &cameraTransferBytes,
        CAMERA_WIDTH,
        CAMERA_HEIGHT
    ))) ok = false;

    if (ok && R_FAILED(CAMU_SetTransferBytes(
        PORT_CAM1,
        cameraTransferBytes,
        CAMERA_WIDTH,
        CAMERA_HEIGHT
    ))) ok = false;

    if (ok && R_FAILED(CAMU_Activate(SELECT_OUT1))) ok = false;

    // L'exemple officiel surveille cet événement : c'était absent de notre
    // première tentative de boucle native.
    if (ok && R_FAILED(CAMU_GetBufferErrorInterruptEvent(
        &cameraErrorEvent,
        PORT_CAM1
    ))) ok = false;

    if (ok && R_FAILED(CAMU_ClearBuffer(PORT_CAM1))) ok = false;
    if (ok && R_FAILED(CAMU_StartCapture(PORT_CAM1))) ok = false;

    if (!ok) {
        if (cameraErrorEvent) {
            svcCloseHandle(cameraErrorEvent);
            cameraErrorEvent = 0;
        }
        CAMU_StopCapture(PORT_CAM1);
        CAMU_Activate(SELECT_NONE);
        camExit();
        free(cameraRawBuffer);
        cameraRawBuffer = nullptr;
        cameraStatus = "Configuration CAMU echouee";
        return false;
    }

    cameraActive = true;
    cameraCaptureInterrupted = false;
    cameraHasFrame = false;
    cameraWarmupFrames = 0;
    cameraStatus = "Camera active - attente premiere frame";
    return true;
}

bool receiveCameraFrame() {
    if (!cameraActive || !cameraRawBuffer) return false;

    // Après une interruption de buffer, l'exemple officiel relance la capture
    // avant de continuer.
    if (cameraCaptureInterrupted) {
        CAMU_ClearBuffer(PORT_CAM1);
        const Result restart = CAMU_StartCapture(PORT_CAM1);
        if (R_FAILED(restart)) {
            cameraStatus = "Impossible de relancer la capture";
            return false;
        }
        cameraCaptureInterrupted = false;
    }

    if (cameraReceiveEvent == 0) {
        const Result receive = CAMU_SetReceiving(
            &cameraReceiveEvent,
            cameraRawBuffer,
            PORT_CAM1,
            CAMERA_FRAME_BYTES,
            static_cast<s16>(cameraTransferBytes)
        );

        if (R_FAILED(receive)) {
            cameraStatus = "CAMU_SetReceiving a echoue";
            return false;
        }
    }

    Handle events[2] = { cameraErrorEvent, cameraReceiveEvent };
    s32 index = -1;

    const Result wait = svcWaitSynchronizationN(
        &index,
        events,
        2,
        false,
        CAMERA_WAIT_TIMEOUT
    );

    if (R_FAILED(wait)) {
        // Ne pas rester bloqué pendant des minutes. On force une vraie
        // resynchronisation de la caméra.
        if (cameraReceiveEvent) {
            svcCloseHandle(cameraReceiveEvent);
            cameraReceiveEvent = 0;
        }
        CAMU_StopCapture(PORT_CAM1);
        CAMU_ClearBuffer(PORT_CAM1);
        cameraCaptureInterrupted = true;
        cameraStatus = "Timeout camera - resynchronisation";
        return false;
    }

    if (index == 0) {
        // Buffer error interrupt : exactement le cas que l'exemple officiel
        // traite avant de continuer.
        if (cameraReceiveEvent) {
            svcCloseHandle(cameraReceiveEvent);
            cameraReceiveEvent = 0;
        }
        CAMU_StopCapture(PORT_CAM1);
        cameraCaptureInterrupted = true;
        cameraStatus = "Buffer camera resynchronise";
        return false;
    }

    if (index != 1) {
        cameraStatus = "Evenement camera inattendu";
        return false;
    }

    svcCloseHandle(cameraReceiveEvent);
    cameraReceiveEvent = 0;

    cameraHasFrame = true;
    ++cameraWarmupFrames;

    if (cameraWarmupFrames < 5) {
        cameraStatus = "Reglage exposition / couleurs...";
    } else {
        cameraStatus = "LIVE";
    }

    return true;
}

// Copie volontairement proche de writePictureToFramebufferRGB565()
// de l'exemple officiel devkitPro camera/video.
void writeCameraFrameToTopFramebuffer(const u8* image) {
    if (!image) return;

    u8* framebuffer = gfxGetFramebuffer(
        GFX_TOP,
        GFX_LEFT,
        nullptr,
        nullptr
    );

    if (!framebuffer) return;

    const u16* pixels = reinterpret_cast<const u16*>(image);

    for (int j = 0; j < CAMERA_HEIGHT; ++j) {
        for (int i = 0; i < CAMERA_WIDTH; ++i) {
            const int drawY = CAMERA_HEIGHT - 1 - j;
            const int drawX = i;

            const size_t v =
                (static_cast<size_t>(drawY) +
                 static_cast<size_t>(drawX) * CAMERA_HEIGHT) * 3;

            const u16 data = pixels[j * CAMERA_WIDTH + i];

            framebuffer[v + 0] = static_cast<u8>((data & 0x1F) << 3);
            framebuffer[v + 1] = static_cast<u8>(((data >> 5) & 0x3F) << 2);
            framebuffer[v + 2] = static_cast<u8>(((data >> 11) & 0x1F) << 3);
        }
    }
}

void presentCameraFrame() {
    if (!cameraHasFrame || !cameraRawBuffer) return;

    writeCameraFrameToTopFramebuffer(cameraRawBuffer);

    gfxFlushBuffers();
    gspWaitForVBlank();
    gfxSwapBuffers();
}

std::string makeCameraFilename() {
    const unsigned long long stamp = static_cast<unsigned long long>(osGetTime());
    char name[64];
    std::snprintf(
        name,
        sizeof(name),
        "CAM_%010llu_%03u.bmp",
        stamp,
        cameraPhotoCount % 1000
    );
    return name;
}

bool captureCameraPhoto() {
    ensureDirectories();

    if (!cameraActive && !startCameraStream()) {
        return false;
    }

    if (!cameraHasFrame || cameraWarmupFrames < 5 || !cameraRawBuffer) {
        cameraStatus = "Attends une seconde : reglage des couleurs...";
        return false;
    }

    // Le compteur n'est modifié qu'après une vraie écriture réussie sur la SD.
    const unsigned int nextPhotoNumber = cameraPhotoCount + 1;
    const unsigned int previousCount = cameraPhotoCount;
    cameraPhotoCount = nextPhotoNumber;
    const std::string name = makeCameraFilename();
    cameraPhotoCount = previousCount;

    const std::string path = std::string(CAMERA_DIR) + "/" + name;

    if (!saveCameraBmp(path, cameraRawBuffer)) {
        cameraStatus = "Erreur d'ecriture sur la carte SD";
        lastAction = "Camera : erreur SD";
        return false;
    }

    cameraPhotoCount = nextPhotoNumber;
    lastPhotoName = name;

    // Conserver exactement la frame photographiée en RAM : l'iPhone peut
    // afficher sa miniature immédiatement sans relire le BMP sur la SD.
    std::memcpy(lastCapturedFrame.data(), cameraRawBuffer, CAMERA_FRAME_BYTES);
    lastCapturedFrameReady = true;

    struct stat photoStat{};
    long long photoSize = 0;
    if (stat(path.c_str(), &photoStat) == 0) {
        photoSize = static_cast<long long>(photoStat.st_size);
    }
    cameraSessionPhotos.insert(cameraSessionPhotos.begin(), {name, photoSize});

    CAMU_PlayShutterSound(SHUTTER_SOUND_TYPE_NORMAL);

    cameraStatus = "Photo " + std::to_string(cameraPhotoCount) + " enregistree";
    lastAction = "Camera : " + name;
    return true;
}

bool sendCameraPreviewBmp(int sock, const u8* source, int outWidth = 160, int outHeight = 96) {
    if (!source) {
        sendSimple(sock, 503, "Service Unavailable", "Camera pas encore prete");
        return false;
    }

    const unsigned int rowBytes = static_cast<unsigned int>(outWidth * 3);
    const unsigned int padding = (4 - (rowBytes % 4)) % 4;
    const unsigned int stride = rowBytes + padding;
    const unsigned int pixelBytes = stride * static_cast<unsigned int>(outHeight);
    const unsigned int fileBytes = 54 + pixelBytes;

    char httpHeader[256];
    const int httpHeaderLength = std::snprintf(
        httpHeader,
        sizeof(httpHeader),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: image/bmp\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Connection: close\r\n\r\n",
        fileBytes
    );

    if (httpHeaderLength <= 0 ||
        !sendAll(sock, httpHeader, static_cast<size_t>(httpHeaderLength))) {
        return false;
    }

    u8 bmpHeader[54]{};
    auto put16 = [&](int pos, unsigned int value) {
        bmpHeader[pos] = value & 0xFF;
        bmpHeader[pos + 1] = (value >> 8) & 0xFF;
    };
    auto put32 = [&](int pos, unsigned int value) {
        bmpHeader[pos] = value & 0xFF;
        bmpHeader[pos + 1] = (value >> 8) & 0xFF;
        bmpHeader[pos + 2] = (value >> 16) & 0xFF;
        bmpHeader[pos + 3] = (value >> 24) & 0xFF;
    };

    put16(0, 0x4D42);
    put32(2, fileBytes);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<unsigned int>(outWidth));
    put32(22, static_cast<unsigned int>(outHeight));
    put16(26, 1);
    put16(28, 24);
    put32(34, pixelBytes);
    put32(38, 2835);
    put32(42, 2835);

    if (!sendAll(sock, reinterpret_cast<const char*>(bmpHeader), sizeof(bmpHeader))) {
        return false;
    }

    const u16* pixels = reinterpret_cast<const u16*>(source);
    std::vector<u8> row(stride, 0);

    // Downsampling à la volée : 160×96 = ~46 Ko au lieu de ~281 Ko.
    // Cela réduit fortement le temps pendant lequel le réseau occupe la 3DS.
    for (int oy = outHeight - 1; oy >= 0; --oy) {
        const int sy = (oy * CAMERA_HEIGHT) / outHeight;
        size_t offset = 0;

        for (int ox = 0; ox < outWidth; ++ox) {
            const int sx = (ox * CAMERA_WIDTH) / outWidth;
            u8 r, g, b;
            unpackCameraPixel(pixels[sy * CAMERA_WIDTH + sx], r, g, b);
            row[offset++] = b;
            row[offset++] = g;
            row[offset++] = r;
        }

        while (offset < row.size()) row[offset++] = 0;

        if (!sendAll(sock, reinterpret_cast<const char*>(row.data()), row.size())) {
            return false;
        }
    }

    return true;
}

bool sendLiveCameraBmp(int sock) {
    if (!cameraHasFrame || !cameraRawBuffer) {
        sendSimple(sock, 503, "Service Unavailable", "Camera pas encore prete");
        return false;
    }
    return sendCameraPreviewBmp(sock, cameraRawBuffer);
}

bool sendLastCapturedPreviewBmp(int sock) {
    if (!lastCapturedFrameReady) {
        sendSimple(sock, 404, "Not Found", "Aucune photo de session");
        return false;
    }
    return sendCameraPreviewBmp(sock, lastCapturedFrame.data());
}

std::vector<FileEntry> listCameraPhotos() {
    std::vector<FileEntry> files;
    DIR* dir = opendir(CAMERA_DIR);
    if (!dir) return files;

    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const std::string path = std::string(CAMERA_DIR) + "/" + name;
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            files.push_back({name, static_cast<long long>(st.st_size)});
        }
    }
    closedir(dir);

    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.name > b.name;
    });
    return files;
}

std::string cameraJson() {
    std::string json = "{\"photos\":[";
    for (size_t i = 0; i < cameraSessionPhotos.size(); ++i) {
        if (i) json += ",";
        json += "{\"name\":\"" + jsonEscape(cameraSessionPhotos[i].name) +
                "\",\"size\":" + std::to_string(cameraSessionPhotos[i].size) + "}";
    }
    json += "],\"count\":" + std::to_string(cameraSessionPhotos.size()) +
            ",\"latest\":\"" + jsonEscape(lastPhotoName) + "\"" +
            ",\"captureCount\":" + std::to_string(cameraPhotoCount) +
            ",\"cameraActive\":" + std::string(cameraActive ? "true" : "false") +
            ",\"hasFrame\":" + std::string(cameraHasFrame ? "true" : "false") +
            "}";
    return json;
}

std::vector<FileEntry> listFiles() {
    std::vector<FileEntry> files;
    DIR* dir = opendir(INBOX_DIR);

    if (!dir) return files;

    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        const std::string path = std::string(INBOX_DIR) + "/" + name;
        struct stat st{};

        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            files.push_back({name, static_cast<long long>(st.st_size)});
        }
    }

    closedir(dir);

    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.name < b.name;
    });

    return files;
}

std::string filesJson() {
    const auto files = listFiles();
    std::string json = "{\"files\":[";

    for (size_t i = 0; i < files.size(); ++i) {
        if (i) json += ",";
        json += "{\"name\":\"" + jsonEscape(files[i].name) + "\",\"size\":" +
                std::to_string(files[i].size) + "}";
    }

    json += "],\"count\":" + std::to_string(files.size()) + "}";
    return json;
}

std::string getQueryValue(const std::string& target, const std::string& key) {
    const size_t q = target.find('?');
    if (q == std::string::npos) return "";

    const std::string query = target.substr(q + 1);
    size_t start = 0;

    while (start <= query.size()) {
        size_t end = query.find('&', start);
        if (end == std::string::npos) end = query.size();

        const std::string part = query.substr(start, end - start);
        const size_t equals = part.find('=');

        const std::string currentKey =
            equals == std::string::npos ? part : part.substr(0, equals);

        if (currentKey == key) {
            return urlDecode(
                equals == std::string::npos ? "" : part.substr(equals + 1)
            );
        }

        start = end + 1;
    }

    return "";
}

std::string pathOnly(const std::string& target) {
    const size_t q = target.find('?');
    return q == std::string::npos ? target : target.substr(0, q);
}

std::string headerValue(const std::string& headers, const std::string& wanted) {
    size_t start = 0;

    while (start < headers.size()) {
        size_t end = headers.find("\r\n", start);
        if (end == std::string::npos) end = headers.size();

        const std::string line = headers.substr(start, end - start);
        const size_t colon = line.find(':');

        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            std::string wantedLower = wanted;
            std::transform(
                wantedLower.begin(),
                wantedLower.end(),
                wantedLower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
            );

            if (key == wantedLower) {
                std::string value = line.substr(colon + 1);

                while (!value.empty() && std::isspace(
                    static_cast<unsigned char>(value.front()))) {
                    value.erase(value.begin());
                }

                while (!value.empty() && std::isspace(
                    static_cast<unsigned char>(value.back()))) {
                    value.pop_back();
                }

                return value;
            }
        }

        start = end + 2;
    }

    return "";
}

bool authorized(const std::string& headers) {
    const std::string pin = headerValue(headers, "X-3DS-Link-Pin");
    return pin == std::to_string(pinCode);
}

bool readRequestHeaders(
    int sock,
    std::string& allReceived,
    size_t& headerEnd
) {
    allReceived.clear();
    headerEnd = std::string::npos;

    char temp[2048];

    while (allReceived.size() < MAX_HEADER) {
        const ssize_t got = recv(sock, temp, sizeof(temp), 0);
        if (got <= 0) return false;

        allReceived.append(temp, static_cast<size_t>(got));

        headerEnd = allReceived.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            headerEnd += 4;
            return true;
        }
    }

    return false;
}

bool receiveBodyToFile(
    int sock,
    const std::string& initial,
    size_t bodyOffset,
    long long contentLength,
    FILE* output
) {
    long long received = 0;

    if (bodyOffset < initial.size()) {
        const size_t already = std::min(
            static_cast<size_t>(contentLength),
            initial.size() - bodyOffset
        );

        if (already > 0) {
            if (fwrite(initial.data() + bodyOffset, 1, already, output) != already) {
                return false;
            }
            received += already;
        }
    }

    std::vector<char> buffer(IO_CHUNK);

    while (received < contentLength) {
        const size_t wanted = static_cast<size_t>(
            std::min<long long>(IO_CHUNK, contentLength - received)
        );

        const ssize_t got = recv(sock, buffer.data(), wanted, 0);
        if (got <= 0) return false;

        if (fwrite(buffer.data(), 1, static_cast<size_t>(got), output) !=
            static_cast<size_t>(got)) {
            return false;
        }

        received += got;
    }

    return true;
}

std::string receiveBodyToString(
    int sock,
    const std::string& initial,
    size_t bodyOffset,
    long long contentLength
) {
    std::string body;

    if (contentLength < 0 || contentLength > 8192) return body;
    body.reserve(static_cast<size_t>(contentLength));

    long long received = 0;

    if (bodyOffset < initial.size()) {
        const size_t already = std::min(
            static_cast<size_t>(contentLength),
            initial.size() - bodyOffset
        );

        body.append(initial.data() + bodyOffset, already);
        received += already;
    }

    char temp[1024];

    while (received < contentLength) {
        const size_t wanted = static_cast<size_t>(
            std::min<long long>(sizeof(temp), contentLength - received)
        );

        const ssize_t got = recv(sock, temp, wanted, 0);
        if (got <= 0) break;

        body.append(temp, static_cast<size_t>(got));
        received += got;
    }

    return body;
}

std::string makeWebPage() {
    return R"HTML(<!doctype html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b84d8">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="3DS Link">
<link rel="manifest" href="/manifest.webmanifest">
<link rel="apple-touch-icon" href="/app-icon.svg">
<title>3DS Link</title>
<style>
:root{
  color-scheme:light;
  font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display","Segoe UI",Arial,sans-serif;
  --accent:#0b84d8;--accent2:#52b7f1;--ink:#17232d;--muted:#6d7f8b;
  --bg:#edf4f8;--surface:rgba(255,255,255,.94);--line:rgba(74,112,134,.18);
  --green:#2faf68;--red:#d04b55;--shadow:0 16px 36px rgba(32,72,94,.13);
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{background:var(--bg)}
body{margin:0;background:
 radial-gradient(circle at 80% -10%,rgba(82,183,241,.28),transparent 34%),
 linear-gradient(180deg,#e8f4fb 0,#f5f8fa 42%,#edf3f6 100%);
 color:var(--ink);min-height:100vh;padding-bottom:88px}
header{
 position:sticky;top:0;z-index:20;
 padding:calc(env(safe-area-inset-top) + 10px) 18px 12px;
 background:rgba(245,250,253,.78);backdrop-filter:blur(22px) saturate(1.45);
 -webkit-backdrop-filter:blur(22px) saturate(1.45);
 border-bottom:1px solid rgba(72,113,139,.14)
}
.wrap{max-width:720px;margin:auto}
.headrow{display:flex;justify-content:space-between;align-items:center;gap:12px}
.brand{display:flex;align-items:center;gap:11px}
.appicon{width:42px;height:42px;border-radius:12px;background:
 linear-gradient(145deg,#55c2f4,#087bd0);box-shadow:0 8px 20px #178bd548;
 display:grid;place-items:center;color:white;font-size:22px;font-weight:900}
h1{font-size:22px;letter-spacing:-.45px;margin:0;line-height:1.05}
.small{font-size:12px;color:var(--muted)}
.status{
 margin:14px 14px 10px;background:var(--surface);border:1px solid var(--line);
 border-radius:22px;padding:15px;box-shadow:var(--shadow);
 backdrop-filter:blur(18px);-webkit-backdrop-filter:blur(18px)
}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--green);
 margin-right:7px;box-shadow:0 0 0 4px #2faf6820}
.pinbox{display:flex;gap:8px;margin-top:12px}
input,button,select,textarea{font:inherit}
input[type=text],input[type=password],textarea{
 width:100%;border:1px solid #c7d6de;border-radius:14px;padding:12px 13px;
 background:#f9fcfd;color:var(--ink);outline:none
}
input:focus,textarea:focus{border-color:#72bceb;box-shadow:0 0 0 4px #0b84d812}
button{
 border:0;border-radius:14px;padding:11px 14px;font-weight:750;
 background:linear-gradient(180deg,#24a2e8,#0b84d8);color:#fff;
 box-shadow:0 5px 14px rgba(11,132,216,.18)
}
button:active{transform:scale(.975)}
button.secondary{background:#edf4f8;color:#31576d;border:1px solid #cfdee6;box-shadow:none}
button.danger{background:#fff0f1;color:#ad3640;border:1px solid #f0c7ca;box-shadow:none}
button:disabled{opacity:.45}
.tabs{
 position:fixed;z-index:30;left:50%;transform:translateX(-50%);bottom:calc(10px + env(safe-area-inset-bottom));
 width:min(680px,calc(100% - 20px));display:grid;grid-template-columns:repeat(5,1fr);gap:5px;
 padding:7px;background:rgba(247,251,253,.88);border:1px solid rgba(99,130,149,.22);
 border-radius:22px;box-shadow:0 16px 35px rgba(35,70,90,.20);
 backdrop-filter:blur(22px) saturate(1.5);-webkit-backdrop-filter:blur(22px) saturate(1.5)
}
.tab{background:transparent;color:#607684;border:0;padding:9px 3px;font-size:11px;box-shadow:none}
.tab.active{background:#fff;color:#087dce;box-shadow:0 3px 10px #52788e24}
.panel{
 display:none;margin:0 14px 14px;background:var(--surface);border:1px solid var(--line);
 border-radius:24px;padding:17px;box-shadow:var(--shadow);
 backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px)
}
.panel.active{display:block}
h2{font-size:22px;letter-spacing:-.4px;margin:0 0 5px}.muted{color:var(--muted);font-size:13px}
.drop{margin-top:14px;border:1.5px dashed #98cce9;border-radius:18px;padding:20px;text-align:center;background:#f5fbfe}
.fileinput{width:100%;margin-top:10px}
.progress{height:8px;background:#dfeaf0;border-radius:999px;overflow:hidden;margin-top:12px}
.bar{height:100%;width:0;background:linear-gradient(90deg,#59c3f4,#0782d5);transition:width .12s}
.filelist{margin-top:14px;border-top:1px solid #e4ecef}
.file{display:grid;grid-template-columns:1fr auto;gap:10px;align-items:center;padding:13px 1px;border-bottom:1px solid #e8eef1}
.name{font-weight:700;overflow-wrap:anywhere}.size{font-size:12px;color:var(--muted);margin-top:3px}
.actions{display:flex;gap:6px}.actions button{padding:8px 10px;font-size:12px}
textarea{min-height:120px;resize:vertical}
.remote{display:grid;grid-template-columns:repeat(3,60px);gap:8px;justify-content:center;margin:18px 0}
.remote button{height:54px}.remote .blank{visibility:hidden}
.abxy{display:grid;grid-template-columns:repeat(2,72px);gap:8px;justify-content:center;margin-top:15px}
.toast{
 position:fixed;z-index:50;left:50%;bottom:calc(88px + env(safe-area-inset-bottom));
 transform:translateX(-50%) translateY(22px);background:#152630eF;color:#fff;
 padding:11px 16px;border-radius:999px;font-size:13px;opacity:0;pointer-events:none;
 transition:.2s;max-width:90%;text-align:center;box-shadow:0 10px 28px #1a2c3660
}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
footer{text-align:center;color:var(--muted);font-size:12px;padding:5px 15px 20px}
.lock{color:#a2631e}
#liveCamera{box-shadow:inset 0 0 0 1px #ffffff16}
@media(max-width:420px){.actions{flex-direction:column}.file{grid-template-columns:1fr auto}#liveCamera{min-height:180px}}

/* 3DS Link Companion — inspiration TWiLight Menu++ 3DS UI */
body{
  background:#e7ece8;
  background-image:linear-gradient(rgba(255,255,255,.24) 1px,transparent 1px),
                   linear-gradient(90deg,rgba(255,255,255,.18) 1px,transparent 1px);
  background-size:12px 12px
}
header{background:#f7f9f6;border-bottom:1px solid #b8c1bc;backdrop-filter:none;-webkit-backdrop-filter:none}
.status,.panel{background:#f9faf8;border:1px solid #bfc8c3;box-shadow:0 3px 0 #cdd4d0;border-radius:13px;backdrop-filter:none}
.tabs{background:#f4f6f3;border:1px solid #aeb9b3;border-radius:13px;box-shadow:0 4px 0 #c1c9c5;backdrop-filter:none;-webkit-backdrop-filter:none}
.tab{border-radius:9px;color:#56645d}
.tab.active{background:#fff;border:2px solid #f0c83e;color:#25332c;box-shadow:0 0 0 2px #fff inset}
button{background:linear-gradient(#63b9e9,#3e99d0);border:1px solid #3185ba;border-radius:9px;box-shadow:inset 0 1px #bce6fa,0 2px 0 #bad0da}
button.secondary{background:linear-gradient(#fff,#e8eeeb);border:1px solid #b8c3bd;color:#33463c}
.appicon{background:#4ba8dc;border-radius:9px;box-shadow:inset 0 1px #b9e5f7,0 2px 0 #afc7d0}
#liveBadge{border:2px solid white}
</style>
</head>
<body>
<header>
  <div class="wrap headrow">
    <div class="brand">
      <div class="appicon">3</div>
      <div><h1>3DS Link</h1><div class="small">3DS-style Companion • Local Link</div></div>
    </div>
    <div class="small">v1.4.1</div>
  </div>
</header>

<div class="wrap">
  <div class="status">
    <div><span class="dot"></span><strong>3DS détectée sur le réseau</strong></div>
    <div class="muted" id="authState">Entre le code PIN affiché sur la 3DS pour déverrouiller les fonctions.</div>
    <div class="pinbox">
      <input id="pin" type="password" inputmode="numeric" maxlength="4" placeholder="Code PIN 3DS">
      <button onclick="unlock()">Connecter</button>
    </div>
  </div>

  <div class="tabs">
    <button class="tab active" onclick="showTab('files',this)">Fichiers</button>
    <button class="tab" onclick="showTab('text',this)">Clavier</button>
    <button class="tab" onclick="showTab('remote',this)">Remote</button>
    <button class="tab" onclick="showTab('camera',this);startCameraLive();loadCamera(true)">Camera</button>
    <button class="tab" onclick="showTab('info',this)">Infos</button>
  </div>

  <section id="files" class="panel active">
    <h2>Transfert de fichiers</h2>
    <div class="muted">Les fichiers sont enregistrés dans <b>/3ds/3DS-Link/inbox/</b>.</div>

    <div class="drop">
      <div>Choisis un fichier sur ton iPhone</div>
      <input class="fileinput" id="filePicker" type="file">
      <button style="margin-top:12px" onclick="uploadFile()">Envoyer vers la 3DS</button>
      <div class="progress"><div id="uploadBar" class="bar"></div></div>
      <div class="muted" id="uploadLabel" style="margin-top:7px">Prêt</div>
    </div>

    <div style="display:flex;justify-content:space-between;align-items:center;margin-top:15px">
      <strong>Sur la 3DS</strong>
      <button class="secondary" onclick="loadFiles()">Actualiser</button>
    </div>
    <div id="fileList" class="filelist"><div class="muted" style="padding:12px 0">Déverrouille la connexion.</div></div>
  </section>

  <section id="text" class="panel">
    <h2>Clavier iPhone → 3DS</h2>
    <div class="muted">Tape confortablement sur l’iPhone puis envoie le texte à l’écran de la 3DS.</div>
    <textarea id="textValue" placeholder="Écris quelque chose…"></textarea>
    <button style="margin-top:10px;width:100%" onclick="sendText()">Afficher sur la 3DS</button>
  </section>

  <section id="remote" class="panel">
    <h2>Remote 3DS Link</h2>
    <div class="muted">Télécommande expérimentale de l’application 3DS Link. Les commandes apparaissent immédiatement sur la console.</div>
    <div class="remote">
      <span class="blank"></span><button onclick="remote('UP')">▲</button><span class="blank"></span>
      <button onclick="remote('LEFT')">◀</button><button onclick="remote('OK')">●</button><button onclick="remote('RIGHT')">▶</button>
      <span class="blank"></span><button onclick="remote('DOWN')">▼</button><span class="blank"></span>
    </div>
    <div class="abxy">
      <button onclick="remote('A')">A</button><button onclick="remote('B')">B</button>
      <button onclick="remote('X')">X</button><button onclick="remote('Y')">Y</button>
    </div>
  </section>


  <section id="camera" class="panel">
    <h2>Camera</h2>
    <div class="muted">Flux léger en direct, commandes prioritaires et photos synchronisées avec la 3DS.</div>

    <div style="margin-top:14px;background:#071016;border-radius:22px;padding:8px;position:relative;overflow:hidden;box-shadow:0 15px 28px #0a27363a">
      <img id="liveCamera" style="display:block;width:100%;aspect-ratio:5/3;object-fit:contain;border-radius:16px;background:#05090b" alt="Flux caméra 3DS">
      <div id="liveBadge" style="position:absolute;left:18px;top:18px;background:#d64048;color:#fff;border-radius:999px;padding:5px 9px;font-size:11px;font-weight:800">LIVE</div>
    </div>

    <div id="cameraState" class="muted" style="margin-top:9px">Connexion au viseur…</div>
    <button style="margin-top:12px;width:100%;font-size:17px;padding:14px" onclick="remoteCapture()">📷 Prendre une photo</button>

    <div id="cameraPreview" style="margin-top:14px"></div>
    <div style="display:flex;justify-content:space-between;align-items:center;margin-top:14px"><strong>Pellicule</strong><button class="secondary" onclick="loadCamera(true)">Actualiser</button></div>
    <div id="cameraRoll" class="filelist"></div>
  </section>

  <section id="info" class="panel">
    <h2>À propos</h2>
    <p class="muted">3DS Link fonctionne uniquement sur ton réseau local. Aucun serveur Internet n’est nécessaire pour le transfert.</p>
    <p class="muted">La v1.4.1 ajoute le launcher tactile, restaure le QR et améliore l’interface 3DS Link.</p>
  </section>

  <footer>3DS Link v1.4.1 • réseau local • garde l’application ouverte sur la 3DS</footer>
</div>

<div id="toast" class="toast"></div>

<script>
let pin = localStorage.getItem('3dsLinkPin') || '';
const $ = id => document.getElementById(id);

if(pin) $('pin').value = pin;

function headers(extra={}) {
  return Object.assign({'X-3DS-Link-Pin':pin}, extra);
}

function toast(msg) {
  const t=$('toast'); t.textContent=msg; t.classList.add('show');
  clearTimeout(window.toastTimer);
  window.toastTimer=setTimeout(()=>t.classList.remove('show'),1800);
}

function showTab(id,btn) {
  document.querySelectorAll('.panel').forEach(x=>x.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));
  $(id).classList.add('active'); btn.classList.add('active');
}

async function unlock() {
  pin=$('pin').value.trim();
  if(!/^\d{4}$/.test(pin)){toast('PIN à 4 chiffres requis');return}
  try{
    const r=await fetch('/api/files',{headers:headers()});
    if(r.status===401){throw new Error('PIN incorrect')}
    if(!r.ok){throw new Error('Erreur réseau')}
    localStorage.setItem('3dsLinkPin',pin);
    $('authState').textContent='Connexion sécurisée active • iPhone autorisé';
    $('authState').className='muted';
    renderFiles(await r.json());
    toast('Connecté à la 3DS');
  }catch(e){
    $('authState').textContent=e.message;
    $('authState').className='muted lock';
    toast(e.message);
  }
}

function formatSize(n){
  if(n<1024)return n+' o';
  if(n<1024*1024)return (n/1024).toFixed(1)+' Ko';
  return (n/(1024*1024)).toFixed(1)+' Mo';
}

function esc(s){
  return s.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

function renderFiles(data){
  const box=$('fileList');
  if(!data.files.length){box.innerHTML='<div class="muted" style="padding:12px 0">Aucun fichier reçu pour le moment.</div>';return}
  box.innerHTML=data.files.map(f=>`
    <div class="file">
      <div><div class="name">${esc(f.name)}</div><div class="size">${formatSize(f.size)}</div></div>
      <div class="actions">
        <button class="secondary" onclick='downloadFile(${JSON.stringify(f.name)})'>Télécharger</button>
        <button class="danger" onclick='deleteFile(${JSON.stringify(f.name)})'>Supprimer</button>
      </div>
    </div>`).join('');
}

async function loadFiles(){
  try{
    const r=await fetch('/api/files',{headers:headers()});
    if(r.status===401)throw new Error('PIN incorrect');
    renderFiles(await r.json());
  }catch(e){toast(e.message)}
}

function uploadFile(){
  const file=$('filePicker').files[0];
  if(!file){toast('Choisis un fichier');return}
  if(!pin){toast('Entre le PIN');return}
  const xhr=new XMLHttpRequest();
  xhr.open('POST','/upload?name='+encodeURIComponent(file.name));
  xhr.setRequestHeader('X-3DS-Link-Pin',pin);
  xhr.upload.onprogress=e=>{
    if(e.lengthComputable){
      const pct=Math.round(e.loaded/e.total*100);
      $('uploadBar').style.width=pct+'%';
      $('uploadLabel').textContent=pct+'% • '+formatSize(e.loaded)+' / '+formatSize(e.total);
    }
  };
  xhr.onload=()=>{
    if(xhr.status===200){
      $('uploadBar').style.width='100%';
      $('uploadLabel').textContent='Transfert terminé';
      toast('Fichier envoyé à la 3DS');
      loadFiles();
    }else{
      $('uploadLabel').textContent='Échec du transfert';
      toast(xhr.status===401?'PIN incorrect':'Erreur '+xhr.status);
    }
  };
  xhr.onerror=()=>{ $('uploadLabel').textContent='Connexion interrompue'; toast('Connexion interrompue') };
  xhr.send(file);
}

async function deleteFile(name){
  if(!confirm('Supprimer "'+name+'" de la 3DS ?'))return;
  const r=await fetch('/delete?name='+encodeURIComponent(name),{method:'POST',headers:headers()});
  if(r.ok){toast('Fichier supprimé');loadFiles()}else toast('Suppression impossible');
}

async function downloadFile(name){
  const r=await fetch('/download?name='+encodeURIComponent(name),{headers:headers()});
  if(!r.ok){toast(r.status===401?'PIN incorrect':'Téléchargement impossible');return}
  const blob=await r.blob();
  const a=document.createElement('a');
  a.href=URL.createObjectURL(blob); a.download=name; document.body.appendChild(a); a.click(); a.remove();
  setTimeout(()=>URL.revokeObjectURL(a.href),1500);
}

async function sendText(){
  const text=$('textValue').value;
  if(!text){toast('Écris du texte');return}
  const r=await fetch('/api/text',{method:'POST',headers:headers({'Content-Type':'text/plain;charset=utf-8'}),body:text});
  toast(r.ok?'Texte affiché sur la 3DS':'Envoi impossible');
}

async function remote(key){
  const r=await fetch('/api/remote?key='+encodeURIComponent(key),{method:'POST',headers:headers()});
  if(r.ok) toast('Commande '+key); else toast('Commande refusée');
}


let latestCameraName='';
let latestCameraUrl='';
let liveCameraObjectUrl='';
let cameraPumpTimer=null;
let cameraPumpBusy=false;
let cameraPumpTick=0;
let capturePending=false;
let captureBefore='';

async function refreshLiveCamera(){
  const r=await fetch('/camera/live.bmp?t='+Date.now(),{
    headers:headers(),
    cache:'no-store'
  });

  if(r.status===503){
    $('cameraState').textContent='La caméra démarre…';
    return;
  }
  if(!r.ok) throw new Error('Flux indisponible');

  const blob=await r.blob();
  const next=URL.createObjectURL(blob);
  $('liveCamera').src=next;

  if(liveCameraObjectUrl) URL.revokeObjectURL(liveCameraObjectUrl);
  liveCameraObjectUrl=next;
}

async function fetchCameraBlob(name){
  const r=await fetch('/camera/file?name='+encodeURIComponent(name)+'&t='+Date.now(),{
    headers:headers(),
    cache:'no-store'
  });
  if(!r.ok) throw new Error(r.status===423?'Photo occupée':'Photo indisponible');
  return await r.blob();
}

async function showLatestCapturedPreview(name){
  try{
    const r=await fetch('/camera/last-preview.bmp?t='+Date.now(),{
      headers:headers(),
      cache:'no-store'
    });
    if(!r.ok) throw new Error('Miniature indisponible');

    const blob=await r.blob();
    if(latestCameraUrl) URL.revokeObjectURL(latestCameraUrl);
    latestCameraUrl=URL.createObjectURL(blob);
    latestCameraName=name;

    $('cameraPreview').innerHTML=`<div style="background:#111;border-radius:14px;padding:8px"><img src="${latestCameraUrl}" style="display:block;width:100%;border-radius:9px" alt="Derniere photo"></div><div style="display:flex;gap:8px;margin-top:8px"><button class="secondary" style="flex:1" onclick='showCameraPhoto(${JSON.stringify(name)})'>Voir en qualité complète</button><button class="secondary" style="flex:1" onclick='saveCameraPhoto(${JSON.stringify(name)})'>Télécharger</button><button class="danger" onclick='deleteCameraPhoto(${JSON.stringify(name)})'>Supprimer</button></div>`;
  }catch(e){}
}

async function showCameraPhoto(name){
  if(!name) return;
  $('cameraState').textContent='Chargement de la photo complète…';
  try{
    const blob=await fetchCameraBlob(name);
    if(latestCameraUrl) URL.revokeObjectURL(latestCameraUrl);
    latestCameraUrl=URL.createObjectURL(blob);
    latestCameraName=name;
    $('cameraPreview').innerHTML=`<div style="background:#111;border-radius:14px;padding:8px"><img src="${latestCameraUrl}" style="display:block;width:100%;border-radius:9px" alt="Photo"></div><div style="display:flex;gap:8px;margin-top:8px"><button class="secondary" style="flex:1" onclick='saveCameraPhoto(${JSON.stringify(name)})'>Télécharger</button><button class="danger" onclick='deleteCameraPhoto(${JSON.stringify(name)})'>Supprimer</button></div>`;
    $('cameraState').textContent='Photo complète affichée';
  }catch(e){
    $('cameraState').textContent=e.message;
    toast(e.message);
  }
}

function renderCamera(data){
  const roll=$('cameraRoll');
  if(!data.photos.length){
    roll.innerHTML='<div class="muted" style="padding:12px 0">Aucune photo dans cette session.</div>';
  }else{
    roll.innerHTML=data.photos.map(p=>`<div class="file"><div><div class="name">${esc(p.name)}</div><div class="size">${formatSize(p.size)}</div></div><div class="actions"><button class="secondary" onclick='showCameraPhoto(${JSON.stringify(p.name)})'>Voir</button><button class="secondary" onclick='saveCameraPhoto(${JSON.stringify(p.name)})'>Télécharger</button></div></div>`).join('');
  }

  const newest=data.photos.length?data.photos[0].name:'';
  if(newest && newest!==latestCameraName){
    showLatestCapturedPreview(newest);
  }

  if(capturePending && newest && newest!==captureBefore){
    capturePending=false;
    $('cameraState').textContent='Photo reçue immédiatement';
    toast('Photo reçue sur l’iPhone');
  }
}

async function loadCamera(){
  const r=await fetch('/api/camera?t='+Date.now(),{
    headers:headers(),
    cache:'no-store'
  });
  if(!r.ok) throw new Error('État Camera indisponible');
  const data=await r.json();
  renderCamera(data);
  return data;
}

async function cameraPump(){
  if(cameraPumpBusy || !$('camera').classList.contains('active') || !pin) return;
  cameraPumpBusy=true;
  try{
    // Une seule file de requêtes : plus de concurrence entre flux,
    // pellicule et attente de capture.
    await refreshLiveCamera();
    cameraPumpTick++;
    if(cameraPumpTick%2===0 || capturePending) await loadCamera();

    if(!capturePending){
      $('cameraState').textContent='LIVE • liaison 3DS active';
    }
  }catch(e){
    $('cameraState').textContent='Synchronisation…';
  }finally{
    cameraPumpBusy=false;
  }
}

function startCameraLive(){
  if(cameraPumpTimer) clearInterval(cameraPumpTimer);
  cameraPumpTick=0;
  cameraPump();
  cameraPumpTimer=setInterval(cameraPump,200);
}

async function remoteCapture(){
  if(!pin){toast('Entre le PIN');return}
  if(capturePending) return;

  captureBefore=latestCameraName;
  capturePending=true;
  $('cameraState').textContent='Envoi du déclenchement…';

  try{
    const r=await fetch('/api/camera/capture',{
      method:'POST',
      headers:headers(),
      cache:'no-store'
    });
    if(!r.ok) throw new Error('Capture refusée');

    $('cameraState').textContent='Commande reçue par la 3DS…';
    // Le pump unique détectera la nouvelle photo sans lancer 24 requêtes
    // supplémentaires en parallèle.
    cameraPump();
  }catch(e){
    capturePending=false;
    $('cameraState').textContent=e.message;
    toast(e.message);
  }
}

async function saveCameraPhoto(name){
  try{
    const blob=await fetchCameraBlob(name);
    const a=document.createElement('a');
    a.href=URL.createObjectURL(blob);
    a.download=name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(()=>URL.revokeObjectURL(a.href),1500);
  }catch(e){toast(e.message)}
}

async function deleteCameraPhoto(name){
  if(!confirm('Supprimer cette photo de la 3DS ?')) return;
  const r=await fetch('/api/camera/delete?name='+encodeURIComponent(name),{
    method:'POST',
    headers:headers()
  });
  if(r.ok){
    if(name===latestCameraName){
      latestCameraName='';
      if(latestCameraUrl) URL.revokeObjectURL(latestCameraUrl);
      latestCameraUrl='';
      $('cameraPreview').innerHTML='';
    }
    await loadCamera();
    toast('Photo supprimée');
  }else{
    toast('Suppression impossible');
  }
}

if(pin) setTimeout(unlock,250);
</script>
</body>
</html>)HTML";
}

void stopServer() {
    closeClient();

    if (serverSocket >= 0) {
        close(serverSocket);
        serverSocket = -1;
    }

    if (socStarted) {
        socExit();
        socStarted = false;
    }

    if (socBuffer) {
        free(socBuffer);
        socBuffer = nullptr;
    }

    serverReady = false;
}

void generatePin() {
    pinCode = 1000 + static_cast<unsigned int>((osGetTime() ^ svcGetSystemTick()) % 9000);
}

bool startServer() {
    stopServer();
    ensureDirectories();

    clientSeen = false;
    requestCount = 0;
    lastClient = "Aucun iPhone connecte";
    statusMessage = "Initialisation du reseau...";
    lastAction = "En attente d'une action iPhone";
    generatePin();

    socBuffer = static_cast<u32*>(memalign(SOC_ALIGN, SOC_BUFFERSIZE));
    if (!socBuffer) {
        statusMessage = "Erreur memoire reseau";
        return false;
    }

    const Result result = socInit(socBuffer, SOC_BUFFERSIZE);
    if (R_FAILED(result)) {
        char buffer[64];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "socInit: 0x%08lX",
            static_cast<unsigned long>(result)
        );
        statusMessage = buffer;
        free(socBuffer);
        socBuffer = nullptr;
        return false;
    }

    socStarted = true;

    const u32 hostId = gethostid();
    in_addr address{};
    address.s_addr = hostId;
    localIp = inet_ntoa(address);

    if (hostId == 0) {
        statusMessage = "Connecte la 3DS au Wi-Fi";
        qrReady = false;
        return false;
    }

    generateConnectionQr();

    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (serverSocket < 0) {
        statusMessage = std::string("socket: ") + strerror(errno);
        return false;
    }

    int yes = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);
    server.sin_addr.s_addr = hostId;

    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&server), sizeof(server)) != 0) {
        statusMessage = std::string("bind: ") + strerror(errno);
        return false;
    }

    const int flags = fcntl(serverSocket, F_GETFL, 0);
    fcntl(serverSocket, F_SETFL, flags | O_NONBLOCK);

    if (listen(serverSocket, 5) != 0) {
        statusMessage = std::string("listen: ") + strerror(errno);
        return false;
    }

    serverReady = true;
    statusMessage = "Serveur pret - ouvre Safari";
    return true;
}

bool sendFileFromDirectory(int sock, const char* directory, const std::string& name, const char* contentType) {
    const std::string safe = sanitizeFilename(name);
    const std::string path = std::string(directory) + "/" + safe;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        sendSimple(sock, 404, "Not Found", "Fichier introuvable");
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        sendSimple(sock, 500, "Internal Server Error", "Erreur fichier");
        return false;
    }
    char header[512];
    const int headerLength = std::snprintf(
        header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        contentType, size
    );
    if (headerLength <= 0 || !sendAll(sock, header, static_cast<size_t>(headerLength))) {
        fclose(file); return false;
    }
    std::vector<char> buffer(IO_CHUNK);
    while (!feof(file)) {
        const size_t got = fread(buffer.data(), 1, buffer.size(), file);
        if (!got) break;
        if (!sendAll(sock, buffer.data(), got)) break;
    }
    fclose(file);
    return true;
}

bool sendDownload(int sock, const std::string& name) {
    const std::string safe = sanitizeFilename(name);
    const std::string path = std::string(INBOX_DIR) + "/" + safe;

    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        sendSimple(sock, 404, "Not Found", "Fichier introuvable");
        return false;
    }

    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size < 0) {
        fclose(file);
        sendSimple(sock, 500, "Internal Server Error", "Erreur fichier");
        return false;
    }

    char header[512];
    const int headerLength = std::snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %ld\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        size,
        safe.c_str()
    );

    if (headerLength <= 0 ||
        !sendAll(sock, header, static_cast<size_t>(headerLength))) {
        fclose(file);
        return false;
    }

    std::vector<char> buffer(IO_CHUNK);

    while (!feof(file)) {
        const size_t got = fread(buffer.data(), 1, buffer.size(), file);
        if (got == 0) break;
        if (!sendAll(sock, buffer.data(), got)) break;
    }

    fclose(file);
    return true;
}

void handleClient(sockaddr_in& client) {
    std::string received;
    size_t bodyOffset = 0;

    if (!readRequestHeaders(clientSocket, received, bodyOffset)) {
        sendSimple(clientSocket, 400, "Bad Request", "Requete invalide");
        return;
    }

    const std::string headers = received.substr(0, bodyOffset);

    char methodBuffer[16]{};
    char targetBuffer[2048]{};

    if (std::sscanf(headers.c_str(), "%15s %2047s", methodBuffer, targetBuffer) != 2) {
        sendSimple(clientSocket, 400, "Bad Request", "Ligne HTTP invalide");
        return;
    }

    const std::string method = methodBuffer;
    const std::string target = targetBuffer;
    const std::string route = pathOnly(target);

    clientSeen = true;
    ++requestCount;
    lastClient = inet_ntoa(client.sin_addr);

    if (route == "/" && method == "GET") {
        const std::string page = makeWebPage();
        sendSimple(clientSocket, 200, "OK", page, "text/html; charset=utf-8");
        statusMessage = "iPhone connecte";
        return;
    }

    if (route == "/favicon.ico") {
        sendSimple(clientSocket, 204, "No Content", "");
        return;
    }

    if (route == "/manifest.webmanifest" && method == "GET") {
        sendSimple(
            clientSocket,
            200,
            "OK",
            "{\"name\":\"3DS Link\",\"short_name\":\"3DS Link\",\"start_url\":\"/\",\"display\":\"standalone\",\"background_color\":\"#edf4f8\",\"theme_color\":\"#0b84d8\",\"icons\":[{\"src\":\"/app-icon.svg\",\"sizes\":\"any\",\"type\":\"image/svg+xml\"}]}",
            "application/manifest+json"
        );
        return;
    }

    if (route == "/app-icon.svg" && method == "GET") {
        sendSimple(
            clientSocket,
            200,
            "OK",
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 512 512\"><defs><linearGradient id=\"g\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\"><stop stop-color=\"#58c7f5\"/><stop offset=\"1\" stop-color=\"#087bd0\"/></linearGradient></defs><rect width=\"512\" height=\"512\" rx=\"116\" fill=\"url(#g)\"/><rect x=\"87\" y=\"134\" width=\"338\" height=\"244\" rx=\"52\" fill=\"white\" opacity=\".96\"/><circle cx=\"172\" cy=\"256\" r=\"42\" fill=\"#0b84d8\"/><circle cx=\"340\" cy=\"222\" r=\"18\" fill=\"#0b84d8\"/><circle cx=\"382\" cy=\"264\" r=\"18\" fill=\"#0b84d8\"/><path d=\"M150 328h212\" stroke=\"#8ed2f3\" stroke-width=\"24\" stroke-linecap=\"round\"/></svg>",
            "image/svg+xml"
        );
        return;
    }

    if (!authorized(headers)) {
        sendSimple(clientSocket, 401, "Unauthorized", "{\"error\":\"pin\"}", "application/json");
        lastAction = "Tentative refusee : PIN incorrect";
        return;
    }

    // En mode Camera, on garde les commandes légères (état, capture, clavier,
    // remote) mais on repousse les gros transferts SD pour protéger les 30 fps.
    if (cameraMode &&
        (route == "/upload" ||
         route == "/download" ||
         route == "/delete")) {
        sendSimple(
            clientSocket,
            423,
            "Locked",
            "{\"error\":\"camera_busy\",\"message\":\"Quitte Camera Link pour les gros transferts.\"}",
            "application/json; charset=utf-8"
        );
        return;
    }

    if (route == "/api/files" && method == "GET") {
        sendSimple(clientSocket, 200, "OK", filesJson(), "application/json; charset=utf-8");
        lastAction = "Liste des fichiers consultee";
        return;
    }

    if (route == "/upload" && method == "POST") {
        const std::string lengthValue = headerValue(headers, "Content-Length");
        const long long contentLength = lengthValue.empty()
            ? -1
            : std::strtoll(lengthValue.c_str(), nullptr, 10);

        if (contentLength < 0 || contentLength > MAX_UPLOAD) {
            sendSimple(clientSocket, 413, "Payload Too Large", "Fichier trop grand ou taille invalide");
            lastAction = "Upload refuse : taille invalide";
            return;
        }

        const std::string safeName = sanitizeFilename(getQueryValue(target, "name"));
        const std::string path = std::string(INBOX_DIR) + "/" + safeName;

        FILE* output = fopen(path.c_str(), "wb");
        if (!output) {
            sendSimple(clientSocket, 500, "Internal Server Error", "Impossible d'ecrire sur la SD");
            lastAction = "Erreur ecriture carte SD";
            return;
        }

        statusMessage = "Reception : " + safeName;

        const bool ok = receiveBodyToFile(
            clientSocket,
            received,
            bodyOffset,
            contentLength,
            output
        );

        fclose(output);

        if (!ok) {
            remove(path.c_str());
            sendSimple(clientSocket, 500, "Internal Server Error", "Transfert interrompu");
            lastAction = "Transfert interrompu";
            statusMessage = "Erreur de transfert";
            return;
        }

        ++uploadCount;
        lastAction = "Recu : " + safeName;
        statusMessage = "Fichier recu avec succes";
        sendSimple(clientSocket, 200, "OK", "{\"ok\":true}", "application/json");
        return;
    }

    if (route == "/download" && method == "GET") {
        const std::string name = sanitizeFilename(getQueryValue(target, "name"));
        lastAction = "Envoi vers iPhone : " + name;
        sendDownload(clientSocket, name);
        return;
    }

    if (route == "/delete" && method == "POST") {
        const std::string name = sanitizeFilename(getQueryValue(target, "name"));
        const std::string path = std::string(INBOX_DIR) + "/" + name;

        if (remove(path.c_str()) == 0) {
            lastAction = "Supprime : " + name;
            sendSimple(clientSocket, 200, "OK", "{\"ok\":true}", "application/json");
        } else {
            sendSimple(clientSocket, 404, "Not Found", "{\"ok\":false}", "application/json");
        }
        return;
    }

    if (route == "/camera/live.bmp" && method == "GET") {
        sendLiveCameraBmp(clientSocket);
        return;
    }

    if (route == "/camera/last-preview.bmp" && method == "GET") {
        sendLastCapturedPreviewBmp(clientSocket);
        return;
    }

    if (route == "/api/camera" && method == "GET") {
        sendSimple(clientSocket, 200, "OK", cameraJson(), "application/json; charset=utf-8");
        return;
    }

    if (route == "/api/camera/capture" && method == "POST") {
        cameraCaptureRequested = true;
        cameraMode = true;
        cameraStatus = "Capture iPhone en attente";
        sendSimple(clientSocket, 202, "Accepted", "{\"accepted\":true}", "application/json");
        return;
    }

    if (route == "/camera/file" && method == "GET") {
        const std::string name = getQueryValue(target, "name");
        sendFileFromDirectory(clientSocket, CAMERA_DIR, name, "image/bmp");
        return;
    }

    if (route == "/api/camera/delete" && method == "POST") {
        const std::string name = sanitizeFilename(getQueryValue(target, "name"));
        const std::string path = std::string(CAMERA_DIR) + "/" + name;
        if (remove(path.c_str()) == 0) {
            cameraSessionPhotos.erase(
                std::remove_if(
                    cameraSessionPhotos.begin(),
                    cameraSessionPhotos.end(),
                    [&](const FileEntry& entry) { return entry.name == name; }
                ),
                cameraSessionPhotos.end()
            );
            if (lastPhotoName == name) {
                lastPhotoName = cameraSessionPhotos.empty()
                    ? ""
                    : cameraSessionPhotos.front().name;
                lastCapturedFrameReady = false;
            }
            sendSimple(clientSocket, 200, "OK", "{\"ok\":true}", "application/json");
        } else {
            sendSimple(clientSocket, 404, "Not Found", "{\"ok\":false}", "application/json");
        }
        return;
    }

    if (route == "/api/text" && method == "POST") {
        const std::string lengthValue = headerValue(headers, "Content-Length");
        const long long contentLength = lengthValue.empty()
            ? 0
            : std::strtoll(lengthValue.c_str(), nullptr, 10);

        lastText = receiveBodyToString(
            clientSocket,
            received,
            bodyOffset,
            contentLength
        );

        if (lastText.size() > 180) lastText.resize(180);
        lastAction = "Texte recu depuis l'iPhone";
        sendSimple(clientSocket, 200, "OK", "{\"ok\":true}", "application/json");
        return;
    }

    if (route == "/api/remote" && method == "POST") {
        lastRemote = sanitizeFilename(getQueryValue(target, "key"));
        if (lastRemote.size() > 12) lastRemote.resize(12);
        lastAction = "Commande iPhone : " + lastRemote;
        sendSimple(clientSocket, 200, "OK", "{\"ok\":true}", "application/json");
        return;
    }

    sendSimple(clientSocket, 404, "Not Found", "Route inconnue");
}

void pollServer(int maxClients = 3) {
    if (!serverReady || serverSocket < 0) return;

    for (int handled = 0; handled < maxClients; ++handled) {
        sockaddr_in client{};
        socklen_t clientLength = sizeof(client);

        clientSocket = accept(
            serverSocket,
            reinterpret_cast<sockaddr*>(&client),
            &clientLength
        );

        if (clientSocket < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                statusMessage = std::string("accept: ") + strerror(errno);
            }
            return;
        }
        // libctru ne fournit pas SO_RCVTIMEO/SO_SNDTIMEO ici.
        // On garde la stratégie non bloquante/budgetée sans ces options.

        const int flags = fcntl(clientSocket, F_GETFL, 0);
        fcntl(clientSocket, F_SETFL, flags & ~O_NONBLOCK);

        handleClient(client);
        closeClient();
    }
}



bool openSelectedHomeItem() {
    if (homeSelection == 3) {
        cameraMode = true;
        cameraCaptureRequested = false;
        cameraStatus = "Demarrage du viseur...";
        gfxSetDoubleBuffering(GFX_TOP, true);
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        return true;
    }

    if (!serverReady) startServer();

    if (homeSelection == 0) lastAction = "Fichiers : ouvre la page iPhone";
    if (homeSelection == 1) lastAction = "Clavier : utilise l'iPhone";
    if (homeSelection == 2) lastAction = "Remote : utilise l'iPhone";
    if (homeSelection == 4) lastAction = "Infos connexion affichees";

    return false;
}

int touchedHomeTile(const touchPosition& touch) {
    const int y0 = 78;
    const int y1 = 145;
    if (touch.py < y0 || touch.py > y1) return -1;

    for (int i = 0; i < HOME_ITEM_COUNT; ++i) {
        const int x0 = 5 + i * 63;
        const int x1 = x0 + 62;
        if (touch.px >= x0 && touch.px <= x1) return i;
    }
    return -1;
}

bool homeOpenButtonTouched(const touchPosition& touch) {
    return touch.px >= 32 && touch.px <= 288 &&
           touch.py >= 176 && touch.py <= 222;
}

u32 twilightTileColor(int index) {
    static const u32 colors[HOME_ITEM_COUNT] = {
        C2D_Color32(75, 170, 226, 255),
        C2D_Color32(244, 190, 71, 255),
        C2D_Color32(102, 191, 126, 255),
        C2D_Color32(227, 100, 109, 255),
        C2D_Color32(154, 126, 205, 255)
    };
    return colors[index % HOME_ITEM_COUNT];
}

void drawTwilightIconGlyph(int index, float cx, float cy, u32 fg) {
    // Petits pictogrammes géométriques originaux, sans reprendre d'asset Nintendo.
    if (index == 0) {
        C2D_DrawRectSolid(cx - 12, cy - 7, 0.7f, 24, 16, fg);
        C2D_DrawRectSolid(cx - 8, cy - 11, 0.71f, 12, 5, fg);
    } else if (index == 1) {
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 4; ++x)
                C2D_DrawRectSolid(cx - 14 + x * 8, cy - 9 + y * 8, 0.7f, 5, 5, fg);
    } else if (index == 2) {
        C2D_DrawCircleSolid(cx, cy, 0.7f, 14, fg);
        C2D_DrawCircleSolid(cx, cy, 0.71f, 8, C2D_Color32(255,255,255,255));
        C2D_DrawCircleSolid(cx, cy, 0.72f, 4, fg);
    } else if (index == 3) {
        drawRoundedRect(cx - 15, cy - 10, 30, 20, 4, fg, 0.7f);
        C2D_DrawCircleSolid(cx, cy, 0.72f, 6, C2D_Color32(255,255,255,255));
        C2D_DrawRectSolid(cx - 8, cy - 14, 0.71f, 10, 5, fg);
    } else {
        C2D_DrawCircleSolid(cx, cy, 0.7f, 14, fg);
        drawCenteredText("i", cx, cy - 11, 0.50f, C2D_Color32(255,255,255,255));
    }
}

void drawTwilightTile(int index, float x, float y, bool selected) {
    const float w = 52.0f;
    const float h = 52.0f;

    if (selected) {
        // Cadre épais clair + liseré jaune, comme la sélection très lisible de TWiLight.
        drawRoundedRect(x - 5, y - 5, w + 10, h + 10, 9, C2D_Color32(255, 212, 61, 255), 0.30f);
        drawRoundedRect(x - 2, y - 2, w + 4, h + 4, 7, COLOR_WHITE, 0.35f);
    }

    drawRoundedRect(x + 2, y + 3, w, h, 7, C2D_Color32(74, 90, 101, 45), 0.39f);
    drawRoundedRect(x, y, w, h, 7, twilightTileColor(index), 0.40f);
    drawTwilightIconGlyph(index, x + w / 2.0f, y + h / 2.0f, COLOR_WHITE);
}

void drawHomeTopScreen() {
    C2D_TargetClear(topTarget, C2D_Color32(238, 241, 237, 255));
    C2D_SceneBegin(topTarget);

    // Barre supérieure à fort contraste.
    C2D_DrawRectSolid(0, 0, 0.10f, 400, 27, C2D_Color32(250, 251, 249, 255));
    C2D_DrawRectSolid(0, 26, 0.12f, 400, 1, C2D_Color32(155, 163, 158, 255));

    drawText("3DS Link", 12, 4, 0.48f, COLOR_TEXT);
    C2D_DrawCircleSolid(281, 13, 0.5f, 5, serverReady ? COLOR_GREEN : COLOR_RED);
    drawText(serverReady ? "Connecte" : "Hors ligne", 292, 5, 0.33f,
             serverReady ? C2D_Color32(33, 126, 66, 255) : COLOR_RED);
    drawText("v1.4.1", 350, 5, 0.29f, COLOR_TEXT);

    // Carte gauche : fonction sélectionnée.
    drawRoundedRect(13, 38, 225, 153, 14, COLOR_WHITE, 0.20f);

    drawRoundedRect(29, 54, 82, 82, 13, twilightTileColor(homeSelection), 0.35f);
    drawTwilightIconGlyph(homeSelection, 70, 95, COLOR_WHITE);

    drawText(HOME_NAMES[homeSelection], 126, 55, 0.57f, COLOR_TEXT);

    std::string desc = HOME_DESCRIPTIONS[homeSelection];
    if (desc.size() > 28) desc.resize(28);
    drawText(desc, 126, 87, 0.32f, COLOR_TEXT);

    if (serverReady) {
        drawText(localIp + ":" + std::to_string(PORT), 126, 115, 0.31f, COLOR_TEXT);
        drawText("PIN " + std::to_string(pinCode), 126, 139, 0.38f, COLOR_ORANGE);
        drawText("Scanne le QR →", 126, 164, 0.29f, COLOR_MUTED);
    } else {
        drawText("Serveur indisponible", 126, 122, 0.31f, COLOR_RED);
        drawText("A : reessayer", 126, 148, 0.31f, COLOR_TEXT);
    }

    // Grande carte QR : modules >= 2 px, beaucoup plus fiable à scanner.
    drawRoundedRect(248, 38, 141, 153, 14, COLOR_WHITE, 0.20f);
    drawCenteredText("CONNEXION iPHONE", 318, 47, 0.25f, COLOR_TEXT);

    if (serverReady && qrReady) {
        drawConnectionQr(318, 116, 126.0f);
        drawCenteredText("IP + PIN inclus", 318, 177, 0.24f, COLOR_MUTED);
    } else {
        drawCenteredText("QR", 318, 92, 0.50f, COLOR_MUTED);
        drawCenteredText("indisponible", 318, 120, 0.28f, COLOR_MUTED);
    }

    C2D_DrawRectSolid(0, 204, 0.11f, 400, 36, C2D_Color32(248, 249, 247, 255));
    C2D_DrawRectSolid(0, 204, 0.12f, 400, 1, C2D_Color32(150, 159, 154, 255));
    drawText("< > Choisir", 16, 214, 0.35f, COLOR_TEXT);
    drawCenteredText("A Ouvrir", 200, 214, 0.37f, COLOR_TEXT);
    drawText("QR = connexion", 292, 214, 0.30f, COLOR_TEXT);
}

void drawHomeBottomScreen() {
    C2D_TargetClear(bottomTarget, C2D_Color32(229, 233, 229, 255));
    C2D_SceneBegin(bottomTarget);

    C2D_DrawRectSolid(0, 0, 0.10f, 320, 32, C2D_Color32(250, 251, 249, 255));
    C2D_DrawRectSolid(0, 31, 0.12f, 320, 1, C2D_Color32(150, 159, 154, 255));
    drawCenteredText("3DS Link", 160, 5, 0.49f, COLOR_TEXT);

    C2D_DrawCircleSolid(16, 15, 0.3f, 5, clientSeen ? COLOR_GREEN : COLOR_ORANGE);
    drawText(clientSeen ? "iPhone connecte" : "En attente iPhone", 26, 6, 0.30f, COLOR_TEXT);

    drawCenteredText("Touchez une icone", 160, 43, 0.31f, COLOR_TEXT);

    const float startX = 8.0f;
    const float gap = 63.0f;
    const float y = 78.0f;
    for (int i = 0; i < HOME_ITEM_COUNT; ++i) {
        drawTwilightTile(i, startX + gap * i, y, i == homeSelection);
    }

    drawCenteredText(HOME_NAMES[homeSelection], 160, 145, 0.52f, COLOR_TEXT);

    drawRoundedRect(31, 174, 252, 43, 12,
                    homeSelection == 3 ? C2D_Color32(75,170,226,255) : COLOR_WHITE,
                    0.24f);
    drawCenteredText(
        homeSelection == 3 ? "Ouvrir Camera Link" : "Afficher",
        157, 184, 0.40f,
        homeSelection == 3 ? COLOR_WHITE : COLOR_TEXT
    );

    for (int i = 0; i < HOME_ITEM_COUNT; ++i) {
        C2D_DrawCircleSolid(
            136 + i * 12, 229, 0.4f,
            i == homeSelection ? 4 : 2,
            i == homeSelection ? C2D_Color32(49,119,171,255) : C2D_Color32(92,100,96,255)
        );
    }
}

void drawCameraTopScreen() {
    C2D_TargetClear(topTarget, C2D_Color32(10, 12, 14, 255));
    C2D_SceneBegin(topTarget);

    drawCenteredText("Demarrage de la camera...", 200, 103, 0.50f, COLOR_WHITE);
    drawCenteredText("3DS Link v1.4.1", 200, 132, 0.34f, COLOR_MUTED);
}

void drawCameraBottomScreen() {
    C2D_TargetClear(bottomTarget, C2D_Color32(231, 235, 231, 255));
    C2D_SceneBegin(bottomTarget);

    C2D_DrawRectSolid(0, 0, 0.1f, 320, 31, C2D_Color32(247, 249, 246, 255));
    C2D_DrawRectSolid(0, 30, 0.2f, 320, 1, C2D_Color32(184, 194, 189, 255));
    drawCenteredText("Camera Link", 160, 5, 0.45f, COLOR_TEXT);

    C2D_DrawCircleSolid(18, 16, 0.4f, 5, cameraHasFrame ? COLOR_GREEN : COLOR_ORANGE);
    drawText(cameraHasFrame ? "LIVE" : "...", 27, 7, 0.29f, COLOR_MUTED);

    // Carte compteur à gauche
    drawRoundedRect(16, 48, 104, 68, 12, COLOR_WHITE, 0.22f);
    drawText("SESSION", 30, 59, 0.29f, COLOR_MUTED);
    drawCenteredText(std::to_string(cameraPhotoCount), 68, 78, 0.74f, COLOR_BLUE_DARK);

    // Dernière photo
    drawRoundedRect(134, 48, 170, 68, 12, COLOR_WHITE, 0.22f);
    drawText("DERNIERE PHOTO", 147, 59, 0.28f, COLOR_MUTED);
    if (!lastPhotoName.empty())
        drawText(lastPhotoName.substr(0, 22), 147, 82, 0.28f, COLOR_TEXT);
    else
        drawText("Aucune", 147, 82, 0.31f, COLOR_MUTED);

    // Déclencheur façon icône de launcher
    drawRoundedRect(120, 132, 80, 70, 14, C2D_Color32(224, 229, 226, 255), 0.28f);
    drawRoundedRect(124, 128, 72, 70, 14, COLOR_WHITE, 0.31f);
    C2D_DrawCircleSolid(160, 163, 0.4f, 25, C2D_Color32(75, 170, 226, 255));
    C2D_DrawCircleSolid(160, 163, 0.5f, 16, COLOR_WHITE);
    C2D_DrawCircleSolid(160, 163, 0.6f, 10, C2D_Color32(75, 170, 226, 255));

    drawText("B Retour", 15, 216, 0.32f, COLOR_MUTED);
    drawCenteredText("A Photo", 160, 216, 0.34f, COLOR_TEXT);
    drawText("Y Accueil", 247, 216, 0.32f, COLOR_MUTED);
}

void refreshCameraBottomUi() {
    // Pendant Camera Link, l'écran du bas n'est pas double-bufferisé.
    // On peut donc le mettre à jour sans toucher au framebuffer brut du haut.
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    drawCameraBottomScreen();
    C3D_FrameEnd(0);
    C2D_TextBufClear(textBuffer);
}

void drawTopScreen() {
    if (cameraMode) drawCameraTopScreen();
    else drawHomeTopScreen();
}

void drawBottomScreen() {
    if (cameraMode) drawCameraBottomScreen();
    else drawHomeBottomScreen();
}


void primeCameraUiBuffers() {
    // Le framebuffer brut du haut va être swappé directement ensuite.
    // On remplit donc les DEUX buffers de l'écran inférieur avec exactement
    // la même interface afin qu'il reste parfaitement fixe pendant les swaps.
    for (int i = 0; i < 2; ++i) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        C2D_TargetClear(topTarget, C2D_Color32(10, 12, 14, 255));
        C2D_SceneBegin(topTarget);
        drawCenteredText("Demarrage de la camera...", 200, 103, 0.50f, COLOR_WHITE);

        drawCameraBottomScreen();

        C3D_FrameEnd(0);
        C2D_TextBufClear(textBuffer);
    }
}

bool cameraTouchPressed() {
    touchPosition touch{};
    hidTouchRead(&touch);
    const int dx = static_cast<int>(touch.px) - 160;
    const int dy = static_cast<int>(touch.py) - 164;
    return dx * dx + dy * dy <= 42 * 42;
}

bool initGraphics() {
    gfxInitDefault();

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return false;

    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        C3D_Fini();
        return false;
    }

    C2D_Prepare();

    topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuffer = C2D_TextBufNew(4096);

    return topTarget && bottomTarget && textBuffer;
}

void shutdownGraphics() {
    if (textBuffer) {
        C2D_TextBufDelete(textBuffer);
        textBuffer = nullptr;
    }

    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

} // namespace

int main() {
    if (!initGraphics()) return 1;

    startServer();

    bool cameraUiPrimed = false;

    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();

        if (down & KEY_START) break;

        if (cameraMode) {
            // Le moteur caméra v0.8 reste prioritaire ; le réseau est servi une requête par frame.
            if (!cameraUiPrimed) {
                primeCameraUiBuffers();
                cameraUiPrimed = true;
            }

            if (!cameraActive) {
                if (!startCameraStream()) {
                    cameraMode = false;
                    cameraUiPrimed = false;
                    continue;
                }
            }

            if (down & KEY_B || down & KEY_Y) {
                cameraMode = false;
                cameraUiPrimed = false;
                stopCameraStream();
                gfxSetDoubleBuffering(GFX_TOP, true);
                gfxSetDoubleBuffering(GFX_BOTTOM, true);
                cameraStatus = "Pret a prendre une photo";
                continue;
            }

            if ((down & KEY_A) || ((down & KEY_TOUCH) && cameraTouchPressed())) {
                cameraCaptureRequested = true;
            }

            static unsigned int cameraNetworkDivider = 0;

            if (receiveCameraFrame()) {
                if (cameraCaptureRequested && cameraWarmupFrames >= 5) {
                    cameraCaptureRequested = false;

                    if (captureCameraPhoto()) {
                        // Le compteur et le nom de la dernière photo changent
                        // immédiatement sur l'écran inférieur.
                        refreshCameraBottomUi();
                    }
                }

                presentCameraFrame();
            }

            // Une requête au maximum par frame caméra : latence typique
            // d'une frame (~33 ms) sans créer de rafale réseau.
            (void)cameraNetworkDivider;
            pollServer(1);

            // Aucun rendu Citro2D/Citro3D sur l'écran supérieur : le viseur v0.8
            // reste intact.
            continue;
        }

        cameraUiPrimed = false;

        if (down & KEY_LEFT) {
            homeSelection = (homeSelection + HOME_ITEM_COUNT - 1) % HOME_ITEM_COUNT;
        }
        if (down & KEY_RIGHT) {
            homeSelection = (homeSelection + 1) % HOME_ITEM_COUNT;
        }

        if (down & KEY_TOUCH) {
            touchPosition touch{};
            hidTouchRead(&touch);

            const int touched = touchedHomeTile(touch);
            if (touched >= 0) {
                if (homeSelection == touched) {
                    if (openSelectedHomeItem()) continue;
                } else {
                    homeSelection = touched;
                }
            } else if (homeOpenButtonTouched(touch)) {
                if (openSelectedHomeItem()) continue;
            }
        }

        if (down & KEY_A) {
            if (openSelectedHomeItem()) continue;
        }

        if (down & KEY_Y) {
            cameraMode = true;
            cameraCaptureRequested = false;
            cameraStatus = "Demarrage du viseur...";
            gfxSetDoubleBuffering(GFX_TOP, true);
            gfxSetDoubleBuffering(GFX_BOTTOM, false);
            continue;
        }

        if (down & KEY_X) {
            generatePin();
            lastAction = "Nouveau code PIN genere";
        }

        // Hors caméra, on peut vider plusieurs requêtes en attente.
        pollServer(4);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        drawHomeTopScreen();
        drawHomeBottomScreen();
        C3D_FrameEnd(0);

        C2D_TextBufClear(textBuffer);
    }

    stopCameraStream();
    stopServer();
    shutdownGraphics();
    return 0;
}
