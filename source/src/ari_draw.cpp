// The ARI glasses hint pictures (ari_draw.h): white text with a dark outline on a soft slate pill, a white line with a
// dark edge and an arrow head in the panel's origami tan, all on a transparent background.
#include "ari_draw.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <wincodec.h>

#include <cmath>
#include <cstring>

#include "log.h"

extern "C" HRESULT WINAPI WICCreateImagingFactory_Proxy(UINT SDKVersion, IWICImagingFactory** ppIWICImagingFactory);

namespace hrvr::ari {
namespace {
uint32_t g_pics[kHintPictures][kHW * kHH];
int g_state = 0;  // 1 drawn, -1 failed
ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dw = nullptr;
IDWriteFontCollection* g_fonts = nullptr;
IWICImagingFactory* g_wic = nullptr;
IWICBitmap* g_bmp = nullptr;
ID2D1RenderTarget* g_rt = nullptr;

const void* FontRes(int id, DWORD* size) {
    HMODULE mod = nullptr;
    *size = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&FontRes), &mod))
        return nullptr;
    HRSRC r = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL g = r ? LoadResource(mod, r) : nullptr;
    *size = r ? SizeofResource(mod, r) : 0;
    return g ? LockResource(g) : nullptr;
}

void LoadFonts() {  // the built-in Montserrat (RCDATA 3101..3103), as the welcome windows
    IDWriteFactory5* f5 = nullptr;
    if (FAILED(g_dw->QueryInterface(IID_PPV_ARGS(&f5)))) return;
    IDWriteInMemoryFontFileLoader* ld = nullptr;
    IDWriteFontSetBuilder1* b = nullptr;
    if (SUCCEEDED(f5->CreateInMemoryFontFileLoader(&ld)) && SUCCEEDED(f5->RegisterFontFileLoader(ld)) &&
        SUCCEEDED(f5->CreateFontSetBuilder(&b))) {
        int n = 0;
        for (int id = 3101; id <= 3103; ++id) {
            DWORD sz = 0;
            const void* p = FontRes(id, &sz);
            IDWriteFontFile* ff = nullptr;
            if (p && SUCCEEDED(ld->CreateInMemoryFontFileReference(f5, p, sz, nullptr, &ff))) {
                if (SUCCEEDED(b->AddFontFile(ff))) ++n;
                ff->Release();
            }
        }
        IDWriteFontSet* set = nullptr;
        IDWriteFontCollection1* c = nullptr;
        if (n == 3 && SUCCEEDED(b->CreateFontSet(&set)) && SUCCEEDED(f5->CreateFontCollectionFromFontSet(set, &c)))
            g_fonts = c;
        if (set) set->Release();
    }
    if (b) b->Release();
    f5->Release();
}

ID2D1SolidColorBrush* Brush(float r, float g, float b, float a) {
    ID2D1SolidColorBrush* br = nullptr;
    g_rt->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &br);
    return br;
}

// The label: a rounded slate pill, the words in white with a dark outline (eight offsets), centred in the box.
void Label(const wchar_t* s, D2D1_RECT_F box) {
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(g_fonts ? L"Montserrat" : L"Segoe UI", g_fonts, DWRITE_FONT_WEIGHT_BOLD,
                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 56.f, L"en-us", &f);
    if (!f) return;
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    IDWriteTextLayout* l = nullptr;
    if (SUCCEEDED(g_dw->CreateTextLayout(s, UINT32(wcslen(s)), f, box.right - box.left, box.bottom - box.top, &l))) {
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        const float cx = 0.5f * (box.left + box.right), cy = 0.5f * (box.top + box.bottom);
        const D2D1_ROUNDED_RECT pill{{cx - m.width * 0.5f - 28.f, cy - m.height * 0.5f - 14.f, cx + m.width * 0.5f + 28.f,
                                      cy + m.height * 0.5f + 14.f},
                                     22.f, 22.f};
        if (ID2D1SolidColorBrush* bg = Brush(0.10f, 0.11f, 0.13f, 0.72f)) {
            g_rt->FillRoundedRectangle(pill, bg);
            bg->Release();
        }
        if (ID2D1SolidColorBrush* edge = Brush(150 / 255.f, 118 / 255.f, 76 / 255.f, 0.95f)) {
            g_rt->DrawRoundedRectangle(pill, edge, 3.f);
            edge->Release();
        }
        if (ID2D1SolidColorBrush* dark = Brush(0.f, 0.f, 0.f, 0.85f)) {
            for (int k = 0; k < 8; ++k) {
                const float a = k * 0.785398f;
                g_rt->DrawTextLayout({box.left + 3.f * std::cos(a), box.top + 3.f * std::sin(a)}, l, dark);
            }
            dark->Release();
        }
        if (ID2D1SolidColorBrush* white = Brush(1.f, 1.f, 1.f, 1.f)) {
            g_rt->DrawTextLayout({box.left, box.top}, l, white);
            white->Release();
        }
        l->Release();
    }
    f->Release();
}

// A vertical line x = 320 from y0 to y1 ending in an arrow head whose tip is at y1 (down when y1 > y0).
void Arrow(float y0, float y1) {
    const float x = kHW * 0.5f, dir = y1 > y0 ? 1.f : -1.f, head = 64.f;
    const float base = y1 - dir * head;
    ID2D1PathGeometry* tri = nullptr;
    if (SUCCEEDED(g_d2d->CreatePathGeometry(&tri))) {
        ID2D1GeometrySink* sk = nullptr;
        if (SUCCEEDED(tri->Open(&sk))) {
            sk->BeginFigure({x, y1}, D2D1_FIGURE_BEGIN_FILLED);
            sk->AddLine({x - 40.f, base});
            sk->AddLine({x + 40.f, base});
            sk->EndFigure(D2D1_FIGURE_END_CLOSED);
            sk->Close();
            sk->Release();
        }
    }
    ID2D1SolidColorBrush* dark = Brush(0.f, 0.f, 0.f, 0.8f);
    ID2D1SolidColorBrush* white = Brush(1.f, 1.f, 1.f, 1.f);
    ID2D1SolidColorBrush* tan = Brush(214 / 255.f, 178 / 255.f, 124 / 255.f, 1.f);
    if (dark && white && tan) {
        g_rt->DrawLine({x, y0}, {x, base}, dark, 16.f, nullptr);
        g_rt->DrawLine({x, y0}, {x, base}, white, 9.f, nullptr);
        if (tri) {
            g_rt->DrawGeometry(tri, dark, 6.f);
            g_rt->FillGeometry(tri, tan);
        }
    }
    ID2D1SolidColorBrush* brushes[3] = {dark, white, tan};
    for (ID2D1SolidColorBrush* b : brushes)
        if (b) b->Release();
    if (tri) tri->Release();
}

bool Draw() {
    if (g_state) return g_state > 0;
    g_state = -1;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw))) ||
        FAILED(WICCreateImagingFactory_Proxy(WINCODEC_SDK_VERSION, &g_wic)) ||
        FAILED(g_wic->CreateBitmap(kHW, kHH, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &g_bmp))) {
        Log("ARI hint: Direct2D / WIC not available - no hint pictures");
        return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(g_bmp, rp, &g_rt))) {
        Log("ARI hint: no render target - no hint pictures");
        return false;
    }
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    LoadFonts();
    // label on top + arrow down (tip = bottom edge), or arrow up (tip = top edge) + label under it
    struct Pic {
        const wchar_t* text;
        bool arrowUp;
    } const pics[kHintPictures] = {{L"Grab AR Glasses", false},
                                   {L"Put on AR Glasses", true},
                                   {L"Grab AR Glasses", true},
                                   {L"Take off AR Glasses", false}};
    for (int pic = 0; pic < kHintPictures; ++pic) {
        g_rt->BeginDraw();
        g_rt->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
        if (!pics[pic].arrowUp) {
            Label(pics[pic].text, {0.f, 8.f, float(kHW), 120.f});
            Arrow(132.f, float(kHH) - 6.f);
        } else {
            Arrow(352.f, 6.f);
            Label(pics[pic].text, {0.f, 360.f, float(kHW), 472.f});
        }
        if (FAILED(g_rt->EndDraw())) return false;
        WICRect r{0, 0, kHW, kHH};
        IWICBitmapLock* lk = nullptr;
        if (FAILED(g_bmp->Lock(&r, WICBitmapLockRead, &lk))) return false;
        UINT stride = 0, sz = 0;
        BYTE* p = nullptr;
        lk->GetStride(&stride);
        lk->GetDataPointer(&sz, &p);
        for (int y = 0; y < kHH && p; ++y) std::memcpy(g_pics[pic] + size_t(y) * kHW, p + size_t(y) * stride, size_t(kHW) * 4);
        lk->Release();
    }
    Log("ARI hint pictures drawn (%dx%d, font %s)", kHW, kHH, g_fonts ? "Montserrat (built in)" : "Segoe UI");
    g_state = 1;
    return true;
}
}  // namespace

const uint32_t* HintPixels(int picture) {
    if (picture < 0 || picture >= kHintPictures || !Draw()) return nullptr;
    return g_pics[picture];
}

}  // namespace hrvr::ari
