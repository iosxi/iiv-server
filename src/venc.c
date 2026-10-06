/* ==================================================================
 * venc.c - H.264 の符号化(Media Foundation)
 *
 *  GPU のエンコーダ(NVIDIA / Intel / AMD が入れる「ハードウェア MFT」)を
 *  優先して使い、無ければ Windows 標準の CPU のエンコーダ(H264 Encoder MFT)。
 *  どちらも Windows 10 に最初からある仕組み。
 *
 *   GPU  非同期の MFT。D3D11 の NV12 テクスチャをそのまま渡す(GPU から降ろさない)。
 *        イベント(入力が欲しい / 出力がある)は IMFAsyncCallback で受け、Win32 の
 *        イベントで待つ(空回りしない、待つ時間に上限を付けられる)。
 *   CPU  同期の MFT。NV12 を読み出し用のテクスチャへ写して CPU のメモリで渡す。
 *
 *  どちらも低遅延モード(1 枚入れると 1 枚出る)、B フレームなし、キーフレームは
 *  最初と求められたときだけ(GOP は無限)。ビットレートは CBR。
 *  2026-10-06 の実測(RTX 3070 Ti、1920x1080): 入れてから出るまで 2.5〜2.8ms、CPU 4〜7%。
 * ================================================================== */

#include "iiv.h"
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <initguid.h>
#include <codecapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")

typedef struct EvCb {
    IMFAsyncCallbackVtbl *lpVtbl;
    struct VEnc *e;
} EvCb;

struct VEnc {
    IMFTransform           *mft;
    IMFMediaEventGenerator *gen;
    ICodecAPI              *api;
    IMFDXGIDeviceManager   *mgr;
    ID3D11Device           *dev;
    ID3D11DeviceContext    *ctx;
    ID3D11Texture2D        *staging;    /* CPU のエンコーダ: NV12 の読み出し用 */
    BOOL     hw, async;
    int      w, h, kbps, quality;
    DWORD    outCb;                     /* 呼び手が出力の入れ物を用意するときの大きさ */
    BOOL     provides;
    BYTE    *seqHdr;                    /* SPS / PPS(出力に入っていなければキーフレームの前に付ける) */
    int      seqLen;
    /* 非同期のイベント */
    EvCb     cb;
    HANDLE   hEv;
    CRITICAL_SECTION evCs;
    int      needInput, haveOutput;
    BOOL     evError, stopping;
    volatile LONG pendingGet;
};

static volatile LONG g_mfRef;

/* ------------------------------------------------------------------ */
/*  非同期のイベント(IMFAsyncCallback)                                 */
/* ------------------------------------------------------------------ */

static HRESULT STDMETHODCALLTYPE cb_qi(IMFAsyncCallback *t, REFIID riid, void **pp)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFAsyncCallback)) { *pp = t; return S_OK; }
    *pp = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cb_addref(IMFAsyncCallback *t) { (void)t; return 2; }      /* VEnc の中にあり、寿命は VEnc と同じ */
static ULONG STDMETHODCALLTYPE cb_release(IMFAsyncCallback *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE cb_params(IMFAsyncCallback *t, DWORD *f, DWORD *q) { (void)t; (void)f; (void)q; return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE cb_invoke(IMFAsyncCallback *t, IMFAsyncResult *res)
{
    struct VEnc   *e = ((EvCb *)t)->e;
    IMFMediaEvent *ev = NULL;
    MediaEventType type = MEUnknown;
    HRESULT        hr = IMFMediaEventGenerator_EndGetEvent(e->gen, res, &ev);
    InterlockedExchange(&e->pendingGet, 0);
    if (SUCCEEDED(hr) && ev) {
        IMFMediaEvent_GetType(ev, &type);
        IMFMediaEvent_Release(ev);
    }
    EnterCriticalSection(&e->evCs);
    if (FAILED(hr)) e->evError = TRUE;
    else if (type == METransformNeedInput) e->needInput++;
    else if (type == METransformHaveOutput) e->haveOutput++;
    else if (type == MEError) e->evError = TRUE;
    LeaveCriticalSection(&e->evCs);
    if (SUCCEEDED(hr) && !e->stopping && InterlockedExchange(&e->pendingGet, 1) == 0) {
        if (FAILED(IMFMediaEventGenerator_BeginGetEvent(e->gen, (IMFAsyncCallback *)&e->cb, NULL))) {
            InterlockedExchange(&e->pendingGet, 0);
            EnterCriticalSection(&e->evCs);
            e->evError = TRUE;
            LeaveCriticalSection(&e->evCs);
        }
    }
    SetEvent(e->hEv);
    return S_OK;
}

static IMFAsyncCallbackVtbl g_cbVtbl = { cb_qi, cb_addref, cb_release, cb_params, cb_invoke };

/* ------------------------------------------------------------------ */
/*  設定                                                                */
/* ------------------------------------------------------------------ */

static void set_u32(ICodecAPI *api, const GUID *g, ULONG v)
{
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    ICodecAPI_SetValue(api, g, &var);
}

static void set_bool(ICodecAPI *api, const GUID *g, BOOL v)
{
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    ICodecAPI_SetValue(api, g, &var);
}

static void fill_type(IMFMediaType *mt, const GUID *sub, int w, int h, int kbps)
{
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, sub);
    IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_RATE, ((UINT64)60 << 32) | 1);
    IMFMediaType_SetUINT64(mt, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    IMFMediaType_SetUINT32(mt, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT32(mt, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    IMFMediaType_SetUINT32(mt, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    IMFMediaType_SetUINT32(mt, &MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    IMFMediaType_SetUINT32(mt, &MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    if (IsEqualGUID(sub, &MFVideoFormat_H264)) {
        IMFMediaType_SetUINT32(mt, &MF_MT_AVG_BITRATE, (UINT32)(kbps > 0 ? kbps : 20000) * 1000);
        IMFMediaType_SetUINT32(mt, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    }
}

static void read_seq_header(struct VEnc *e)
{
    IMFMediaType *ot = NULL;
    UINT32 n = 0;
    free(e->seqHdr);
    e->seqHdr = NULL;
    e->seqLen = 0;
    if (FAILED(IMFTransform_GetOutputCurrentType(e->mft, 0, &ot))) return;
    if (SUCCEEDED(IMFMediaType_GetBlobSize(ot, &MF_MT_MPEG_SEQUENCE_HEADER, &n)) && n) {
        e->seqHdr = (BYTE *)malloc(n);
        if (e->seqHdr && SUCCEEDED(IMFMediaType_GetBlob(ot, &MF_MT_MPEG_SEQUENCE_HEADER, e->seqHdr, n, NULL))) e->seqLen = (int)n;
    }
    IMFMediaType_Release(ot);
}

/* ビットレートの決め方。
   kbps = 0: 画質一定(CODECAPI_AVEncCommonQuality)。止まった絵ではほとんど送らない。
             NVIDIA の MFT は CODECAPI_AVEncVideoEncodeQP を読み返すと設定した値なのに無視する
             (2026-10-06 実測。16 でも 30 でも同じ大きさ)。画質(0〜100)は効く。
   kbps > 0: 上限付きの可変(相手が回線に合わせて求めたとき) */
static void apply_rate(struct VEnc *e)
{
    if (!e->api) return;
    if (e->kbps <= 0) {
        set_u32(e->api, &CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
        set_u32(e->api, &CODECAPI_AVEncCommonQuality, (ULONG)e->quality);
    } else {
        set_u32(e->api, &CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR);
        set_u32(e->api, &CODECAPI_AVEncCommonMeanBitRate, (ULONG)e->kbps * 1000 / 2);
        set_u32(e->api, &CODECAPI_AVEncCommonMaxBitRate, (ULONG)e->kbps * 1000);
    }
    if (g_cfg.speed >= 0) set_u32(e->api, &CODECAPI_AVEncCommonQualityVsSpeed, (ULONG)g_cfg.speed);
}

void venc_set_quality(VEnc *e, int q)
{
    if (!e || !e->api || q == e->quality) return;
    e->quality = q;
    if (e->kbps <= 0) set_u32(e->api, &CODECAPI_AVEncCommonQuality, (ULONG)q);
}

static BOOL configure(struct VEnc *e)
{
    IMFMediaType *mt = NULL;
    HRESULT hr;
    MFT_OUTPUT_STREAM_INFO si;

    if (SUCCEEDED(IMFTransform_QueryInterface(e->mft, &IID_ICodecAPI, (void **)&e->api))) {
        set_bool(e->api, &CODECAPI_AVLowLatencyMode, TRUE);
        set_u32(e->api, &CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        set_u32(e->api, &CODECAPI_AVEncMPVGOPSize, 0xFFFFFFFF);
        apply_rate(e);
    }
    if (FAILED(MFCreateMediaType(&mt))) return FALSE;
    fill_type(mt, &MFVideoFormat_H264, e->w, e->h, e->kbps);
    hr = IMFTransform_SetOutputType(e->mft, 0, mt, 0);
    IMFMediaType_Release(mt);
    if (FAILED(hr)) { log_printf(L"符号化: 出力の形を決められない (0x%08lX)", hr); return FALSE; }
    if (FAILED(MFCreateMediaType(&mt))) return FALSE;
    fill_type(mt, &MFVideoFormat_NV12, e->w, e->h, e->kbps);
    hr = IMFTransform_SetInputType(e->mft, 0, mt, 0);
    IMFMediaType_Release(mt);
    if (FAILED(hr)) { log_printf(L"符号化: 入力の形を決められない (0x%08lX)", hr); return FALSE; }
    IMFTransform_GetOutputStreamInfo(e->mft, 0, &si);
    e->provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    e->outCb = si.cbSize ? si.cbSize : (DWORD)(e->w * e->h * 2);
    read_seq_header(e);
    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return TRUE;
}

static BOOL try_activate(struct VEnc *e, IMFActivate *a, BOOL hw, WCHAR *name, int nameCap)
{
    WCHAR *fn = NULL;
    UINT32 len = 0;
    IMFAttributes *attr = NULL;

    IMFActivate_GetAllocatedString(a, &MFT_FRIENDLY_NAME_Attribute, &fn, &len);
    if (FAILED(IMFActivate_ActivateObject(a, &IID_IMFTransform, (void **)&e->mft))) { CoTaskMemFree(fn); return FALSE; }
    e->hw = hw;
    e->async = FALSE;
    if (SUCCEEDED(IMFTransform_GetAttributes(e->mft, &attr))) {
        UINT32 isAsync = 0;
        IMFAttributes_GetUINT32(attr, &MF_TRANSFORM_ASYNC, &isAsync);
        if (isAsync) {
            IMFAttributes_SetUINT32(attr, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            e->async = TRUE;
        }
        IMFAttributes_Release(attr);
    }
    if (hw && FAILED(IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)e->mgr))) {
        log_printf(L"符号化: %s に D3D11 を渡せない", fn ? fn : L"?");
        goto fail;
    }
    if (e->async && FAILED(IMFTransform_QueryInterface(e->mft, &IID_IMFMediaEventGenerator, (void **)&e->gen))) goto fail;
    if (!configure(e)) goto fail;
    if (e->async) {
        InterlockedExchange(&e->pendingGet, 1);
        if (FAILED(IMFMediaEventGenerator_BeginGetEvent(e->gen, (IMFAsyncCallback *)&e->cb, NULL))) goto fail;
    }
    lstrcpynW(name, fn ? fn : L"?", nameCap);
    CoTaskMemFree(fn);
    return TRUE;
fail:
    CoTaskMemFree(fn);
    if (e->api) { ICodecAPI_Release(e->api); e->api = NULL; }
    if (e->gen) { IMFMediaEventGenerator_Release(e->gen); e->gen = NULL; }
    IMFActivate_ShutdownObject(a);
    IMFTransform_Release(e->mft);
    e->mft = NULL;
    return FALSE;
}

VEnc *venc_open(void *d3dDevice, int w, int h, int kbps, BOOL allowHw, WCHAR *name, int nameCap)
{
    struct VEnc *e = (struct VEnc *)calloc(1, sizeof(struct VEnc));
    MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate **act = NULL;
    UINT32 n = 0, i;
    UINT token = 0;

    if (!e) return NULL;
    if (InterlockedIncrement(&g_mfRef) == 1) MFStartup(MF_VERSION, MFSTARTUP_LITE);
    e->dev = (ID3D11Device *)d3dDevice;
    ID3D11Device_AddRef(e->dev);
    ID3D11Device_GetImmediateContext(e->dev, &e->ctx);
    e->w = w;
    e->h = h;
    e->kbps = kbps;
    e->quality = g_cfg.qMove;
    e->cb.lpVtbl = &g_cbVtbl;
    e->cb.e = e;
    e->hEv = CreateEventW(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&e->evCs);
    if (FAILED(MFCreateDXGIDeviceManager(&token, &e->mgr)) || FAILED(IMFDXGIDeviceManager_ResetDevice(e->mgr, (IUnknown *)e->dev, token))) {
        venc_close(e);
        return NULL;
    }

    if (allowHw && SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, NULL, &out, &act, &n))) {
        for (i = 0; i < n && !e->mft; i++) try_activate(e, act[i], TRUE, name, nameCap);
        for (i = 0; i < n; i++) IMFActivate_Release(act[i]);
        CoTaskMemFree(act);
        act = NULL;
    }
    if (!e->mft && SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, NULL, &out, &act, &n))) {
        for (i = 0; i < n && !e->mft; i++) try_activate(e, act[i], FALSE, name, nameCap);
        for (i = 0; i < n; i++) IMFActivate_Release(act[i]);
        CoTaskMemFree(act);
    }
    if (!e->mft) {
        log_printf(L"符号化: H.264 のエンコーダが使えない(%dx%d)", w, h);
        venc_close(e);
        return NULL;
    }
    if (!e->hw) {
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(e->dev, &td, NULL, &e->staging))) { venc_close(e); return NULL; }
    }
    log_printf(L"符号化: %s(%s%s)%dx%d、%s %d", name, e->hw ? L"GPU" : L"CPU", e->async ? L"、非同期" : L"", w, h,
               kbps > 0 ? L"上限 kbps" : L"画質", kbps > 0 ? kbps : e->quality);
    return e;
}

BOOL venc_is_hw(VEnc *e) { return e && e->hw; }

void venc_set_kbps(VEnc *e, int kbps)
{
    if (!e || kbps == e->kbps) return;
    e->kbps = kbps;
    apply_rate(e);
    log_printf(L"符号化: %s", kbps > 0 ? L"ビットレートの上限を決めた" : L"画質で決める");
}

/* ------------------------------------------------------------------ */
/*  出力                                                                */
/* ------------------------------------------------------------------ */

static BOOL has_sps(const BYTE *p, int n)
{
    int i;
    for (i = 0; i + 4 < n && i < 256; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1F) == 7) return TRUE;
    return FALSE;
}

/* 出力を 1 つ取り出す。無ければ NULL(*again = TRUE なら、また呼べば取れるかもしれない) */
static VFrame *take_output(struct VEnc *e, BOOL *again)
{
    MFT_OUTPUT_DATA_BUFFER odb;
    DWORD   status = 0;
    HRESULT hr;
    VFrame *f = NULL;

    *again = FALSE;
    ZeroMemory(&odb, sizeof(odb));
    if (!e->provides) {
        IMFMediaBuffer *b = NULL;
        if (FAILED(MFCreateSample(&odb.pSample)) || FAILED(MFCreateMemoryBuffer(e->outCb, &b))) {
            if (odb.pSample) IMFSample_Release(odb.pSample);
            return NULL;
        }
        IMFSample_AddBuffer(odb.pSample, b);
        IMFMediaBuffer_Release(b);
    }
    hr = IMFTransform_ProcessOutput(e->mft, 0, 1, &odb, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        IMFMediaType *ot = NULL;
        if (SUCCEEDED(IMFTransform_GetOutputAvailableType(e->mft, 0, 0, &ot))) {
            IMFTransform_SetOutputType(e->mft, 0, ot, 0);
            IMFMediaType_Release(ot);
        }
        read_seq_header(e);
        *again = TRUE;
    } else if (SUCCEEDED(hr) && odb.pSample) {
        IMFMediaBuffer *b = NULL;
        BYTE  *p;
        DWORD  len = 0;
        UINT32 clean = 0;
        IMFSample_GetUINT32(odb.pSample, &MFSampleExtension_CleanPoint, &clean);
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(odb.pSample, &b)) && SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, &len))) {
            int pre = (clean && e->seqLen && !has_sps(p, (int)len)) ? e->seqLen : 0;
            f = (VFrame *)malloc(sizeof(VFrame) + (size_t)pre + len);
            if (f) {
                ZeroMemory(f, sizeof(VFrame));
                f->ref = 1;
                f->key = clean != 0;
                f->len = pre + (int)len;
                if (pre) memcpy(f->data, e->seqHdr, (size_t)pre);
                memcpy(f->data + pre, p, len);
            }
            IMFMediaBuffer_Unlock(b);
        }
        if (b) IMFMediaBuffer_Release(b);
    }
    if (odb.pSample) IMFSample_Release(odb.pSample);
    if (odb.pEvents) IMFCollection_Release(odb.pEvents);
    return f;
}

/* 非同期: 条件が満たされるまでイベントを待つ。timeout ミリ秒で諦める */
static BOOL wait_event(struct VEnc *e, BOOL forInput, DWORD timeout)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        BOOL ok, err;
        EnterCriticalSection(&e->evCs);
        ok = forInput ? e->needInput > 0 : e->haveOutput > 0;
        err = e->evError;
        LeaveCriticalSection(&e->evCs);
        if (ok) return TRUE;
        if (err) return FALSE;
        {
            DWORD el = GetTickCount() - t0;
            if (el >= timeout) return FALSE;
            WaitForSingleObject(e->hEv, timeout - el);
        }
    }
}

static IMFSample *make_input(struct VEnc *e, ID3D11Texture2D *tex, LONG64 pts)
{
    IMFSample      *s = NULL;
    IMFMediaBuffer *b = NULL;
    DWORD size = (DWORD)(e->w * e->h * 3 / 2);
    if (e->hw) {
        if (FAILED(MFCreateDXGISurfaceBuffer(&IID_ID3D11Texture2D, (IUnknown *)tex, 0, FALSE, &b))) return NULL;
        IMFMediaBuffer_SetCurrentLength(b, size);
    } else {
        D3D11_MAPPED_SUBRESOURCE m;
        BYTE *p;
        int   y;
        ID3D11DeviceContext_CopyResource(e->ctx, (ID3D11Resource *)e->staging, (ID3D11Resource *)tex);
        if (FAILED(ID3D11DeviceContext_Map(e->ctx, (ID3D11Resource *)e->staging, 0, D3D11_MAP_READ, 0, &m))) return NULL;
        if (FAILED(MFCreateMemoryBuffer(size, &b)) || FAILED(IMFMediaBuffer_Lock(b, &p, NULL, NULL))) {
            ID3D11DeviceContext_Unmap(e->ctx, (ID3D11Resource *)e->staging, 0);
            if (b) IMFMediaBuffer_Release(b);
            return NULL;
        }
        for (y = 0; y < e->h; y++) memcpy(p + (size_t)y * e->w, (BYTE *)m.pData + (size_t)y * m.RowPitch, (size_t)e->w);
        for (y = 0; y < e->h / 2; y++)
            memcpy(p + (size_t)e->w * e->h + (size_t)y * e->w, (BYTE *)m.pData + (size_t)(e->h + y) * m.RowPitch, (size_t)e->w);
        IMFMediaBuffer_Unlock(b);
        ID3D11DeviceContext_Unmap(e->ctx, (ID3D11Resource *)e->staging, 0);
        IMFMediaBuffer_SetCurrentLength(b, size);
    }
    if (FAILED(MFCreateSample(&s))) { IMFMediaBuffer_Release(b); return NULL; }
    IMFSample_AddBuffer(s, b);
    IMFMediaBuffer_Release(b);
    IMFSample_SetSampleTime(s, pts);
    IMFSample_SetSampleDuration(s, 166667);
    return s;
}

VFrame *venc_encode(VEnc *e, void *nv12Texture, BOOL forceKey, LONG64 pts100ns)
{
    IMFSample *s;
    VFrame    *f = NULL;
    BOOL       again;
    HRESULT    hr;

    if (!e || !e->mft) return NULL;
    if (forceKey && e->api) set_u32(e->api, &CODECAPI_AVEncVideoForceKeyFrame, 1);
    s = make_input(e, (ID3D11Texture2D *)nv12Texture, pts100ns);
    if (!s) return NULL;

    if (e->async) {
        if (!wait_event(e, TRUE, 500)) { log_printf(L"符号化: 入力を受け付けない"); IMFSample_Release(s); return NULL; }
        EnterCriticalSection(&e->evCs);
        e->needInput--;
        LeaveCriticalSection(&e->evCs);
        hr = IMFTransform_ProcessInput(e->mft, 0, s, 0);
        IMFSample_Release(s);
        if (FAILED(hr)) { log_printf(L"符号化: ProcessInput 0x%08lX", hr); return NULL; }
        /* 低遅延なので、ふつうはすぐ出てくる。出てこなければ次の呼び出しで取る */
        while (!f && wait_event(e, FALSE, 200)) {
            EnterCriticalSection(&e->evCs);
            e->haveOutput--;
            LeaveCriticalSection(&e->evCs);
            f = take_output(e, &again);
        }
    } else {
        hr = IMFTransform_ProcessInput(e->mft, 0, s, 0);
        IMFSample_Release(s);
        if (FAILED(hr)) { log_printf(L"符号化: ProcessInput 0x%08lX", hr); return NULL; }
        do { f = take_output(e, &again); } while (!f && again);
    }
    return f;
}

void venc_close(VEnc *e)
{
    if (!e) return;
    e->stopping = TRUE;
    if (e->mft) {
        IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    if (e->gen) {
        /* 出したままの BeginGetEvent が終わるのを少し待つ(MFT を壊すと Invoke が失敗で返る) */
        IMFShutdown *sh = NULL;
        if (SUCCEEDED(IMFTransform_QueryInterface(e->mft, &IID_IMFShutdown, (void **)&sh))) {
            IMFShutdown_Shutdown(sh);
            IMFShutdown_Release(sh);
        }
        {
            int i;
            for (i = 0; i < 50 && e->pendingGet; i++) Sleep(10);
        }
        IMFMediaEventGenerator_Release(e->gen);
    }
    if (e->api) ICodecAPI_Release(e->api);
    if (e->mft) IMFTransform_Release(e->mft);
    if (e->mgr) IMFDXGIDeviceManager_Release(e->mgr);
    if (e->staging) ID3D11Texture2D_Release(e->staging);
    if (e->ctx) ID3D11DeviceContext_Release(e->ctx);
    if (e->dev) ID3D11Device_Release(e->dev);
    if (e->hEv) CloseHandle(e->hEv);
    DeleteCriticalSection(&e->evCs);
    free(e->seqHdr);
    free(e);
    if (InterlockedDecrement(&g_mfRef) == 0) MFShutdown();
}
