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
constexpr int QR_MAX_VERSION = 5;
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
constexpr u32 COLOR_TEXT        = C2D_Color32(43, 57, 67, 255);
constexpr u32 COLOR_MUTED       = C2D_Color32(105, 119, 129, 255);
constexpr u32 COLOR_WHITE       = C2D_Color32(255, 255, 255, 255);
constexpr u32 COLOR_GREEN       = C2D_Color32(57, 176, 103, 255);
constexpr u32 COLOR_RED         = C2D_Color32(202, 73, 79, 255);
constexpr u32 COLOR_ORANGE      = C2D_Color32(224, 145, 55, 255);
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

C3D_RenderTarget* topTarget = nullptr;
C3D_RenderTarget* bottomTarget = nullptr;
C2D_TextBuf textBuffer = nullptr;

struct FileEntry {
    std::string name;
    long long size = 0;
};

std::string connectionUrl() {
    return "http://" + localIp + ":" + std::to_string(PORT);
}

void generateConnectionQr() {
    const std::string url = connectionUrl();
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

    for (int y = CAMERA_HEIGHT - 1; y >= 0; --y) {
        for (int x = 0; x < CAMERA_WIDTH; ++x) {
            const u16 data = pixels[y * CAMERA_WIDTH + x];
            const u8 b = static_cast<u8>(((data >> 11) & 0x1F) << 3);
            const u8 g = static_cast<u8>(((data >> 5) & 0x3F) << 2);
            const u8 r = static_cast<u8>((data & 0x1F) << 3);
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
    cameraStatus = "Initialisation de la camera...";

    std::vector<u8> frame(CAMERA_FRAME_BYTES);
    if (frame.empty()) {
        cameraStatus = "Memoire insuffisante";
        return false;
    }

    Result result = camInit();
    if (R_FAILED(result)) {
        cameraStatus = "Impossible d'initialiser la camera";
        return false;
    }

    bool ok = true;
    Handle receiveEvent = 0;
    u32 transferBytes = 0;

    if (R_FAILED(CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A))) ok = false;
    if (ok && R_FAILED(CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A))) ok = false;
    if (ok) CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30);
    if (ok) CAMU_SetNoiseFilter(SELECT_OUT1, true);
    if (ok) CAMU_SetAutoExposure(SELECT_OUT1, true);
    if (ok) CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
    if (ok) CAMU_SetTrimming(PORT_CAM1, false);
    if (ok && R_FAILED(CAMU_GetMaxBytes(&transferBytes, CAMERA_WIDTH, CAMERA_HEIGHT))) ok = false;
    if (ok && R_FAILED(CAMU_SetTransferBytes(PORT_CAM1, transferBytes, CAMERA_WIDTH, CAMERA_HEIGHT))) ok = false;
    if (ok && R_FAILED(CAMU_Activate(SELECT_OUT1))) ok = false;

    if (ok) {
        CAMU_ClearBuffer(PORT_CAM1);
        if (R_FAILED(CAMU_StartCapture(PORT_CAM1))) ok = false;
    }

    if (ok && R_FAILED(CAMU_SetReceiving(
        &receiveEvent,
        frame.data(),
        PORT_CAM1,
        CAMERA_FRAME_BYTES,
        static_cast<s16>(transferBytes)
    ))) {
        ok = false;
    }

    if (ok) {
        cameraStatus = "Capture en cours...";
        const Result wait = svcWaitSynchronization(receiveEvent, CAMERA_WAIT_TIMEOUT);
        if (R_FAILED(wait)) ok = false;
    }

    if (ok) CAMU_PlayShutterSound(SHUTTER_SOUND_TYPE_NORMAL);
    CAMU_StopCapture(PORT_CAM1);
    if (receiveEvent) svcCloseHandle(receiveEvent);
    CAMU_Activate(SELECT_NONE);
    camExit();

    if (!ok) {
        cameraStatus = "Echec de la capture";
        lastAction = "Camera : capture echouee";
        return false;
    }

    ++cameraPhotoCount;
    const std::string name = makeCameraFilename();
    const std::string path = std::string(CAMERA_DIR) + "/" + name;

    if (!saveCameraBmp(path, frame.data())) {
        cameraStatus = "Erreur d'ecriture sur la carte SD";
        lastAction = "Camera : erreur SD";
        return false;
    }

    lastPhotoName = name;
    cameraStatus = "Photo prise - envoyee au site";
    lastAction = "Camera : " + name;
    return true;
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
    const auto files = listCameraPhotos();
    std::string json = "{\"photos\":[";
    for (size_t i = 0; i < files.size(); ++i) {
        if (i) json += ",";
        json += "{\"name\":\"" + jsonEscape(files[i].name) + "\",\"size\":" +
                std::to_string(files[i].size) + "}";
    }
    json += "],\"count\":" + std::to_string(files.size()) +
            ",\"latest\":\"" + jsonEscape(lastPhotoName) + "\"}";
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
<meta name="theme-color" content="#459ed6">
<title>3DS Link</title>
<style>
:root{
  color-scheme:light;
  font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display","Segoe UI",Arial,sans-serif;
  --blue:#459ed6;--blue2:#236da5;--bg:#eef4f8;--card:#fff;--line:#c6d5de;
  --text:#273640;--muted:#71838e;--green:#39ad67;--red:#c94a51;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);min-height:100vh}
header{background:linear-gradient(180deg,#7fc9ed,var(--blue));color:#fff;border-bottom:1px solid var(--blue2);padding:18px 16px 16px}
.wrap{max-width:720px;margin:auto}
.headrow{display:flex;justify-content:space-between;align-items:center;gap:12px}
h1{font-size:28px;letter-spacing:-.5px;margin:0}.small{font-size:13px;opacity:.92}
.status{margin:14px;background:#fff;border:1px solid var(--line);border-radius:16px;padding:14px;box-shadow:0 4px 14px #4f6d8018}
.dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:var(--green);margin-right:7px;box-shadow:0 0 0 4px #39ad6722}
.pinbox{display:flex;gap:8px;margin-top:12px}
input,button,select{font:inherit}
input[type=text],input[type=password]{width:100%;border:1px solid #b8cbd7;border-radius:11px;padding:12px;background:#fbfdfe;color:var(--text);outline:none}
input:focus{border-color:var(--blue);box-shadow:0 0 0 3px #459ed622}
button{border:0;border-radius:11px;padding:11px 14px;font-weight:700;background:var(--blue);color:#fff}
button.secondary{background:#e7eff4;color:#315267;border:1px solid #c9d8e1}
button.danger{background:#fff0f1;color:#a8373e;border:1px solid #efc6c9}
button:disabled{opacity:.45}
.tabs{display:grid;grid-template-columns:repeat(5,1fr);gap:7px;padding:0 14px 12px}
.tab{background:#e4edf3;color:#45606f;border:1px solid #ccd9e1;padding:9px 5px;font-size:12px}
.tab.active{background:#fff;color:#236da5;border-color:#9ccae5}
.panel{display:none;margin:0 14px 14px;background:#fff;border:1px solid var(--line);border-radius:16px;padding:15px;box-shadow:0 4px 14px #4f6d8012}
.panel.active{display:block}
h2{font-size:19px;margin:0 0 5px}.muted{color:var(--muted);font-size:13px}
.drop{margin-top:14px;border:2px dashed #acd0e5;border-radius:14px;padding:18px;text-align:center;background:#f8fcfe}
.fileinput{width:100%;margin-top:10px}
.progress{height:9px;background:#dfe9ef;border-radius:999px;overflow:hidden;margin-top:12px}
.bar{height:100%;width:0;background:linear-gradient(90deg,#60bde8,#3497d1);transition:width .12s}
.filelist{margin-top:14px;border-top:1px solid #e3ebef}
.file{display:grid;grid-template-columns:1fr auto;gap:10px;align-items:center;padding:12px 1px;border-bottom:1px solid #e8eef2}
.name{font-weight:650;overflow-wrap:anywhere}.size{font-size:12px;color:var(--muted);margin-top:3px}
.actions{display:flex;gap:6px}.actions button{padding:8px 9px;font-size:12px}
textarea{width:100%;min-height:120px;border:1px solid #b8cbd7;border-radius:12px;padding:12px;resize:vertical;font:inherit;color:var(--text)}
.remote{display:grid;grid-template-columns:repeat(3,58px);gap:7px;justify-content:center;margin:18px 0}
.remote button{height:52px}.remote .blank{visibility:hidden}
.abxy{display:grid;grid-template-columns:repeat(2,70px);gap:8px;justify-content:center;margin-top:15px}
.toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%) translateY(25px);background:#263640;color:#fff;padding:10px 15px;border-radius:999px;font-size:13px;opacity:0;pointer-events:none;transition:.2s;max-width:90%;text-align:center}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
footer{text-align:center;color:var(--muted);font-size:12px;padding:5px 15px 25px}
.lock{color:#a2631e}
@media(max-width:420px){h1{font-size:25px}.actions{flex-direction:column}.file{grid-template-columns:1fr auto}}
</style>
</head>
<body>
<header>
  <div class="wrap headrow">
    <div><h1>3DS Link</h1><div class="small">Pont local iPhone ↔ Nintendo 3DS</div></div>
    <div class="small">v0.4</div>
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
    <button class="tab" onclick="showTab('camera',this);loadCamera(true)">Camera</button>
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
    <h2>Camera Link</h2>
    <div class="muted">Prends plusieurs photos avec la camera exterieure de la 3DS. Chaque nouvelle photo arrive automatiquement sur cette page.</div>
    <button style="margin-top:12px;width:100%;font-size:17px;padding:14px" onclick="remoteCapture()">📷 Prendre une photo sur la 3DS</button>
    <div id="cameraState" class="muted" style="margin-top:9px">Tu peux aussi appuyer sur Y puis A directement sur la console.</div>
    <div id="cameraPreview" style="margin-top:14px"></div>
    <div style="display:flex;justify-content:space-between;align-items:center;margin-top:14px"><strong>Pellicule</strong><button class="secondary" onclick="loadCamera(true)">Actualiser</button></div>
    <div id="cameraRoll" class="filelist"></div>
  </section>

  <section id="info" class="panel">
    <h2>À propos</h2>
    <p class="muted">3DS Link fonctionne uniquement sur ton réseau local. Aucun serveur Internet n’est nécessaire pour le transfert.</p>
    <p class="muted">La v0.4 ajoute Camera Link : capture multiple sur la 3DS et transfert automatique vers cette page.</p>
  </section>

  <footer>3DS Link v0.4 • réseau local • garde l’application ouverte sur la 3DS</footer>
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

async function fetchCameraBlob(name){
  const r=await fetch('/camera/file?name='+encodeURIComponent(name),{headers:headers()});
  if(!r.ok) throw new Error('Photo indisponible');
  return await r.blob();
}

async function showCameraPhoto(name){
  if(!name || name===latestCameraName) return;
  try{
    const blob=await fetchCameraBlob(name);
    if(latestCameraUrl) URL.revokeObjectURL(latestCameraUrl);
    latestCameraUrl=URL.createObjectURL(blob);
    latestCameraName=name;
    $('cameraPreview').innerHTML=`<div style="background:#111;border-radius:14px;padding:8px"><img src="${latestCameraUrl}" style="display:block;width:100%;border-radius:9px" alt="Derniere photo"></div><div style="display:flex;gap:8px;margin-top:8px"><button class="secondary" style="flex:1" onclick='saveCameraPhoto(${JSON.stringify(name)})'>Télécharger</button><button class="danger" onclick='deleteCameraPhoto(${JSON.stringify(name)})'>Supprimer</button></div>`;
    $('cameraState').textContent='Nouvelle photo reçue automatiquement : '+name;
  }catch(e){ $('cameraState').textContent=e.message; }
}

function renderCamera(data, forcePreview=false){
  const roll=$('cameraRoll');
  if(!data.photos.length){
    roll.innerHTML='<div class="muted" style="padding:12px 0">Aucune photo dans cette session.</div>';
    return;
  }
  roll.innerHTML=data.photos.map(p=>`<div class="file"><div><div class="name">${esc(p.name)}</div><div class="size">${formatSize(p.size)}</div></div><div class="actions"><button class="secondary" onclick='showCameraPhoto(${JSON.stringify(p.name)})'>Voir</button><button class="secondary" onclick='saveCameraPhoto(${JSON.stringify(p.name)})'>Télécharger</button></div></div>`).join('');
  const newest=data.photos[0].name;
  if(forcePreview || newest!==latestCameraName) showCameraPhoto(newest);
}

async function loadCamera(force=false){
  if(!pin) return;
  try{
    const r=await fetch('/api/camera',{headers:headers()});
    if(!r.ok) return;
    renderCamera(await r.json(),force);
  }catch(e){}
}

async function remoteCapture(){
  if(!pin){toast('Entre le PIN');return}
  const before=latestCameraName;
  $('cameraState').textContent='Capture demandée à la 3DS…';
  const r=await fetch('/api/camera/capture',{method:'POST',headers:headers()});
  if(!r.ok){toast('Capture refusée');return}
  for(let i=0;i<14;i++){
    await new Promise(resolve=>setTimeout(resolve,650));
    try{
      const s=await fetch('/api/camera',{headers:headers()});
      if(!s.ok) continue;
      const data=await s.json();
      if(data.photos.length && data.photos[0].name!==before){
        renderCamera(data,true); toast('Photo reçue sur l’iPhone'); return;
      }
    }catch(e){}
  }
  $('cameraState').textContent='La capture prend plus de temps que prévu. Appuie sur Actualiser.';
}

async function saveCameraPhoto(name){
  try{
    const blob=await fetchCameraBlob(name);
    const a=document.createElement('a');
    a.href=URL.createObjectURL(blob); a.download=name; document.body.appendChild(a); a.click(); a.remove();
    setTimeout(()=>URL.revokeObjectURL(a.href),1500);
  }catch(e){toast(e.message)}
}

async function deleteCameraPhoto(name){
  if(!confirm('Supprimer cette photo de la 3DS ?')) return;
  const r=await fetch('/api/camera/delete?name='+encodeURIComponent(name),{method:'POST',headers:headers()});
  if(r.ok){ if(name===latestCameraName){latestCameraName='';$('cameraPreview').innerHTML=''} loadCamera(true); toast('Photo supprimée'); }
}

setInterval(()=>{
  if($('camera').classList.contains('active')) loadCamera(false);
},2200);

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

    if (!authorized(headers)) {
        sendSimple(clientSocket, 401, "Unauthorized", "{\"error\":\"pin\"}", "application/json");
        lastAction = "Tentative refusee : PIN incorrect";
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

    if (route == "/api/camera" && method == "GET") {
        sendSimple(clientSocket, 200, "OK", cameraJson(), "application/json; charset=utf-8");
        return;
    }

    if (route == "/api/camera/capture" && method == "POST") {
        cameraCaptureRequested = true;
        cameraMode = true;
        cameraStatus = "Capture demandee depuis l'iPhone";
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
            if (lastPhotoName == name) lastPhotoName.clear();
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

void pollServer() {
    if (!serverReady || serverSocket < 0) return;

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

    const int flags = fcntl(clientSocket, F_GETFL, 0);
    fcntl(clientSocket, F_SETFL, flags & ~O_NONBLOCK);

    handleClient(client);
    closeClient();
}

void drawHomeTopScreen() {
    C2D_TargetClear(topTarget, COLOR_BG_TOP);
    C2D_SceneBegin(topTarget);

    C2D_DrawRectSolid(0, 0, 0.1f, 400, 43, COLOR_BLUE);
    C2D_DrawRectSolid(0, 0, 0.2f, 400, 3, COLOR_BLUE_LIGHT);
    C2D_DrawRectSolid(0, 42, 0.2f, 400, 1, COLOR_BLUE_DARK);

    drawText("3DS Link", 16, 8, 0.72f, COLOR_WHITE);
    drawText("v0.4", 348, 11, 0.40f, COLOR_WHITE);

    // QR code : grande zone blanche avec quiet-zone standard.
    drawRoundedRect(12, 54, 164, 174, 14, COLOR_SHADOW, 0.15f);
    drawRoundedRect(9, 51, 164, 174, 14, COLOR_WHITE, 0.2f);
    drawCenteredText("Scanner avec l'iPhone", 91, 60, 0.37f, COLOR_TEXT);

    if (serverReady && qrReady) {
        drawConnectionQr(91, 151, 148.0f);
    } else {
        drawCenteredText("QR indisponible", 91, 142, 0.42f, COLOR_RED);
    }

    // Etat du serveur.
    drawRoundedRect(190, 54, 198, 74, 14, COLOR_SHADOW, 0.15f);
    drawRoundedRect(187, 51, 198, 74, 14, COLOR_WHITE, 0.2f);

    C2D_DrawCircleSolid(
        210,
        77,
        0.5f,
        8,
        serverReady ? COLOR_GREEN : COLOR_RED
    );

    drawText(
        serverReady ? "Serveur actif" : "Serveur indisponible",
        228,
        65,
        0.47f,
        COLOR_TEXT
    );
    drawText(statusMessage.substr(0, 25), 202, 96, 0.32f, COLOR_MUTED);

    // URL et code PIN.
    drawRoundedRect(190, 142, 198, 86, 14, COLOR_SHADOW, 0.15f);
    drawRoundedRect(187, 139, 198, 86, 14, COLOR_WHITE, 0.2f);
    drawText("Adresse locale", 201, 151, 0.38f, COLOR_MUTED);

    if (serverReady) {
        drawText("http://" + localIp, 201, 174, 0.36f, COLOR_BLUE_DARK);
        drawText(":" + std::to_string(PORT), 201, 193, 0.36f, COLOR_BLUE_DARK);
        drawText("PIN", 292, 193, 0.34f, COLOR_MUTED);
        drawText(std::to_string(pinCode), 320, 188, 0.53f, COLOR_ORANGE);
    } else {
        drawText("A : reessayer", 201, 181, 0.39f, COLOR_BLUE_DARK);
    }
}

void drawHomeBottomScreen() {
    C2D_TargetClear(bottomTarget, COLOR_BG_BOTTOM);
    C2D_SceneBegin(bottomTarget);

    C2D_DrawRectSolid(0, 0, 0.1f, 320, 34, COLOR_BLUE);
    drawText("Activite iPhone", 13, 7, 0.56f, COLOR_WHITE);

    drawRoundedRect(10, 46, 300, 64, 12, COLOR_WHITE, 0.2f);
    C2D_DrawRectSolid(10, 109, 0.25f, 300, 1, COLOR_LINE);

    drawText(
        clientSeen ? "iPhone detecte" : "En attente de l'iPhone",
        24,
        57,
        0.51f,
        clientSeen ? COLOR_GREEN : COLOR_TEXT
    );
    drawText(lastAction.substr(0, 43), 24, 84, 0.34f, COLOR_MUTED);

    drawRoundedRect(10, 122, 300, 64, 12, COLOR_WHITE, 0.2f);
    drawText("Clavier distant", 24, 132, 0.45f, COLOR_TEXT);

    if (lastText.empty()) {
        drawText("Aucun texte recu", 24, 158, 0.34f, COLOR_MUTED);
    } else {
        drawText(lastText.substr(0, 45), 24, 157, 0.34f, COLOR_BLUE_DARK);
    }

    drawText("Remote : " + lastRemote, 13, 204, 0.34f, COLOR_MUTED);
    drawText("X Nouveau PIN", 107, 204, 0.34f, COLOR_ORANGE);
    drawText("START Quitter", 220, 204, 0.34f, COLOR_MUTED);
}

void drawCameraTopScreen() {
    C2D_TargetClear(topTarget, C2D_Color32(20, 23, 26, 255));
    C2D_SceneBegin(topTarget);

    C2D_DrawRectSolid(0, 0, 0.1f, 400, 31, C2D_Color32(0, 0, 0, 180));
    drawText("Camera Link", 13, 5, 0.56f, COLOR_WHITE);
    drawText("v0.4", 350, 7, 0.34f, C2D_Color32(210, 220, 226, 255));

    // Zone viseur. La capture reelle utilise la camera exterieure ;
    // l'apercu video continu sera la prochaine couche d'optimisation.
    C2D_DrawRectSolid(18, 44, 364, 154, C2D_Color32(39, 45, 49, 255));
    C2D_DrawRectSolid(20, 46, 360, 150, C2D_Color32(25, 30, 33, 255));
    C2D_DrawLine(200, 91, COLOR_MUTED, 200, 149, COLOR_MUTED, 1.0f, 0.4f);
    C2D_DrawLine(171, 120, COLOR_MUTED, 229, 120, COLOR_MUTED, 1.0f, 0.4f);
    C2D_DrawCircleSolid(200, 120, 0.45f, 5, COLOR_BLUE);

    drawCenteredText("Camera exterieure", 200, 62, 0.40f, C2D_Color32(205, 215, 220, 255));
    drawCenteredText(cameraStatus.substr(0, 42), 200, 207, 0.39f,
        cameraStatus.find("Echec") != std::string::npos ? COLOR_RED : COLOR_WHITE);
}

void drawCameraBottomScreen() {
    C2D_TargetClear(bottomTarget, C2D_Color32(236, 241, 244, 255));
    C2D_SceneBegin(bottomTarget);

    C2D_DrawRectSolid(0, 0, 0.1f, 320, 37, COLOR_BLUE);
    drawText("Appareil photo", 13, 8, 0.56f, COLOR_WHITE);

    drawRoundedRect(11, 50, 298, 55, 12, COLOR_WHITE, 0.2f);
    drawText("Photos de la session", 24, 61, 0.43f, COLOR_TEXT);
    drawText(std::to_string(cameraPhotoCount), 262, 57, 0.66f, COLOR_BLUE_DARK);
    if (!lastPhotoName.empty()) drawText(lastPhotoName.substr(0, 28), 24, 84, 0.31f, COLOR_MUTED);
    else drawText("Aucune photo pour le moment", 24, 84, 0.31f, COLOR_MUTED);

    // Gros declencheur tactile, dans l'esprit de l'appareil photo 3DS.
    C2D_DrawCircleSolid(160, 164, 0.3f, 38, C2D_Color32(190, 202, 209, 255));
    C2D_DrawCircleSolid(160, 164, 0.4f, 32, COLOR_WHITE);
    C2D_DrawCircleSolid(160, 164, 0.5f, 25, COLOR_BLUE);
    C2D_DrawCircleSolid(160, 164, 0.6f, 18, C2D_Color32(239, 248, 253, 255));

    drawText("B Retour", 17, 213, 0.35f, COLOR_MUTED);
    drawCenteredText("A  Prendre", 160, 213, 0.35f, COLOR_TEXT);
    drawText("Y Accueil", 244, 213, 0.35f, COLOR_MUTED);
}

void drawTopScreen() {
    if (cameraMode) drawCameraTopScreen();
    else drawHomeTopScreen();
}

void drawBottomScreen() {
    if (cameraMode) drawCameraBottomScreen();
    else drawHomeBottomScreen();
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

    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();

        if (down & KEY_START) break;

        if (cameraMode) {
            if (down & KEY_B || down & KEY_Y) {
                cameraMode = false;
                cameraStatus = "Pret a prendre une photo";
            } else if ((down & KEY_A) || ((down & KEY_TOUCH) && cameraTouchPressed())) {
                cameraCaptureRequested = true;
            }
        } else {
            if (down & KEY_A) startServer();
            if (down & KEY_Y) {
                cameraMode = true;
                cameraStatus = "Pret - A ou bouton tactile";
            }
            if (down & KEY_X) {
                generatePin();
                lastAction = "Nouveau code PIN genere";
            }
        }

        pollServer();

        if (cameraCaptureRequested) {
            cameraCaptureRequested = false;
            captureCameraPhoto();
        }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        drawTopScreen();
        drawBottomScreen();
        C3D_FrameEnd(0);

        C2D_TextBufClear(textBuffer);
    }

    stopServer();
    shutdownGraphics();
    return 0;
}
