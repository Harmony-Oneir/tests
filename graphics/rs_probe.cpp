/*
 * Copyright (c) 2026 Harmony-Oneir
 * SPDX-License-Identifier: Apache-2.0
 *
 * rs_probe — проверка Render Service (RS) клиентом.
 *
 * Строит дерево узлов: окно (RSSurfaceNode, прикреплено к экрану) -> RSCanvasNode (содержимое),
 * рисует разными способами и проверяет результат снимком экрана (TakeSurfaceCapture):
 * так видно, что RS действительно отрисовал кадр, а не только принял команды.
 *
 *   rs_probe                — все тесты
 *   rs_probe <тест> ...     — выбранные: screen colors windows shapes text cpubuffer vsync perf stress
 *   rs_probe -h             — список
 *
 * Опции:  --hold <с>  сколько держать каждую картинку на экране (по умолчанию 1)
 *         --grid N    stress: сетка N x N узлов (по умолчанию 20)
 *         --attach-to-display  прикреплять окно через AttachToDisplay вместо RSDisplayNode
 *                     (на стенде без службы дисплеев RS при этом рисует, но не выводит на экран)
 *         --no-capture  не делать снимков (только вывод на экран)
 */

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "event_handler.h"
#include "pixel_map.h"
#include "pipeline/rs_recording_canvas.h"
#include "surface_utils.h"
#include "draw/brush.h"
#include "draw/color.h"
#include "effect/shader_effect.h"
#include "text/font.h"
#include "text/text_blob.h"
#include "transaction/rs_interfaces.h"
#include "transaction/rs_render_interface.h"
#include "transaction/rs_transaction.h"
#include "ui/rs_canvas_node.h"
#include "ui/rs_display_node.h"
#include "ui/rs_surface_node.h"
#include "ui/rs_ui_context.h"
#include "ui/rs_ui_director.h"

using namespace OHOS;
using namespace OHOS::Rosen;
using Clock = std::chrono::steady_clock;

namespace {
// ----------------------------------------------------------------------------- общий контекст
struct Ctx {
    ScreenId screen = INVALID_SCREEN_ID;
    int width = 0;
    int height = 0;
    uint32_t refresh = 60;  // частота активного режима экрана, Гц
    int holdSec = 1;
    bool capture = true;
    int grid = 20;          // stress: сетка grid x grid узлов (--grid N)
    // Окно крепится к экрану через клиентский RSDisplayNode: только при нём RS выводит кадры на дисплей
    // (проверено: 21 коммит композитора против 0 при AttachToDisplay). --attach-to-display — для сравнения.
    bool useDisplayNode = true;
    std::shared_ptr<RSDisplayNode> display;
    std::shared_ptr<RSSurfaceNode> window;
    // Снимки в этой версии RS делаются через RSRenderInterface, который выдаёт контекст UI
    // (RSUIDirector -> RSUIContext -> GetRSRenderInterface), как в демо render_service_client.
    std::shared_ptr<RSUIDirector> director;
};
Ctx g;

struct Result {
    std::string name;
    bool ok;
    std::string note;
};
std::vector<Result> g_results;

void Report(const std::string &name, bool ok, const std::string &note = "")
{
    g_results.push_back({name, ok, note});
    printf("  [%s] %s%s%s\n", ok ? " OK " : "FAIL", name.c_str(), note.empty() ? "" : " — ", note.c_str());
    fflush(stdout);
}

double MsSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void Flush()
{
    RSTransaction::FlushImplicitTransaction();
    // Команды узлов, привязанных к контексту UI, отправляет его директор.
    if (g.director) {
        g.director->SendMessages();
    }
}

void Hold()
{
    sleep(static_cast<unsigned>(g.holdSec));
}

uint32_t Argb(uint8_t a, uint8_t r, uint8_t gg, uint8_t b)
{
    return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(gg) << 8) | b;
}

// ----------------------------------------------------------------------------- снимок экрана
struct Rgba {
    int r, g, b, a;
};

class CaptureCb : public SurfaceCaptureCallback {
public:
    void OnSurfaceCapture(std::shared_ptr<Media::PixelMap> pixelmap) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        pm_ = pixelmap;
        done_ = true;
        cv_.notify_all();
    }
    // HDR-вариант снимка (есть в этой версии RS): берём обычную часть.
    void OnSurfaceCaptureHDR(std::shared_ptr<Media::PixelMap> pixelmap, std::shared_ptr<Media::PixelMap>) override
    {
        OnSurfaceCapture(pixelmap);
    }
    std::shared_ptr<Media::PixelMap> Wait(int ms)
    {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return done_; });
        return pm_;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool done_ = false;
    std::shared_ptr<Media::PixelMap> pm_;
};

std::string g_captureError; // причина последней неудачи снимка — для сообщений

std::shared_ptr<Media::PixelMap> Capture(double *latencyMs = nullptr)
{
    g_captureError.clear();
    if (!g.capture) {
        return nullptr;
    }
    usleep(150 * 1000); // дать RS отрисовать последний кадр
    auto cb = std::make_shared<CaptureCb>();
    auto t0 = Clock::now();
    // В этой версии RS снимки в RSInterfaces — заглушки (return false); реализация перенесена
    // в RSRenderInterface (rs_render_interface.cpp), его выдаёт контекст UI. Снимаем весь экран.
    auto ctx = g.director ? g.director->GetRSUIContext() : nullptr;
    auto ri = ctx ? ctx->GetRSRenderInterface() : nullptr;
    if (!ri) {
        g_captureError = "нет RSRenderInterface (контекст UI не создан)";
        return nullptr;
    }
    // Снимок окна, а не узла экрана: клиентский RSDisplayNode в этой версии RS — логический узел
    // без размеров (RS создаёт настоящий узел экрана сам), и снимок по нему получается 0x0.
    // Окно занимает весь экран с (0,0) и содержит всё тестовое дерево.
    if (!ri->TakeSurfaceCapture(g.window, cb)) {
        g_captureError = "TakeSurfaceCapture вернул false (запрос отклонён)";
        return nullptr;
    }
    auto pm = cb->Wait(3000);
    if (latencyMs) {
        *latencyMs = MsSince(t0);
    }
    if (!pm) {
        g_captureError = "запрос принят, но снимок пустой или нет ответа за 3 с (см. hilog: Capture)";
    }
    return pm;
}

bool PixelAt(const std::shared_ptr<Media::PixelMap> &pm, int x, int y, Rgba &out)
{
    if (!pm || x < 0 || y < 0) {
        return false;
    }
    // снимок может быть меньше экрана — масштабируем координаты
    int px = x * pm->GetWidth() / std::max(1, g.width);
    int py = y * pm->GetHeight() / std::max(1, g.height);
    if (px >= pm->GetWidth() || py >= pm->GetHeight()) {
        return false;
    }
    const uint8_t *p = pm->GetPixels() + py * pm->GetRowBytes() + px * 4;
    if (pm->GetPixelFormat() == Media::PixelFormat::BGRA_8888) {
        out = {p[2], p[1], p[0], p[3]};
    } else {
        out = {p[0], p[1], p[2], p[3]};
    }
    return true;
}

bool Near(const Rgba &c, int r, int gg, int b, int tol = 24)
{
    return std::abs(c.r - r) <= tol && std::abs(c.g - gg) <= tol && std::abs(c.b - b) <= tol;
}

std::string Str(const Rgba &c)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "(%d,%d,%d)", c.r, c.g, c.b);
    return buf;
}

// проверка точки снимка; без снимков — только сообщение
void CheckPixel(const std::string &name, const std::shared_ptr<Media::PixelMap> &pm, int x, int y,
                int r, int gg, int b)
{
    if (!g.capture) {
        Report(name, true, "без снимка — проверьте глазами");
        return;
    }
    Rgba c {};
    if (!pm) {
        Report(name, false, "снимок не получен: " + g_captureError);
        return;
    }
    if (!PixelAt(pm, x, y, c)) {
        Report(name, false, "точка (" + std::to_string(x) + "," + std::to_string(y) + ") вне снимка " +
               std::to_string(pm->GetWidth()) + "x" + std::to_string(pm->GetHeight()));
        return;
    }
    char want[48];
    snprintf(want, sizeof(want), "(%d,%d,%d)", r, gg, b);
    Report(name, Near(c, r, gg, b), "в (" + std::to_string(x) + "," + std::to_string(y) + ") " + Str(c) +
           ", ожидалось " + want);
}

std::shared_ptr<RSCanvasNode> NewCanvas(float x, float y, float w, float h, uint32_t argb)
{
    auto node = RSCanvasNode::Create();
    node->SetBounds(x, y, w, h);
    node->SetFrame(x, y, w, h);
    node->SetBackgroundColor(argb);
    return node;
}

// Клиентские узлы RS не принадлежат родителю: окно хранит детей по идентификатору, а деструктор
// узла отправляет в RS команду удаления. Узлы, созданные в цикле, нужно держать живыми, пока они
// на экране, — иначе они исчезают в той же транзакции, в которой были добавлены.
std::vector<std::shared_ptr<RSNode>> g_keepAlive;

void ClearWindow()
{
    g.window->ClearChildren();
    g_keepAlive.clear();
    g.window->SetBackgroundColor(Argb(255, 0, 0, 0));
    Flush();
}

// ----------------------------------------------------------------------------- тесты
void TestScreen()
{
    auto &rs = RSInterfaces::GetInstance();
    auto mode = rs.GetScreenActiveMode(g.screen);
    auto modes = rs.GetScreenSupportedModes(g.screen);
    printf("  экран %llu: %dx%d @ %u Гц, режимов %zu, яркость %d\n",
           static_cast<unsigned long long>(g.screen), mode.GetScreenWidth(), mode.GetScreenHeight(),
           mode.GetScreenRefreshRate(), modes.size(), rs.GetScreenBacklight(g.screen));
    for (auto &m : modes) {
        printf("    режим %d: %dx%d @ %u Гц\n", m.GetScreenModeId(), m.GetScreenWidth(), m.GetScreenHeight(),
               m.GetScreenRefreshRate());
    }
    Report("экран: активный режим", mode.GetScreenWidth() > 0 && mode.GetScreenHeight() > 0,
           std::to_string(mode.GetScreenWidth()) + "x" + std::to_string(mode.GetScreenHeight()));
    Report("экран: список режимов", !modes.empty());
    rs.SetScreenPowerStatus(g.screen, ScreenPowerStatus::POWER_STATUS_ON);
    Report("экран: питание ON", true);
}

void TestColors()
{
    struct C { const char *name; int r, g, b; } colors[] = {
        {"красный", 255, 0, 0}, {"зелёный", 0, 255, 0}, {"синий", 0, 0, 255},
        {"белый", 255, 255, 255}, {"серый", 128, 128, 128}, {"чёрный", 0, 0, 0},
    };
    for (auto &c : colors) {
        g.window->SetBackgroundColor(Argb(255, c.r, c.g, c.b));
        Flush();
        Hold();
        CheckPixel(std::string("заливка ") + c.name, Capture(), g.width / 2, g.height / 2, c.r, c.g, c.b);
    }
    ClearWindow();
}

void TestWindows()
{
    // три «окна» разных размеров, перекрываются; порядок по Z, прозрачность, скругления
    auto a = NewCanvas(100, 100, 800, 500, Argb(255, 220, 40, 40));
    auto b = NewCanvas(500, 300, 800, 500, Argb(255, 40, 180, 40));
    auto c = NewCanvas(900, 500, 700, 400, Argb(255, 40, 80, 220));
    b->SetPositionZ(2);
    c->SetPositionZ(1);
    c->SetCornerRadius(60);
    g.window->AddChild(a, -1);
    g.window->AddChild(b, -1);
    g.window->AddChild(c, -1);
    Flush();
    Hold();
    auto pm = Capture();
    CheckPixel("окно A (только A)", pm, 200, 200, 220, 40, 40);
    CheckPixel("A и B перекрываются, B выше по Z", pm, 700, 400, 40, 180, 40);
    CheckPixel("B и C перекрываются, B выше по Z", pm, 1100, 650, 40, 180, 40);
    CheckPixel("окно C (только C)", pm, 1450, 820, 40, 80, 220);
    CheckPixel("скруглённый угол C — фон", pm, 905, 893, 0, 0, 0);

    // размеры: окно меняет размер
    a->SetBounds(100, 100, 300, 200);
    a->SetFrame(100, 100, 300, 200);
    Flush();
    Hold();
    pm = Capture();
    CheckPixel("после уменьшения A: внутри", pm, 200, 200, 220, 40, 40);
    CheckPixel("после уменьшения A: снаружи — фон", pm, 450, 450, 0, 0, 0);

    // прозрачность 50 % над чёрным фоном
    a->SetAlpha(0.5f);
    Flush();
    Hold();
    CheckPixel("A с прозрачностью 0.5", Capture(), 200, 200, 110, 20, 20);
    ClearWindow();
}

void TestShapes()
{
    auto node = NewCanvas(0, 0, static_cast<float>(g.width), static_cast<float>(g.height), Argb(255, 0, 0, 0));
    auto canvas = node->BeginRecording(g.width, g.height);
    Drawing::Brush brush;
    brush.SetColor(Drawing::Color::ColorQuadSetARGB(255, 255, 200, 0));
    canvas->AttachBrush(brush);
    canvas->DrawRect(Drawing::Rect(100, 100, 500, 400));
    canvas->DetachBrush();

    brush.SetColor(Drawing::Color::ColorQuadSetARGB(255, 0, 200, 255));
    canvas->AttachBrush(brush);
    canvas->DrawCircle(Drawing::Point(900, 300), 180);
    canvas->DetachBrush();

    std::vector<Drawing::ColorQuad> colors = {Drawing::Color::ColorQuadSetARGB(255, 255, 0, 0),
                                              Drawing::Color::ColorQuadSetARGB(255, 0, 0, 255)};
    std::vector<Drawing::scalar> pos = {0.0f, 1.0f};
    Drawing::Brush grad;
    grad.SetShaderEffect(Drawing::ShaderEffect::CreateLinearGradient(
        Drawing::Point(100, 600), Drawing::Point(1800, 600), colors, pos, Drawing::TileMode::CLAMP));
    canvas->AttachBrush(grad);
    canvas->DrawRect(Drawing::Rect(100, 550, 1800, 950));
    canvas->DetachBrush();
    node->FinishRecording();

    g.window->AddChild(node, -1);
    Flush();
    Hold();
    auto pm = Capture();
    CheckPixel("прямоугольник", pm, 300, 250, 255, 200, 0);
    CheckPixel("круг: центр", pm, 900, 300, 0, 200, 255);
    CheckPixel("круг: снаружи — фон", pm, 1120, 300, 0, 0, 0);
    CheckPixel("градиент: левый край", pm, 110, 750, 255, 0, 0);
    CheckPixel("градиент: правый край", pm, 1790, 750, 0, 0, 255);
    ClearWindow();
}

void TestText()
{
    auto node = NewCanvas(0, 0, static_cast<float>(g.width), static_cast<float>(g.height), Argb(255, 0, 0, 0));
    auto canvas = node->BeginRecording(g.width, g.height);
    Drawing::Brush brush;
    brush.SetColor(Drawing::Color::ColorQuadSetARGB(255, 255, 255, 255));
    canvas->AttachBrush(brush);
    int sizes[] = {24, 48, 96, 160};
    int y = 150;
    for (int s : sizes) {
        Drawing::Font font;
        font.SetSize(static_cast<float>(s));
        auto blob = Drawing::TextBlob::MakeFromString("Oneir Harmony 0123", font, Drawing::TextEncoding::UTF8);
        if (blob) {
            canvas->DrawTextBlob(blob.get(), 100, static_cast<float>(y));
        }
        y += s + 40;
    }
    canvas->DetachBrush();
    node->FinishRecording();
    g.window->AddChild(node, -1);
    Flush();
    Hold();

    auto pm = Capture();
    if (!g.capture) {
        Report("текст", true, "без снимка — проверьте глазами");
    } else if (!pm) {
        Report("текст", false, "снимок не получен: " + g_captureError);
    } else {
        // в полосе самого крупного текста должны быть светлые пиксели
        int lit = 0;
        for (int x = 100; x < 1800; x += 4) {
            for (int yy = y - 160 - 40 - 140; yy < y - 40; yy += 4) {
                Rgba c {};
                if (PixelAt(pm, x, yy, c) && c.r > 128 && c.g > 128 && c.b > 128) {
                    lit++;
                }
            }
        }
        Report("текст: глифы на экране", lit > 50,
               "светлых точек " + std::to_string(lit) + (lit > 50 ? "" : " (нет шрифтов в /system/fonts?)"));
    }
    ClearWindow();
}

// Рисование на CPU прямо в буфер поверхности окна (так работает drawing_sample_rs):
// RS только компонует готовый буфер. Если этот тест не проходит, а canvas-тесты проходят,
// проблема в пути буферов (allocator -> RS -> композиция), а не в рендеринге.
void TestCpuBuffer()
{
    RSSurfaceNodeConfig cfg;
    cfg.SurfaceNodeName = "rs_probe_cpu";
    auto node = RSSurfaceNode::Create(cfg, RSSurfaceNodeType::SELF_DRAWING_NODE, false);
    if (!node) {
        Report("CPU-буфер: создание узла", false);
        return;
    }
    const int w = 600, h = 400, x = 200, y = 200;
    node->SetBounds(x, y, w, h);
    node->SetFrame(x, y, w, h);
    g.window->AddChild(node, -1);
    Flush();

    auto surface = node->GetSurface();
    if (!surface) {
        Report("CPU-буфер: поверхность", false);
        return;
    }
    sptr<SurfaceBuffer> buffer;
    int32_t releaseFence = -1;
    BufferRequestConfig req = {
        .width = w, .height = h, .strideAlignment = 0x8, .format = GRAPHIC_PIXEL_FMT_RGBA_8888,
        .usage = BUFFER_USAGE_CPU_READ | BUFFER_USAGE_CPU_WRITE | BUFFER_USAGE_MEM_DMA, .timeout = 1000,
    };
    if (surface->RequestBuffer(buffer, releaseFence, req) != GSERROR_OK || !buffer) {
        Report("CPU-буфер: RequestBuffer", false);
        return;
    }
    // залить пурпурным (RGBA)
    auto *base = static_cast<uint8_t *>(buffer->GetVirAddr());
    for (int row = 0; row < h; row++) {
        uint32_t *line = reinterpret_cast<uint32_t *>(base + row * buffer->GetStride());
        for (int col = 0; col < w; col++) {
            line[col] = 0xFFFF00FF; // A=FF, B=FF, G=00, R=FF в памяти RGBA little-endian
        }
    }
    BufferFlushConfig flush = {.damage = {.x = 0, .y = 0, .w = w, .h = h}, .timestamp = 0};
    bool flushed = surface->FlushBuffer(buffer, -1, flush) == GSERROR_OK;
    Report("CPU-буфер: FlushBuffer", flushed);
    Flush();
    Hold();
    CheckPixel("CPU-буфер на экране", Capture(), x + w / 2, y + h / 2, 255, 0, 255);
    ClearWindow();
}

// ----------------------------------------------------------------------------- VSync и производительность
std::atomic<int> g_vsyncCount {0};
std::vector<int64_t> g_vsyncTs;
std::mutex g_vsyncMu;
std::shared_ptr<VSyncReceiver> g_receiver;
VSyncReceiver::FrameCallback g_fcb;
std::atomic<bool> g_vsyncRunning {false};

void OnVsync(int64_t ts, void *)
{
    {
        std::lock_guard<std::mutex> lock(g_vsyncMu);
        g_vsyncTs.push_back(ts);
    }
    g_vsyncCount++;
    if (g_vsyncRunning) {
        g_receiver->RequestNextVSync(g_fcb);
    }
}

// Статистика интервалов между отметками времени VSync (нс): средний период, максимум,
// число пропусков (интервал больше полутора средних — значит, сигнал не пришёл вовремя).
struct IntervalStats {
    double avgMs = 0, maxMs = 0, maxDevMs = 0;
    int missed = 0;
};

IntervalStats Intervals(const std::vector<int64_t> &ts, double nominalMs)
{
    IntervalStats st;
    if (ts.size() < 3) {
        return st;
    }
    std::vector<double> d;
    for (size_t i = 1; i < ts.size(); i++) {
        d.push_back((ts[i] - ts[i - 1]) / 1e6);
    }
    for (double v : d) {
        st.avgMs += v;
        st.maxMs = std::max(st.maxMs, v);
    }
    st.avgMs /= d.size();
    const double ref = nominalMs > 0 ? nominalMs : st.avgMs;
    for (double v : d) {
        st.maxDevMs = std::max(st.maxDevMs, std::abs(v - st.avgMs));
        if (v > ref * 1.5) {
            st.missed++;
        }
    }
    return st;
}

std::string g_vsyncError;

// Приёмник VSync создаётся в Setup() ДО контекста UI (RSUIDirector): после создания
// контекста с соединением к процессу рендеринга CreateVSyncReceiver возвращает nullptr.
void CreateVsyncReceiver()
{
    static std::shared_ptr<AppExecFwk::EventRunner> runner = AppExecFwk::EventRunner::Create(true);
    auto handler = std::make_shared<AppExecFwk::EventHandler>(runner);
    g_receiver = RSInterfaces::GetInstance().CreateVSyncReceiver("rs_probe", handler);
    if (!g_receiver) {
        g_vsyncError = "CreateVSyncReceiver вернул nullptr";
        return;
    }
    int initRet = g_receiver->Init();
    if (initRet != 0) {
        g_vsyncError = "Init() = " + std::to_string(initRet);
        g_receiver.reset();
    }
}

void TestVsync()
{
    if (!g_receiver) {
        Report("VSync: приёмник", false, g_vsyncError);
        return;
    }
    g_fcb.userData_ = nullptr;
    g_fcb.callback_ = OnVsync;
    g_vsyncTs.clear();
    g_vsyncCount = 0;
    g_vsyncRunning = true;
    g_receiver->RequestNextVSync(g_fcb);
    sleep(2);
    g_vsyncRunning = false;
    usleep(50 * 1000); // дать последнему обратному вызову завершиться
    int n = g_vsyncCount.load();
    double avg = 0;
    double jitter = 0;
    {
        std::lock_guard<std::mutex> lock(g_vsyncMu);
        if (g_vsyncTs.size() > 2) {
            std::vector<double> d;
            for (size_t i = 1; i < g_vsyncTs.size(); i++) {
                d.push_back((g_vsyncTs[i] - g_vsyncTs[i - 1]) / 1e6);
            }
            for (double v : d) avg += v;
            avg /= d.size();
            for (double v : d) jitter = std::max(jitter, std::abs(v - avg));
        }
    }
    char note[128];
    snprintf(note, sizeof(note), "%d сигналов за 2 с, период %.2f мс (%.1f Гц), макс. отклонение %.2f мс",
             n, avg, avg > 0 ? 1000.0 / avg : 0.0, jitter);
    Report("VSync", n > 30, note);
}

// Анимация по VSync: каждый сигнал — ровно один шаг квадрата и одна транзакция, как в
// настоящем приложении. Так рывки самого теста исключены, и остаются только рывки вывода.
struct AnimState {
    std::shared_ptr<RSCanvasNode> node;
    int frame = 0;
    int frames = 240;
    float step = 8.0f;
    std::vector<int64_t> ts;
    double flushMax = 0;
    bool done = false;
    std::mutex mu;
    std::condition_variable cv;
};
AnimState *g_anim = nullptr;
VSyncReceiver::FrameCallback g_animFcb;

void OnAnimVsync(int64_t ts, void *)
{
    AnimState &a = *g_anim;
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(a.mu);
        a.ts.push_back(ts);
        // «пинг-понг» между краями экрана: движение без скачка в начало
        const float span = static_cast<float>(g.width - 200);
        float pos = std::fmod(a.frame * a.step, 2 * span);
        float x = pos <= span ? pos : 2 * span - pos;
        a.node->SetBounds(x, 400, 200, 200);
        a.node->SetFrame(x, 400, 200, 200);
        auto tf = Clock::now();
        Flush();
        a.flushMax = std::max(a.flushMax, MsSince(tf));
        a.frame++;
        finished = a.frame >= a.frames;
        a.done = finished;
    }
    if (finished) {
        a.cv.notify_all();
    } else {
        g_receiver->RequestNextVSync(g_animFcb);
    }
}

void TestPerf()
{
    char note[512]; // кириллица — 2 байта на символ
    if (!g_receiver) {
        Report("анимация по VSync", false, "нет приёмника VSync: " + g_vsyncError);
    } else {
        AnimState a;
        a.node = NewCanvas(0, 400, 200, 200, Argb(255, 255, 160, 0));
        g.window->AddChild(a.node, -1);
        Flush();
        usleep(100 * 1000);

        g_anim = &a;
        g_animFcb.userData_ = nullptr;
        g_animFcb.callback_ = OnAnimVsync;
        auto t0 = Clock::now();
        g_receiver->RequestNextVSync(g_animFcb);
        {
            std::unique_lock<std::mutex> lock(a.mu);
            a.cv.wait_for(lock, std::chrono::seconds(10), [&a] { return a.done; });
        }
        double total = MsSince(t0);
        bool completed;
        IntervalStats st;
        {
            std::lock_guard<std::mutex> lock(a.mu);
            completed = a.done;
            st = Intervals(a.ts, 1000.0 / g.refresh);
        }
        g_anim = nullptr;
        snprintf(note, sizeof(note),
                 "%d шагов за %.0f мс (%.1f к/с); VSync: период %.2f мс (экран %u Гц = %.2f мс), "
                 "макс. интервал %.2f мс, пропусков %d; макс. Flush %.2f мс",
                 a.frame, total, a.frame * 1000.0 / total, st.avgMs, g.refresh, 1000.0 / g.refresh,
                 st.maxMs, st.missed, a.flushMax);
        Report("анимация по VSync", completed && st.missed == 0, note);
    }

    // 2. задержка снимка экрана (полный кадр с GPU в память)
    if (g.capture) {
        double lat = 0;
        auto pm = Capture(&lat);
        snprintf(note, sizeof(note), "%.1f мс", lat);
        Report("задержка снимка экрана", pm != nullptr, note);
    }
    ClearWindow();
}

// Диагностика расхождения windows/stress: один узел, меняется по одному параметру —
// цвет, размер, точка проверки. Каждая вариация в чистом окне.
void TestSingle()
{
    struct V {
        const char *name;
        float x, y, w, h;
        int r, gg, b;
        int px, py;
    } vars[] = {
        {"как окно A: 800x500, красный, точка (200,200)", 100, 100, 800, 500, 220, 40, 40, 200, 200},
        {"800x500, серый 60, точка (200,200)", 100, 100, 800, 500, 60, 60, 60, 200, 200},
        {"во весь экран, красный, точка (200,200)", 2, 2, 1916, 1076, 220, 40, 40, 200, 200},
        {"во весь экран, красный, точка (960,540)", 2, 2, 1916, 1076, 220, 40, 40, 960, 540},
        {"во весь экран, серый 60, точка (960,540)", 2, 2, 1916, 1076, 60, 60, 60, 960, 540},
    };
    for (auto &v : vars) {
        auto n = NewCanvas(v.x, v.y, v.w, v.h,
                           Argb(255, static_cast<uint8_t>(v.r), static_cast<uint8_t>(v.gg), static_cast<uint8_t>(v.b)));
        g.window->AddChild(n, -1);
        Flush();
        Hold();
        CheckPixel(v.name, Capture(), v.px, v.py, v.r, v.gg, v.b);
        ClearWindow();
    }
}

// Цвет ячейки сетки: заметно отличается от чёрного фона при любом размере сетки,
// чтобы тест не проходил «случайно» на тёмных ячейках и сетку было видно глазами.
static void CellColor(int c, int r, int &cr, int &cg, int &cb)
{
    cr = 60 + (c * 37) % 190;
    cg = 60 + (r * 53) % 190;
    cb = 60 + ((c + r) * 29) % 190;
}

void TestStress()
{
    // много узлов: сетка grid x grid цветных прямоугольников (по умолчанию 20x20 = 400)
    const int cols = g.grid, rows = g.grid;
    float cw = static_cast<float>(g.width) / cols, ch = static_cast<float>(g.height) / rows;
    auto t0 = Clock::now();
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int cr, cg, cb;
            CellColor(c, r, cr, cg, cb);
            auto n = NewCanvas(c * cw + 2, r * ch + 2, cw - 4, ch - 4,
                               Argb(255, static_cast<uint8_t>(cr), static_cast<uint8_t>(cg), static_cast<uint8_t>(cb)));
            g.window->AddChild(n, -1);
            g_keepAlive.push_back(n);
        }
    }
    Flush();
    double build = MsSince(t0);
    Hold();
    double lat = 0;
    auto pm = Capture(&lat);
    char note[160];
    snprintf(note, sizeof(note), "%d узлов: создание+Flush %.1f мс, снимок %.1f мс", cols * rows, build, lat);
    const std::string name = "нагрузка: " + std::to_string(cols * rows) + " узлов";
    Report(name + ", замеры", true, note);
    // проверяем ячейку в середине сетки — общей проверкой, как в остальных тестах
    const int pc = cols / 4, pr = rows / 3;
    int cx = static_cast<int>(pc * cw + cw / 2), cy = static_cast<int>(pr * ch + ch / 2);
    int er, eg, eb;
    CellColor(pc, pr, er, eg, eb);
    CheckPixel(name + ", ячейка (" + std::to_string(pc) + "," + std::to_string(pr) + ")", pm, cx, cy, er, eg, eb);
    ClearWindow();
}

// ----------------------------------------------------------------------------- инициализация
bool Setup()
{
    auto &rs = RSInterfaces::GetInstance();
    g.screen = rs.GetDefaultScreenId();
    if (g.screen == INVALID_SCREEN_ID) {
        printf("RS не вернул экран по умолчанию — RS запущен?\n");
        return false;
    }
    CreateVsyncReceiver(); // до контекста UI — см. комментарий у функции

    // Контекст UI — через него в версии 7 доступен RSRenderInterface (снимки экрана).
    // Создаётся без токена процесса рендеринга, как у оконного менеджера и ArkUI
    // (RSUIDirector::Create(nullptr)): в однопроцессном режиме RS контекст работает через
    // соединение по умолчанию. Токен (GetConnectToRenderToken) нужен только для отдельного
    // процесса рендеринга многопроцессного режима; с ним клиент получал «hasRemoteObj err».
    g.director = RSUIDirector::Create(nullptr);
    if (!g.director || !g.director->GetRSUIContext()) {
        printf("контекст UI не создан — снимки экрана работать не будут\n");
    } else {
        g.director->SendMessages();
    }

    auto mode = rs.GetScreenActiveMode(g.screen);
    g.width = mode.GetScreenWidth();
    g.height = mode.GetScreenHeight();
    if (mode.GetScreenRefreshRate() > 0) {
        g.refresh = mode.GetScreenRefreshRate();
    }
    if (g.width <= 0 || g.height <= 0) {
        g.width = 1920;
        g.height = 1080;
    }

    RSSurfaceNodeConfig wcfg;
    wcfg.SurfaceNodeName = "rs_probe_window";
    g.window = RSSurfaceNode::Create(wcfg, RSSurfaceNodeType::APP_WINDOW_NODE);
    if (!g.window) {
        printf("не удалось создать окно\n");
        return false;
    }
    g.window->SetBounds(0, 0, g.width, g.height);
    g.window->SetFrame(0, 0, g.width, g.height);
    g.window->SetBackgroundColor(Argb(255, 0, 0, 0));

    if (g.useDisplayNode) {
        // клиентский узел экрана -> окно: RS выводит кадры на дисплей
        RSDisplayNodeConfig dcfg;
        dcfg.screenId = g.screen;
        g.display = RSDisplayNode::Create(dcfg);
        if (!g.display) {
            printf("не удалось создать узел экрана\n");
            return false;
        }
        g.display->SetBounds(0, 0, g.width, g.height);
        g.display->SetFrame(0, 0, g.width, g.height);
        g.display->AddChild(g.window, -1);
    } else {
        // как в демо render_service_client: окно прикрепляется к экрану напрямую. На стенде RS
        // при этом рисует окно (снимки верны), но не выводит его на дисплей: 0 коммитов композитора.
        g.window->AttachToDisplay(g.screen);
    }
    if (g.director) {
        g.director->SetRSSurfaceNode(g.window);
    }
    Flush();
    printf("экран %llu, %dx%d; узел экрана и окно созданы\n",
           static_cast<unsigned long long>(g.screen), g.width, g.height);
    sleep(1);
    return true;
}

void Teardown()
{
    if (g.display) {
        g.display->RemoveFromTree();
    } else if (g.window) {
        g.window->RemoveFromTree();
    }
    Flush();
    if (g_receiver) {
        g_receiver.reset();
    }
}
} // namespace

int main(int argc, char **argv)
{
    std::map<std::string, std::function<void()>> tests = {
        {"screen", TestScreen}, {"colors", TestColors}, {"windows", TestWindows},
        {"shapes", TestShapes}, {"text", TestText}, {"cpubuffer", TestCpuBuffer},
        {"vsync", TestVsync}, {"perf", TestPerf}, {"stress", TestStress}, {"single", TestSingle},
    };
    const std::vector<std::string> order = {"screen", "colors", "windows", "shapes", "text",
                                            "cpubuffer", "vsync", "perf", "stress"};
    std::vector<std::string> selected;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            printf("rs_probe [--hold <с>] [--grid N] [--attach-to-display] [--no-capture] [тест ...]\nтесты:");
            for (auto &t : order) printf(" %s", t.c_str());
            printf("\n");
            return 0;
        } else if (a == "--hold" && i + 1 < argc) {
            g.holdSec = std::max(0, atoi(argv[++i]));
        } else if (a == "--grid" && i + 1 < argc) {
            g.grid = std::max(1, std::min(60, atoi(argv[++i])));
        } else if (a == "--attach-to-display") {
            g.useDisplayNode = false;
        } else if (a == "--no-capture") {
            g.capture = false;
        } else if (tests.count(a)) {
            selected.push_back(a);
        } else {
            printf("неизвестный тест: %s (rs_probe -h)\n", a.c_str());
            return 2;
        }
    }
    if (selected.empty()) {
        selected = order;
    }
    if (!Setup()) {
        return 1;
    }
    for (auto &name : selected) {
        printf("== %s\n", name.c_str());
        tests[name]();
    }
    Teardown();

    int fails = 0;
    for (auto &r : g_results) fails += r.ok ? 0 : 1;
    printf("\nитого: %zu проверок, %d с ошибками\n", g_results.size(), fails);
    for (auto &r : g_results) {
        if (!r.ok) printf("  FAIL %s — %s\n", r.name.c_str(), r.note.c_str());
    }
    return fails ? 3 : 0;
}
