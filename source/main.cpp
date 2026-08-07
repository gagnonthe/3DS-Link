#include <3ds.h>
#include <citro2d.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

namespace {

constexpr int PORT = 8080;

constexpr u32 COLOR_BG_TOP      = C2D_Color32(234, 241, 246, 255);
constexpr u32 COLOR_BG_BOTTOM   = C2D_Color32(241, 245, 248, 255);
constexpr u32 COLOR_BLUE        = C2D_Color32(69, 158, 214, 255);
constexpr u32 COLOR_BLUE_DARK   = C2D_Color32(30, 111, 171, 255);
constexpr u32 COLOR_BLUE_LIGHT  = C2D_Color32(128, 202, 238, 255);
constexpr u32 COLOR_TEXT        = C2D_Color32(45, 58, 68, 255);
constexpr u32 COLOR_MUTED       = C2D_Color32(107, 120, 130, 255);
constexpr u32 COLOR_WHITE       = C2D_Color32(255, 255, 255, 255);
constexpr u32 COLOR_GREEN       = C2D_Color32(62, 176, 105, 255);
constexpr u32 COLOR_RED         = C2D_Color32(202, 73, 79, 255);
constexpr u32 COLOR_SHADOW      = C2D_Color32(65, 80, 92, 45);

u32* socBuffer = nullptr;
int serverSocket = -1;
int clientSocket = -1;
bool socStarted = false;
bool serverReady = false;
bool clientSeen = false;
unsigned int requestCount = 0;

std::string localIp = "0.0.0.0";
std::string lastClient = "Aucun iPhone connecte";
std::string statusMessage = "Initialisation du reseau...";

C3D_RenderTarget* topTarget = nullptr;
C3D_RenderTarget* bottomTarget = nullptr;
C2D_TextBuf textBuffer = nullptr;

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

std::string makeWebPage() {
    return R"HTML(<!doctype html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>3DS Link</title>
<style>
:root{color-scheme:light;font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display",Arial,sans-serif}
*{box-sizing:border-box}
body{margin:0;background:#eef4f8;color:#26343e;min-height:100vh}
header{background:linear-gradient(180deg,#7fc9ed,#459ed6);color:#fff;padding:24px 20px 20px;border-bottom:1px solid #236da5}
.wrap{max-width:720px;margin:auto}
h1{margin:0;font-size:30px;letter-spacing:-.5px}
.sub{opacity:.9;margin-top:4px}
.status{margin:18px 14px 0;background:#fff;border:1px solid #c5d3dc;border-radius:16px;padding:16px;box-shadow:0 4px 14px #4f6d8020}
.dot{display:inline-block;width:11px;height:11px;border-radius:50%;background:#3eb069;margin-right:8px;box-shadow:0 0 0 4px #3eb06922}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;padding:14px}
.card{background:#fff;border:1px solid #c5d3dc;border-radius:16px;padding:18px;min-height:120px;box-shadow:0 4px 14px #4f6d8017}
.icon{font-size:30px}.title{font-size:18px;font-weight:700;margin-top:8px}.soon{font-size:13px;color:#71838f;margin-top:4px}
footer{text-align:center;color:#71838f;padding:14px 18px 30px;font-size:13px}
.badge{display:inline-block;background:#e6f5fd;color:#236da5;border:1px solid #b8def2;border-radius:999px;padding:5px 10px;font-weight:600}
</style>
</head>
<body>
<header><div class="wrap"><h1>3DS Link</h1><div class="sub">Votre iPhone est relié à votre Nintendo 3DS.</div></div></header>
<div class="wrap">
<div class="status"><span class="dot"></span><strong>3DS connectée</strong><br><span class="soon">Connexion locale active</span></div>
<div class="grid">
<div class="card"><div class="icon">📁</div><div class="title">Fichiers</div><div class="soon">Transfert iPhone ↔ 3DS — bientôt</div></div>
<div class="card"><div class="icon">🖼️</div><div class="title">Photos</div><div class="soon">Envoi direct vers la galerie — bientôt</div></div>
<div class="card"><div class="icon">⌨️</div><div class="title">Clavier</div><div class="soon">Texte iPhone → 3DS — bientôt</div></div>
<div class="card"><div class="icon">🎮</div><div class="title">Remote</div><div class="soon">Télécommande 3DS — bientôt</div></div>
</div>
<footer><span class="badge">3DS Link v0.1</span><p>Garde cette page ouverte pendant que 3DS Link fonctionne.</p></footer>
</div>
</body>
</html>)HTML";
}

void closeClient() {
    if (clientSocket >= 0) {
        close(clientSocket);
        clientSocket = -1;
    }
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

bool startServer() {
    stopServer();

    clientSeen = false;
    requestCount = 0;
    lastClient = "Aucun iPhone connecte";
    statusMessage = "Initialisation du reseau...";

    socBuffer = static_cast<u32*>(memalign(SOC_ALIGN, SOC_BUFFERSIZE));
    if (!socBuffer) {
        statusMessage = "Erreur memoire reseau";
        return false;
    }

    const Result socResult = socInit(socBuffer, SOC_BUFFERSIZE);
    if (R_FAILED(socResult)) {
        char buffer[64];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "socInit: 0x%08lX",
            static_cast<unsigned long>(socResult)
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
        return false;
    }

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

    char request[1025]{};
    recv(clientSocket, request, 1024, 0);

    clientSeen = true;
    ++requestCount;
    lastClient = inet_ntoa(client.sin_addr);
    statusMessage = "iPhone connecte !";

    const std::string page = makeWebPage();

    char header[256];
    const int headerLength = std::snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        static_cast<unsigned int>(page.size())
    );

    if (headerLength > 0) {
        sendAll(clientSocket, header, static_cast<size_t>(headerLength));
        sendAll(clientSocket, page.c_str(), page.size());
    }

    closeClient();
}

void drawTopScreen() {
    C2D_TargetClear(topTarget, COLOR_BG_TOP);
    C2D_SceneBegin(topTarget);

    C2D_DrawRectSolid(0, 0, 0.1f, 400, 43, COLOR_BLUE);
    C2D_DrawRectSolid(0, 0, 0.2f, 400, 3, COLOR_BLUE_LIGHT);
    C2D_DrawRectSolid(0, 42, 0.2f, 400, 1, COLOR_BLUE_DARK);

    drawText("3DS Link", 16, 8, 0.72f, COLOR_WHITE);
    drawText("v0.1", 348, 11, 0.40f, COLOR_WHITE);

    drawRoundedRect(28, 63, 344, 104, 14, COLOR_SHADOW, 0.15f);
    drawRoundedRect(25, 60, 344, 104, 14, COLOR_WHITE, 0.2f);

    C2D_DrawCircleSolid(
        54,
        86,
        0.5f,
        9,
        serverReady ? COLOR_GREEN : COLOR_RED
    );

    drawText(
        serverReady ? "Serveur local actif" : "Serveur indisponible",
        74,
        74,
        0.57f,
        COLOR_TEXT
    );

    drawText(statusMessage, 42, 108, 0.43f, COLOR_MUTED);

    if (serverReady) {
        drawCenteredText(
            "Ouvre Safari sur ton iPhone",
            200,
            180,
            0.47f,
            COLOR_MUTED
        );

        drawRoundedRect(53, 202, 294, 28, 10, COLOR_BLUE, 0.3f);

        drawCenteredText(
            "http://" + localIp + ":" + std::to_string(PORT),
            200,
            207,
            0.46f,
            COLOR_WHITE
        );
    } else {
        drawCenteredText(
            "A : reessayer la connexion",
            200,
            197,
            0.48f,
            COLOR_BLUE_DARK
        );
    }
}

void drawBottomScreen() {
    C2D_TargetClear(bottomTarget, COLOR_BG_BOTTOM);
    C2D_SceneBegin(bottomTarget);

    C2D_DrawRectSolid(0, 0, 0.1f, 320, 34, COLOR_BLUE);
    drawText("Connexion", 13, 7, 0.58f, COLOR_WHITE);

    drawRoundedRect(13, 49, 294, 75, 12, COLOR_SHADOW, 0.15f);
    drawRoundedRect(10, 46, 300, 75, 12, COLOR_WHITE, 0.2f);

    drawText(
        clientSeen ? "iPhone detecte" : "En attente de l'iPhone",
        25,
        58,
        0.55f,
        clientSeen ? COLOR_GREEN : COLOR_TEXT
    );

    drawText(
        clientSeen
            ? ("Adresse : " + lastClient)
            : "Ouvre l'adresse affichee en haut.",
        25,
        87,
        0.39f,
        COLOR_MUTED
    );

    drawRoundedRect(10, 137, 145, 55, 11, COLOR_WHITE, 0.2f);
    drawText("A", 24, 147, 0.61f, COLOR_BLUE_DARK);
    drawText("Relancer", 55, 148, 0.49f, COLOR_TEXT);
    drawText("le serveur", 55, 170, 0.34f, COLOR_MUTED);

    drawRoundedRect(165, 137, 145, 55, 11, COLOR_WHITE, 0.2f);
    drawText("START", 177, 148, 0.42f, COLOR_BLUE_DARK);
    drawText("Quitter", 237, 148, 0.49f, COLOR_TEXT);
    drawText("3DS Link", 237, 170, 0.34f, COLOR_MUTED);

    drawText(
        "Requetes Safari : " + std::to_string(requestCount),
        13,
        211,
        0.35f,
        COLOR_MUTED
    );
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
        if (down & KEY_A) startServer();

        pollServer();

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
